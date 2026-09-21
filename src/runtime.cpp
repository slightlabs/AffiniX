#include "afx/runtime.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#ifdef __linux__
#include <sys/signalfd.h>
#endif
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "afx/sys/clock.hpp"
#include "afx/sys/crash_dump.hpp"
#include "afx/sys/log.hpp"
#include "afx/sys/numa.hpp"

namespace afx {

#ifndef __linux__
namespace {
// macOS/BSD have no signalfd: the signal thread installs plain handlers that
// write the signal number onto the self-pipe (async-signal-safe write only;
// user callbacks still run on the dedicated thread, never in the handler).
std::atomic<int> g_sig_pipe_w{-1};
void sig_to_pipe(int sig) noexcept {
    int w = g_sig_pipe_w.load(std::memory_order_relaxed);
    if (w >= 0) {
        char b = char(sig);
        ssize_t ignored = ::write(w, &b, 1);
        (void)ignored;
    }
}
}  // namespace
#endif

Runtime::~Runtime() {
    shutdown(5s);
    join();
}

Runtime::Group Runtime::spawn_group(std::string name, std::size_t n,
                                    ThreadConfig cfg) {
    std::size_t begin = shards_.size();
    for (std::size_t i = 0; i < n; ++i) {
        auto s = std::make_unique<Shard>();
        s->name = name + "-" + std::to_string(i);
        s->cfg = cfg;
        s->cfg.em.name = s->name;
        shards_.push_back(std::move(s));
    }
    return Group(this, begin, n);
}

Runtime::Group& Runtime::Group::each(std::function<void(EventManager&)> fn) {
    for (std::size_t i = begin_; i < begin_ + count_; ++i)
        rt_->shards_[i]->setup.push_back(fn);
    return *this;
}

std::vector<Mailbox> Runtime::Group::mailboxes() const {
    std::vector<Mailbox> out;
    for (std::size_t i = begin_; i < begin_ + count_; ++i) {
        // Spin briefly until the thread has published its EM.
        auto& s = rt_->shards_[i];
        while (!s->ready.load(std::memory_order_acquire))
            std::this_thread::yield();
        if (EventManager* e = s->em.load()) out.push_back(e->mailbox());
    }
    return out;
}

std::vector<Mailbox> Runtime::mailboxes() const {
    std::vector<Mailbox> out;
    for (auto& s : shards_)
        if (EventManager* e = s->em.load()) out.push_back(e->mailbox());
    return out;
}

EventManager* Runtime::em(std::size_t i) const {
    return i < shards_.size() ? shards_[i]->em.load() : nullptr;
}

Runtime::ShardInfo Runtime::shard_info(std::size_t i) const {
    ShardInfo out;
    if (i >= shards_.size()) return out;
    const Shard& s = *shards_[i];
    out.name = s.name;
    out.requested_core = s.requested_core;
    out.bound_core = s.bound_core.load(std::memory_order_relaxed);
    out.numa_node = s.numa_node.load(std::memory_order_relaxed);
    out.running = s.em.load(std::memory_order_acquire) != nullptr;
    out.failed = s.failed.load(std::memory_order_relaxed);
    return out;
}

void Runtime::on_signal(std::vector<int> sigs, std::function<void()> fn) {
    for (int s : sigs) signal_handlers_.emplace_back(s, fn);
}

void Runtime::start() {
    if (started_) return;
    started_ = true;

    // Block handled signals in this thread BEFORE spawning children so they
    // inherit the mask (§20).
    sigset_t mask;
    sigemptyset(&mask);
    for (auto& [s, _] : signal_handlers_) sigaddset(&mask, s);
    if (!signal_handlers_.empty()) ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);

    // M7-08: flight-recorder dump on fault — the handler only touches the
    // registered rings and write(2) (§21.1).
    install_crash_dump();

    // Resolve placement per shard.
    auto phys = topo_.physical_cores();
    std::size_t phys_i = 0;

    for (std::size_t i = 0; i < shards_.size(); ++i) {
        Shard& s = *shards_[i];
        int core = -1;
        switch (s.cfg.placement) {
            case Placement::OnePerPhysicalCore:
                if (!phys.empty()) core = phys[phys_i++ % phys.size()];
                break;
            case Placement::OnePerLogicalCore:
                if (!topo_.empty()) core = topo_.cores()[i % topo_.size()].id;
                break;
            case Placement::Explicit: {
                auto cpus = s.cfg.cores.cpus();
                if (!cpus.empty()) core = cpus[i % cpus.size()];
                break;
            }
            case Placement::None:
                break;
        }
        s.requested_core = core;
        s.thread = std::thread([this, &s, core] { thread_main(s, core); });
    }

    if (!signal_handlers_.empty()) {
        // self-pipe created before the thread so shutdown() can always wake it
        if (::pipe(signal_selfpipe_) != 0) {
            signal_selfpipe_[0] = signal_selfpipe_[1] = -1;
            return;  // no wake path — signal thread can't run safely
        }
        ::fcntl(signal_selfpipe_[0], F_SETFL, O_NONBLOCK);
        ::fcntl(signal_selfpipe_[1], F_SETFL, O_NONBLOCK);
        signal_thread_ = std::thread([this] { signal_thread_main(); });
    }
}

void Runtime::thread_main(Shard& s, int core) {
    if (core >= 0) {
        CoreSet one;
        one.add(core);
        if (auto r = pin_this_thread(one); !r) {
            // Platforms without hard pinning (macOS/BSD) degrade to unpinned
            // with a warning; a real pinning failure stays fatal.
            if (r.error() ==
                make_error(ErrorCategory::Config, Err::Unsupported)) {
                AFX_LOG(LogLevel::Warn,
                        "afx: shard '%s' requested core %d but pinning "
                        "is unsupported here — running unpinned\n",
                        s.name.c_str(), core);
                core = -1;
            } else {
                s.start_error = r.error();
                s.failed = true;
                s.ready = true;  // publish so mailboxes() doesn't hang
                return;
            }
        }
    }
    if (auto r = apply_sched_this_thread(s.cfg.sched); !r) {
        // Only fatal for Fifo/Rr; Other failing is impossible in practice.
        if (!std::holds_alternative<SchedPolicy::Other>(s.cfg.sched.v)) {
            s.start_error = r.error();
            s.failed = true;
            s.ready = true;
            return;
        }
    }

    EventManager em(std::move(s.cfg.em));

    // M5-07: placement is logged once at startup, always (§19). The line
    // names shard, core, NUMA node and wait strategy so a misplacement is
    // discoverable from stderr without attaching a profiler.
    int node = core >= 0 ? topo_.numa_node_of(core) : numa::current_node();
    s.bound_core.store(core, std::memory_order_relaxed);
    s.numa_node.store(node, std::memory_order_relaxed);
    const char* wait = s.cfg.em.wait == WaitStrategy::Block ? "block"
                       : s.cfg.em.wait == WaitStrategy::SpinThenBlock
                           ? "spin-then-block"
                           : "spin";
    if (core >= 0)
        AFX_LOG(LogLevel::Info,
                "afx: shard '%s' -> core %d (numa %d) wait=%s\n",
                s.name.c_str(), core, node, wait);
    else
        AFX_LOG(LogLevel::Info,
                "afx: shard '%s' -> unpinned (numa %d) wait=%s\n",
                s.name.c_str(), node, wait);

    // M5-06: a shard bound to a different NUMA node than its NIC pays for
    // every packet in cross-node traffic. Warn loudly rather than silently
    // accept it (§19).
    if (!s.cfg.nic_ifname.empty()) {
        if (auto nic_node = topo_.numa_node_of_nic(s.cfg.nic_ifname)) {
            if (node >= 0 && *nic_node >= 0 && *nic_node != node)
                AFX_LOG(LogLevel::Warn,
                        "afx: WARNING shard '%s' on numa %d but NIC '%s' "
                        "is on numa %d\n",
                        s.name.c_str(), node, s.cfg.nic_ifname.c_str(),
                        *nic_node);
        }
    }

    s.em.store(&em, std::memory_order_release);
    s.ready.store(true, std::memory_order_release);

    for (auto& f : s.setup) f(em);
    em.run();
    s.em.store(nullptr, std::memory_order_release);
}

// Signal handling on a designated thread via signalfd on Linux, self-pipe +
// handler elsewhere (§20).
void Runtime::signal_thread_main() {
    sigset_t mask;
    sigemptyset(&mask);
    for (auto& [s, _] : signal_handlers_) sigaddset(&mask, s);
#ifdef __linux__
    int sfd = ::signalfd(-1, &mask, SFD_CLOEXEC);
    if (sfd < 0) return;

    while (!signal_stop_.load(std::memory_order_acquire)) {
        pollfd pfds[2] = {{sfd, POLLIN, 0}, {signal_selfpipe_[0], POLLIN, 0}};
        int r = ::poll(pfds, 2, -1);
        if (r <= 0) continue;
        if (pfds[1].revents) break;
        if (pfds[0].revents) {
            signalfd_siginfo si{};
            if (::read(sfd, &si, sizeof(si)) != sizeof(si)) continue;
            for (auto& [sig, fn] : signal_handlers_)
                if (int(si.ssi_signo) == sig) fn();
        }
    }
    ::close(sfd);
#else
    // Publish the pipe, install byte-forwarding handlers, then unblock the
    // mask — process-directed signals land on this thread only.
    g_sig_pipe_w.store(signal_selfpipe_[1], std::memory_order_release);
    struct sigaction sa {};
    sa.sa_handler = &sig_to_pipe;
    sigemptyset(&sa.sa_mask);
    for (auto& [s, _] : signal_handlers_) ::sigaction(s, &sa, nullptr);
    ::pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);

    bool stop = false;
    while (!stop) {
        pollfd pfd{signal_selfpipe_[0], POLLIN, 0};
        if (::poll(&pfd, 1, -1) <= 0) continue;
        char buf[64];
        ssize_t r = ::read(signal_selfpipe_[0], buf, sizeof(buf));
        if (r <= 0) break;
        for (ssize_t i = 0; i < r && !stop; ++i) {
            if (buf[i] == 0) {
                stop = true;  // shutdown() sentinel
                break;
            }
            for (auto& [sig, fn] : signal_handlers_)
                if (int(buf[i]) == sig) fn();
        }
    }
    g_sig_pipe_w.store(-1, std::memory_order_release);
#endif
    ::close(signal_selfpipe_[0]);
    ::close(signal_selfpipe_[1]);
}

void Runtime::shutdown(Duration timeout) {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true)) return;
    // §20 drain sequence, run on each EM's own thread via its mailbox:
    // listeners close -> apps notified -> writes drain until the deadline ->
    // shutdown_write -> loop stops and destructors hard-close the rest.
    // EMs that are not running fall back to a plain stop() (they will run
    // their destructor teardown when the thread unwinds).
    TimePoint deadline = SteadyClock{}.now() + timeout;
    for (auto& s : shards_) {
        EventManager* e = s->em.load(std::memory_order_acquire);
        if (!e) continue;
        if (e->is_running() && e->post([e, deadline] {
                e->begin_shutdown(deadline);
            }) == PostResult::Ok)
            continue;
        e->stop();
    }
    signal_stop_.store(true, std::memory_order_release);
    if (signal_selfpipe_[1] >= 0) {
        // Byte 0 is the stop sentinel on the self-pipe signal path (no
        // valid signo is 0); on Linux any byte suffices — revents is enough.
        char b = 0;
        ssize_t ignored = ::write(signal_selfpipe_[1], &b, 1);
        (void)ignored;
    }
}

void Runtime::join() {
    for (auto& s : shards_)
        if (s->thread.joinable()) s->thread.join();
    if (signal_thread_.joinable()) signal_thread_.join();
}

}  // namespace afx

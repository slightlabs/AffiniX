#include "afx/runtime.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "afx/sys/clock.hpp"
#include "afx/sys/crash_dump.hpp"
#include "afx/sys/numa.hpp"

namespace afx {

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
            s.start_error = r.error();
            s.failed = true;
            s.ready = true;  // publish so mailboxes() doesn't hang
            return;
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
    const char* wait = s.cfg.em.wait == WaitStrategy::Block       ? "block"
                       : s.cfg.em.wait == WaitStrategy::SpinThenBlock
                           ? "spin-then-block"
                           : "spin";
    if (core >= 0)
        std::fprintf(stderr, "afx: shard '%s' -> core %d (numa %d) wait=%s\n",
                     s.name.c_str(), core, node, wait);
    else
        std::fprintf(stderr, "afx: shard '%s' -> unpinned (numa %d) wait=%s\n",
                     s.name.c_str(), node, wait);

    // M5-06: a shard bound to a different NUMA node than its NIC pays for
    // every packet in cross-node traffic. Warn loudly rather than silently
    // accept it (§19).
    if (!s.cfg.nic_ifname.empty()) {
        if (auto nic_node = topo_.numa_node_of_nic(s.cfg.nic_ifname)) {
            if (node >= 0 && *nic_node >= 0 && *nic_node != node)
                std::fprintf(stderr,
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

// Signal handling on a designated thread via signalfd (§20).
void Runtime::signal_thread_main() {
    sigset_t mask;
    sigemptyset(&mask);
    for (auto& [s, _] : signal_handlers_) sigaddset(&mask, s);
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
        if (e->is_running() &&
            e->post([e, deadline] { e->begin_shutdown(deadline); }) ==
                PostResult::Ok)
            continue;
        e->stop();
    }
    signal_stop_.store(true, std::memory_order_release);
    if (signal_selfpipe_[1] >= 0) {
        char b = 1;
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

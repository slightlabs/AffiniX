#pragma once

// net/dns.hpp — async DNS resolver (M11-03, DESIGN.md §28.2).
//
// getaddrinfo(3) is a blocking syscall; running it on a loop thread is the
// classic production stall. Resolver owns one dedicated EventManager on its
// own thread: requests arrive via that EM's mailbox, getaddrinfo runs
// there (serially — DNS is not a hot path), and the result is posted back
// to the *requester's* mailbox.
//
// Deadline-aware (§7.4): resolve() arms a timer on the requester's wheel.
// Whichever side reaches the shared state first wins — the reply delivers
// the address list, or the timer delivers Err::Expired. Exactly one
// callback invocation is guaranteed; a reply racing an expired request is
// dropped. The worker also skips jobs whose deadline already passed, so a
// backlog of stale lookups never burns resolve time.

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <netdb.h>

#include "afx/core/event_manager.hpp"  // DefaultPollBackend
#include "afx/itc/mailbox.hpp"
#include "afx/net/sock_addr.hpp"
#include "afx/sys/inline_fn.hpp"
#include "afx/sys/result.hpp"

namespace afx {

class Resolver {
  public:
    using AddrList = std::vector<SockAddr>;
    using Cb = InlineFn<void(Result<AddrList>), 64>;

    Resolver();
    ~Resolver();
    Resolver(const Resolver&) = delete;
    Resolver& operator=(const Resolver&) = delete;

    // EM-affine: call on `em`'s loop thread. `cb` fires exactly once on that
    // thread — with the resolved addresses, Err::ResolveFailed, or
    // Err::Expired when `timeout` elapses first. family is AF_UNSPEC,
    // AF_INET, or AF_INET6 (Happy Eyeballs asks for both and interleaves).
    template <class EM>
    void resolve(std::string host, std::uint16_t port, EM& em, Duration timeout,
                 Cb cb, int family = AF_UNSPEC) {
        auto st = std::make_shared<State>();
        st->cb = std::move(cb);
        if (timeout.count() > 0) {
            EM* ep = &em;
            st->timer = em.after(timeout, [st, ep](TimerCtx) {
                if (!st->done.exchange(true) && st->cb)
                    st->cb(make_error(ErrorCategory::Cancelled, Err::Expired));
            });
            st->cancel = [ep, tid = st->timer] { (void)ep->cancel(tid); };
        }
        Job j;
        j.host = std::move(host);
        j.port = port;
        j.family = family;
        j.reply_to = em.mailbox();
        j.st = std::move(st);
        j.deadline = timeout.count() ? em.now() + timeout : TimePoint::max();
        (void)em_.mailbox().post(
            [this, j = std::move(j)]() mutable { run_job(std::move(j)); });
    }

    // Test hook: sleep this long inside the worker before each lookup —
    // exercises the requester-side deadline against a deliberately slow
    // resolver (M11 exit criterion) without depending on external DNS.
    void set_testing_delay(Duration d) noexcept {
        delay_.store(d, std::memory_order_relaxed);
    }

  private:
    struct State {
        std::atomic<bool> done{false};
        TimerId timer{};
        InlineFn<void(), 48> cancel;  // requester-side timer disarm
        Cb cb;
    };
    struct Job {
        std::string host;
        std::uint16_t port = 0;
        int family = AF_UNSPEC;
        Mailbox reply_to;
        std::shared_ptr<State> st;
        TimePoint deadline{};
    };

    void run_job(Job&& j);

    // Dedicated readiness EM for the resolver thread — the platform poll
    // backend (kqueue/epoll), never uring: resolver ops are all blocking
    // getaddrinfo + a wake pipe, nothing a proactor ring accelerates.
    using ResolverEM = BasicEventManager<SteadyClock, DefaultPollBackend>;
    ResolverEM em_;
    std::atomic<Duration> delay_{Duration::zero()};
    std::thread thread_;
};

}  // namespace afx

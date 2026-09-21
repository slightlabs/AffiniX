#pragma once

// TcpClient — DESIGN.md §15.2. Connect with a deadline, reconnect with
// jittered exponential backoff, connection-state observability. Hostname
// resolution is synchronous here only for numeric addresses; name resolution
// is done once at construction (before the loop runs in the common case) —
// the async resolver is a §28.2 extension.

#include <netdb.h>
#include <algorithm>
#include <random>

#include "afx/core/pool.hpp"
#include "afx/net/connection.hpp"
#include "afx/net/dns.hpp"
#include "afx/net/socket.hpp"

namespace afx {

enum class ClientState : std::uint8_t {
    Disconnected,
    Connecting,
    Connected,
    ReconnectWait,
};

struct Backoff {
    Duration initial = 100ms;
    Duration max = 30s;
    double jitter = 0.2;
    Duration current = Duration::zero();  // runtime cursor

    Duration next() {
        Duration d = current.count() ? current : initial;
        current = std::min(d * 2, max);
        return d;
    }
    void reset() { current = Duration::zero(); }
};

struct ClientConfig {
    Endpoint target{};
    Duration connect_timeout = 5s;
    Backoff reconnect{};
    bool auto_reconnect = true;
    // M11-04 Happy Eyeballs (RFC 8305): with >1 resolved address, launch
    // attempts staggered by he_stagger, v6 first — first connect wins, the
    // rest are torn down quietly. Falls back to sequential failover when
    // off or a single address resolved.
    bool happy_eyeballs = true;
    Duration he_stagger = 250ms;
    // M11-03 async resolver. Set it for DNS names — without one a hostname
    // resolves via blocking getaddrinfo in start(), which stalls the EM.
    Resolver* resolver = nullptr;
    // M11-05: connect to a Unix-domain path instead of host:port. Resolver
    // and Happy Eyeballs are bypassed — a unix target is a single SockAddr.
    std::optional<SockAddr> unix_target;
    // M11-06: harvest SCM_RIGHTS descriptors on this conn (the receiving
    // side of a hot-restart hand-off). Meaningful for unix_target conns.
    bool fd_passing = false;
    std::optional<SockAddr> bind_local;
    SocketOptions sock{};
    FlowControl flow{};
};

template <class P, class EM>
class TcpClient {
  public:
    using Conn = Connection<P, EM>;
    using StateFn = InlineFn<void(ClientState), 48>;

    TcpClient(EM& em, ClientConfig cfg, Handlers<P> handlers)
        : em_(&em),
          cfg_(std::move(cfg)),
          handlers_(std::move(handlers)),
          // A client holds one connection at a time; a small pool covers
          // reconnect churn without per-connect heap traffic (M6-02).
          pool_(&em.arena(), 4) {}

    ~TcpClient() {
        // Detach the attempt lists first: close() notifies on_conn_gone
        // synchronously when the EM has stopped, and reclaim_attempt must
        // not double-destroy what we destroy below.
        auto pend = std::move(pending_);
        auto clos = std::move(closing_);
        pending_.clear();
        closing_.clear();
        for (auto* c : pend) {
            c->close(CloseReason::Shutdown);
            pool_.destroy(c);
        }
        for (auto* c : clos) pool_.destroy(c);  // already torn down
        if (conn_) {
            conn_->close(CloseReason::Shutdown);
            pool_.destroy(conn_);
            conn_ = nullptr;
        }
        if (group_.valid()) em_->cancel_group(group_);
        if (he_group_.valid()) em_->cancel_group(he_group_);
    }

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    Result<void> start() {
        // Observe Connected via the user's on_open path.
        auto user_open = std::move(handlers_.on_open);
        handlers_.on_open = [this, u = std::move(user_open)](ConnId id,
                                                             Peer p) mutable {
            on_connected();
            if (u) u(id, p);
        };
        group_ = em_->make_timer_group();
        // Numeric targets resolve synchronously; names go through the async
        // resolver when one is configured (blocking getaddrinfo on an EM
        // thread is the stall M11-03 exists to remove — the legacy path is
        // kept for callers that never attach a resolver).
        if (cfg_.unix_target) {
            addrs_ = {*cfg_.unix_target};
            begin_connect();
            return {};
        }
        if (auto a = SockAddr::parse(cfg_.target.host, cfg_.target.port); a) {
            addrs_ = {*a};
            begin_connect();
            return {};
        }
        if (cfg_.resolver) {
            set_state(ClientState::Connecting);
            resolving_ = true;
            Duration budget = connect_budget();
            cfg_.resolver->resolve(
                cfg_.target.host, cfg_.target.port, *em_, budget,
                [this](Result<Resolver::AddrList> r) {
                    resolving_ = false;
                    if (!r) {
                        if (handlers_.on_error)
                            handlers_.on_error(ConnId{}, r.error());
                        set_state(ClientState::Disconnected);
                        schedule_reconnect();
                        return;
                    }
                    addrs_ = std::move(*r);
                    begin_connect();
                });
            return {};
        }
        auto resolved = resolve_target();
        if (!resolved) return resolved.error();
        addrs_ = std::move(*resolved);
        begin_connect();
        return {};
    }

    void on_state_change(StateFn&& f) { state_fn_ = std::move(f); }
    ClientState state() const noexcept { return state_; }
    Connection<P, EM>* conn() { return conn_; }
    EM& em() const noexcept { return *em_; }

    // ---- §20 drain-sequence hooks ----------------------------------------
    static void hooks_notify(void* p) {
        auto* me = static_cast<TcpClient*>(p);
        if (me->conn_ && me->handlers_.on_shutdown)
            me->handlers_.on_shutdown(me->conn_->id());
    }
    static bool hooks_drained(void* p) {
        auto* me = static_cast<TcpClient*>(p);
        return !me->conn_ || me->conn_->queued_write_bytes() == 0;
    }
    static void hooks_shutdown_write(void* p) {
        auto* me = static_cast<TcpClient*>(p);
        if (me->conn_) me->conn_->shutdown_write();
    }

  private:
    void set_state(ClientState s) {
        if (state_ == s) return;
        state_ = s;
        if (state_fn_) state_fn_(s);
    }

    Result<std::vector<SockAddr>> resolve_target() {
        // Numeric first; names via getaddrinfo (setup-path, not the loop).
        if (auto a = SockAddr::parse(cfg_.target.host, cfg_.target.port); a)
            return std::vector<SockAddr>{*a};
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        std::string port = std::to_string(cfg_.target.port);
        if (::getaddrinfo(cfg_.target.host.c_str(), port.c_str(), &hints,
                          &res) != 0 ||
            !res)
            return make_error(ErrorCategory::Net, Err::ResolveFailed);
        std::vector<SockAddr> out;
        for (auto* p = res; p; p = p->ai_next) {
            SockAddr a;
            std::memcpy(a.addr(), p->ai_addr, p->ai_addrlen);
            out.push_back(a);
        }
        ::freeaddrinfo(res);
        if (out.empty())
            return make_error(ErrorCategory::Net, Err::ResolveFailed);
        return out;
    }

    // Connect timeout derives from the ambient deadline (§7.4) when set,
    // else the configured connect_timeout.
    Duration connect_budget() const {
        Duration budget = cfg_.connect_timeout;
        if (em_->context().deadline.is_set())
            budget =
                std::min(budget, em_->context().deadline.remaining(em_->now()));
        return budget;
    }

    void begin_connect() {
        addr_idx_ = 0;
        if (cfg_.happy_eyeballs && addrs_.size() > 1) {
            he_begin();
            return;
        }
        connect_next();
    }

    void connect_next() {
        if (addr_idx_ >= addrs_.size()) {
            set_state(ClientState::Disconnected);
            schedule_reconnect();
            return;
        }
        const SockAddr& target = addrs_[addr_idx_++];
        auto fd = sock::create(target.family(), cfg_.sock);
        if (!fd) {
            connect_next();
            return;
        }
        int raw = *fd;
        if (cfg_.bind_local)
            (void)sock::bind(raw, *cfg_.bind_local, false, false);

        if (conn_) {
            pool_.destroy(conn_);
            conn_ = nullptr;
        }
        conn_ = pool_.construct(
            *em_, raw, handlers_,
            typename Conn::Params{
                cfg_.flow, {}, {}, {}, cfg_.sock.timestamping, cfg_.fd_passing},
            this, &TcpClient::on_conn_gone);
        if (!conn_) {
            ::close(raw);
            schedule_reconnect();
            return;
        }
        set_state(ClientState::Connecting);
        em_->submit_connect(conn_->sink(), raw, target);

        EM* em = em_;
        auto me = conn_->id();
        em_->after(
            connect_budget(),
            [em, me](TimerCtx) {
                if (auto* c = Conn::resolve(*em, me))
                    if (c->state() == ConnState::Connecting)
                        c->close(CloseReason::ConnectFailed);
            },
            group_);
    }

    // ---- Happy Eyeballs (M11-04, RFC 8305) ----------------------------------
    // In-flight attempts get attempt_handlers_: on_open/on_close are
    // intercepted so losers never reach the user's handlers; every other
    // hook forwards. The winner promotes to conn_ — its later close lands
    // in he_lost and is forwarded as a normal user-visible close.
    void build_attempt_handlers() {
        attempt_handlers_.proto = handlers_.proto;
        attempt_handlers_.on_messages =
            [this](ConnId id,
                   std::span<const typename Handlers<P>::Message> b) {
                if (handlers_.on_messages) handlers_.on_messages(id, b);
            };
        attempt_handlers_.on_error = [this](ConnId id, Error e) {
            if (handlers_.on_error) handlers_.on_error(id, e);
        };
        attempt_handlers_.on_writable = [this](ConnId id) {
            if (handlers_.on_writable) handlers_.on_writable(id);
        };
        attempt_handlers_.on_shutdown = [this](ConnId id) {
            if (handlers_.on_shutdown) handlers_.on_shutdown(id);
        };
        attempt_handlers_.on_open = [this](ConnId id, Peer p) {
            he_win(id, p);
        };
        attempt_handlers_.on_close = [this](ConnId id, CloseReason r) {
            he_lost(id, r);
        };
    }

    void he_begin() {
        he_won_ = false;
        he_next_ = 0;
        build_attempt_handlers();
        he_group_ = em_->make_timer_group();
        // RFC 8305 §5: v6 first, then alternate families.
        he_addrs_.clear();
        std::vector<const SockAddr*> v6, v4;
        for (auto& a : addrs_) (a.family() == AF_INET6 ? v6 : v4).push_back(&a);
        while (!v6.empty() || !v4.empty()) {
            if (!v6.empty()) {
                he_addrs_.push_back(*v6.front());
                v6.erase(v6.begin());
            }
            if (!v4.empty()) {
                he_addrs_.push_back(*v4.front());
                v4.erase(v4.begin());
            }
        }
        if (he_addrs_.empty()) {
            set_state(ClientState::Disconnected);
            schedule_reconnect();
            return;
        }
        // One overall deadline bounds the whole attempt set.
        em_->after(
            connect_budget(), [this](TimerCtx) { he_timeout(); }, he_group_);
        he_launch();
        arm_stagger();
    }

    void he_launch() {
        if (he_won_ || he_next_ >= he_addrs_.size()) return;
        const SockAddr& target = he_addrs_[he_next_++];
        auto fd = sock::create(target.family(), cfg_.sock);
        if (!fd) {
            he_attempt_done(nullptr);
            return;
        }
        int raw = *fd;
        if (cfg_.bind_local)
            (void)sock::bind(raw, *cfg_.bind_local, false, false);
        Conn* c = pool_.construct(
            *em_, raw, attempt_handlers_,
            typename Conn::Params{
                cfg_.flow, {}, {}, {}, cfg_.sock.timestamping, cfg_.fd_passing},
            this, &TcpClient::on_conn_gone);
        if (!c) {
            ::close(raw);
            he_attempt_done(nullptr);
            return;
        }
        pending_.push_back(c);
        set_state(ClientState::Connecting);
        em_->submit_connect(c->sink(), raw, target);
    }

    void arm_stagger() {
        if (he_won_ || he_next_ >= he_addrs_.size()) return;
        em_->after(
            cfg_.he_stagger,
            [this](TimerCtx) {
                he_launch();
                arm_stagger();
            },
            he_group_);
    }

    Conn* find_pending(ConnId id) {
        for (auto* c : pending_)
            if (c->id() == id) return c;
        return nullptr;
    }

    void he_win(ConnId id, Peer p) {
        if (he_won_) {
            // A second attempt completed in the same instant — lose it
            // quietly (its close lands in he_lost, not the user's on_close).
            if (auto* c = find_pending(id)) c->close(CloseReason::LocalClose);
            return;
        }
        Conn* w = find_pending(id);
        if (!w) return;
        he_won_ = true;
        conn_ = w;
        pending_.erase(std::find(pending_.begin(), pending_.end(), w));
        // Losers close quietly; on_conn_gone reclaims them from closing_.
        auto losers = std::move(pending_);
        pending_.clear();
        for (auto* c : losers) {
            closing_.push_back(c);
            c->close(CloseReason::LocalClose);
        }
        if (he_group_.valid()) {
            em_->cancel_group(he_group_);
            he_group_ = {};
        }
        handlers_.on_open(id, p);  // wrapped: on_connected + user callback
    }

    void he_lost(ConnId id, CloseReason r) {
        // Only the promoted conn's close is user-visible.
        if (conn_ && conn_->id() == id && handlers_.on_close)
            handlers_.on_close(id, r);
    }

    void he_timeout() {
        // Overall deadline hit — exhaust the candidate list so the reclaim
        // path drains to Disconnected instead of launching more attempts.
        he_next_ = he_addrs_.size();
        for (auto* c : pending_) c->close(CloseReason::ConnectFailed);
        // on_conn_gone drains the attempts and drives the reconnect path.
    }

    // A launch failure or a dead attempt: with nothing in flight, fail over
    // to the next candidate immediately (RFC 8305); while other attempts
    // are still pending, the stagger timer paces the rest.
    void he_attempt_done(Conn*) {
        if (he_won_ || !pending_.empty()) return;
        if (he_next_ < he_addrs_.size()) {
            he_launch();
        } else {
            set_state(ClientState::Disconnected);
            schedule_reconnect();
        }
    }

    void destroy_conn() noexcept {
        pool_.destroy(conn_);
        conn_ = nullptr;
    }

    void on_connected() {
        cfg_.reconnect.reset();
        set_state(ClientState::Connected);
    }

    void schedule_reconnect() {
        if (!cfg_.auto_reconnect) return;
        set_state(ClientState::ReconnectWait);
        Duration d = cfg_.reconnect.next();
        // jitter: uniform ±jitter around the backoff value
        double j = cfg_.reconnect.jitter;
        if (j > 0) {
            std::uniform_real_distribution<double> u(1.0 - j, 1.0 + j);
            d = Duration(std::int64_t(d.count() * u(rng_)));
        }
        em_->after(d, [this](TimerCtx) { begin_connect(); }, group_);
    }

    static void on_conn_gone(void* self, ConnId id, CloseReason r) {
        auto* me = static_cast<TcpClient*>(self);
        // The notification is deferred — only drop the conn if it is still
        // the one that died (a reconnect may already have replaced it).
        if (me->conn_ && me->conn_->id() == id)
            me->destroy_conn();
        else
            me->reclaim_attempt(id);
        if (r == CloseReason::ConnectFailed) {
            // Sequential mode retries the next resolved address; HE drives
            // its own failover from reclaim_attempt.
            if (!me->cfg_.happy_eyeballs || me->he_addrs_.empty())
                me->connect_next();
        } else if (me->state_ != ClientState::Disconnected) {
            me->schedule_reconnect();
        }
    }

    // Reclaim a finished HE attempt (loser teardown or connect failure) and
    // advance the attempt set. Runs on the deferred notify, so pending_ has
    // already been detached from the winner.
    void reclaim_attempt(ConnId id) {
        for (auto it = pending_.begin(); it != pending_.end(); ++it)
            if ((*it)->id() == id) {
                pool_.destroy(*it);
                pending_.erase(it);
                he_attempt_done(nullptr);
                return;
            }
        for (auto it = closing_.begin(); it != closing_.end(); ++it)
            if ((*it)->id() == id) {
                pool_.destroy(*it);
                closing_.erase(it);
                return;
            }
    }

    EM* em_;
    ClientConfig cfg_;
    Handlers<P> handlers_;
    Handlers<P> attempt_handlers_;  // HE: loser-suppressing view (M11-04)
    StateFn state_fn_;
    Pool<Conn> pool_;
    Conn* conn_ = nullptr;
    std::vector<Conn*> pending_;  // HE attempts in flight
    std::vector<Conn*> closing_;  // HE losers torn down, notify pending
    std::vector<SockAddr> addrs_;
    std::vector<SockAddr> he_addrs_;  // RFC 8305 interleaved order
    std::size_t addr_idx_ = 0;
    std::size_t he_next_ = 0;
    bool he_won_ = false;
    bool resolving_ = false;
    ClientState state_ = ClientState::Disconnected;
    TimerGroup group_{};
    TimerGroup he_group_{};  // stagger + overall HE deadline timers
    std::mt19937_64 rng_{std::random_device{}()};
};

// EventManager::make_client
template <class C, class B>
template <class P, class H>
Result<TcpClient<P, BasicEventManager<C, B>>*>
BasicEventManager<C, B>::make_client(ClientConfig cfg, H&& handlers) {
    using Cli = TcpClient<P, BasicEventManager>;
    auto* s = new Cli(*this, std::move(cfg), std::forward<H>(handlers));
    if (auto r = s->start(); !r) {
        delete s;
        return r.error();
    }
    typename BasicEventManager::ShutdownHooks hooks{nullptr, &Cli::hooks_notify,
                                                    &Cli::hooks_drained,
                                                    &Cli::hooks_shutdown_write};
    own(s, [](void* p) { delete static_cast<Cli*>(p); }, hooks);
    return s;
}

}  // namespace afx

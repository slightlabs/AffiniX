#pragma once

// TcpClient — DESIGN.md §15.2. Connect with a deadline, reconnect with
// jittered exponential backoff, connection-state observability. Hostname
// resolution is synchronous here only for numeric addresses; name resolution
// is done once at construction (before the loop runs in the common case) —
// the async resolver is a §28.2 extension.

#include <netdb.h>
#include <random>

#include "afx/core/pool.hpp"
#include "afx/net/connection.hpp"
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
    bool happy_eyeballs = true;
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
        if (conn_) {
            conn_->close(CloseReason::Shutdown);
            pool_.destroy(conn_);
            conn_ = nullptr;
        }
        if (group_.valid()) em_->cancel_group(group_);
    }

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    Result<void> start() {
        auto resolved = resolve_target();
        if (!resolved) return resolved.error();
        addrs_ = std::move(*resolved);
        // Observe Connected via the user's on_open path.
        auto user_open = std::move(handlers_.on_open);
        handlers_.on_open = [this, u = std::move(user_open)](ConnId id,
                                                             Peer p) mutable {
            on_connected();
            if (u) u(id, p);
        };
        group_ = em_->make_timer_group();
        connect_next();
        return {};
    }

    void on_state_change(StateFn&& f) { state_fn_ = std::move(f); }
    ClientState state() const noexcept { return state_; }
    Connection<P, EM>* conn() { return conn_; }

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
        conn_ = pool_.construct(*em_, raw, handlers_,
                                typename Conn::Params{cfg_.flow, {}, {}, {},
                                                      cfg_.sock.timestamping},
                                this, &TcpClient::on_conn_gone);
        if (!conn_) {
            ::close(raw);
            schedule_reconnect();
            return;
        }
        set_state(ClientState::Connecting);
        em_->submit_connect(conn_->sink(), raw, target);

        // Connect timeout derives from the ambient deadline (§7.4) when set,
        // else the configured connect_timeout.
        Duration budget = cfg_.connect_timeout;
        if (em_->context().deadline.is_set())
            budget =
                std::min(budget, em_->context().deadline.remaining(em_->now()));
        EM* em = em_;
        auto me = conn_->id();
        em_->after(
            budget,
            [em, me](TimerCtx) {
                if (auto* c = Conn::resolve(*em, me))
                    if (c->state() == ConnState::Connecting)
                        c->close(CloseReason::ConnectFailed);
            },
            group_);
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
        em_->after(
            d,
            [this](TimerCtx) {
                addr_idx_ = 0;
                connect_next();
            },
            group_);
    }

    static void on_conn_gone(void* self, ConnId id, CloseReason r) {
        auto* me = static_cast<TcpClient*>(self);
        // The notification is deferred — only drop the conn if it is still
        // the one that died (a reconnect may already have replaced it).
        if (me->conn_ && me->conn_->id() == id) me->destroy_conn();
        if (r == CloseReason::ConnectFailed) {
            me->connect_next();  // try the next resolved address
        } else if (me->state_ != ClientState::Disconnected) {
            me->schedule_reconnect();
        }
    }

    EM* em_;
    ClientConfig cfg_;
    Handlers<P> handlers_;
    StateFn state_fn_;
    Pool<Conn> pool_;
    Conn* conn_ = nullptr;
    std::vector<SockAddr> addrs_;
    std::size_t addr_idx_ = 0;
    ClientState state_ = ClientState::Disconnected;
    TimerGroup group_{};
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
    typename BasicEventManager::ShutdownHooks hooks{
        nullptr, &Cli::hooks_notify, &Cli::hooks_drained,
        &Cli::hooks_shutdown_write};
    own(s, [](void* p) { delete static_cast<Cli*>(p); }, hooks);
    return s;
}

}  // namespace afx

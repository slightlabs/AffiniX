#pragma once

// TcpServer — DESIGN.md §15.1. Owned by its EM; any number may coexist per
// EM, each with a different Protocol. With reuse_port every EM shard opens
// its own listener on the same port and the kernel distributes accepts.

#include <memory>
#include <unordered_map>

#include "afx/net/connection.hpp"
#include "afx/net/socket.hpp"

namespace afx {

struct ServerConfig {
    SockAddr bind = SockAddr::any(0);
    int backlog = 1024;
    bool reuse_port = true;
    bool reuse_addr = true;
    bool defer_accept = false;
    std::size_t max_connections = 64 * 1024;
    SocketOptions sock{};
    FlowControl flow{};
    Duration idle_read_timeout{};
    Duration idle_write_timeout{};
    std::size_t accepts_per_iteration = 32;
};

template <class P, class EM>
class TcpServer {
  public:
    using Conn = Connection<P, EM>;

    TcpServer(EM& em, ServerConfig cfg, Handlers<P> handlers)
        : em_(&em), cfg_(std::move(cfg)), handlers_(std::move(handlers)) {}

    ~TcpServer() {
        // Move conns_ out before closing: close() may synchronously invoke
        // on_conn_gone() (when the EM is no longer running, nothing will
        // ever drain a deferred notification), which erases from conns_.
        // Iterating conns_ while it is being erased from under us is
        // undefined behaviour; iterating a detached copy sidesteps it, and
        // the erase() inside on_conn_gone() becomes a harmless no-op against
        // the now-empty member map.
        auto conns = std::move(conns_);
        conns_.clear();
        for (auto& [k, c] : conns) c->close(CloseReason::Shutdown);
        conns.clear();
        if (listen_fd_ >= 0) {
            em_->backend_detach(listen_fd_);
            ::close(listen_fd_);
        }
        if (accept_sink_.valid()) em_->release_sink(accept_sink_);
    }

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    Result<void> open() {
        auto fd = sock::create(
            cfg_.bind.family() ? cfg_.bind.family() : AF_INET, cfg_.sock);
        if (!fd) return fd.error();
        listen_fd_ = *fd;
        if (auto r = sock::bind(listen_fd_, cfg_.bind, cfg_.reuse_addr,
                                cfg_.reuse_port);
            !r)
            return fail(r.error());
        if (auto r = sock::listen(listen_fd_, cfg_.backlog); !r)
            return fail(r.error());
        if (auto lr = sock::local_addr(listen_fd_); lr) bound_ = *lr;

        accept_sink_ = em_->register_sink(this, &TcpServer::dispatch_accept,
                                          nullptr, {}, &type_tag_);
        return em_->submit_accept(accept_sink_, listen_fd_);
    }

    SockAddr bound_addr() const noexcept { return bound_; }
    int listen_fd() const noexcept { return listen_fd_; }
    std::size_t connection_count() const noexcept { return conns_.size(); }

    Connection<P, EM>* conn(ConnId id) {
        auto it = conns_.find(key(id));
        return it == conns_.end() ? nullptr : it->second.get();
    }

    void close_conn(ConnId id, CloseReason r) {
        if (auto* c = conn(id)) c->close(r);
    }

  private:
    static std::uint64_t key(ConnId id) noexcept {
        return (std::uint64_t(id.idx) << 32) | id.gen;
    }

    Result<void> fail(Error e) {
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        return e;
    }

    static void dispatch_accept(void* obj, OpKind kind, const Completion& c) {
        if (kind == OpKind::Accept) static_cast<TcpServer*>(obj)->on_accept(c);
    }

    void on_accept(const Completion& c) {
        // Re-arm a single-shot accept on every terminal completion —
        // including transient errors and capacity drops, or the accept
        // pipeline silently dies. A multishot accept stays armed while
        // More is set; a cancelled accept is never re-armed.
        bool terminal = !(c.flags & CompletionFlag::More);
        if (terminal && c.result != -ECANCELED)
            em_->submit_accept(accept_sink_, listen_fd_);
        if (c.result < 0) {
            if (c.result != -ECANCELED) ++em_->stats().internal_errors;
            return;
        }
        int cfd = c.result;
        ++em_->stats().accepts;
        if (conns_.size() >= cfg_.max_connections) {
            ::close(cfd);
            return;  // bounded, as everything is
        }
        if (auto r = sock::apply(cfd, cfg_.sock); !r) {
            ::close(cfd);
            return;
        }
        Peer peer{};
        if (auto p = sock::peer_addr(cfd); p) peer.addr = *p;

        typename Conn::Params params{cfg_.flow, cfg_.idle_read_timeout,
                                     cfg_.idle_write_timeout,
                                     em_->config().memory.read_buffer_size};
        auto conn = std::make_unique<Conn>(*em_, cfd, handlers_, params, this,
                                           &TcpServer::on_conn_gone);
        ConnId id = conn->id();
        conns_[key(id)] = std::move(conn);
        ++em_->stats().conns_opened;
        em_->recorder().record(EventKind::Accept, id.idx, std::uint32_t(cfd));
        conns_[key(id)]->start(peer);
    }

    static void on_conn_gone(void* self, ConnId id, CloseReason) {
        static_cast<TcpServer*>(self)->conns_.erase(key(id));
    }

    EM* em_;
    ServerConfig cfg_;
    Handlers<P> handlers_;
    int listen_fd_ = -1;
    SockAddr bound_{};
    typename EM::SinkHandle accept_sink_{};
    std::unordered_map<std::uint64_t, std::unique_ptr<Conn>> conns_;
    static inline char type_tag_{};
};

// EventManager::make_server — defined here so the EM header stays net-free.
template <class C, class B>
template <class P, class H>
Result<TcpServer<P, BasicEventManager<C, B>>*>
BasicEventManager<C, B>::make_server(ServerConfig cfg, H&& handlers) {
    using Srv = TcpServer<P, BasicEventManager>;
    auto* s = new Srv(*this, std::move(cfg), std::forward<H>(handlers));
    if (auto r = s->open(); !r) {
        delete s;
        return r.error();
    }
    own(s, [](void* p) { delete static_cast<Srv*>(p); });
    return s;
}

}  // namespace afx

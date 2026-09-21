#pragma once

// net/udp_socket.hpp — UdpSocket<P> (M11-01/02, DESIGN.md §28.2).
//
// UDP is datagram-oriented, so it does not ride the proactor submit_recv
// seam (which carries no source address): the socket is watched for
// Interest::Readable through the generic em.watch() escape hatch and the
// socket itself performs recvmmsg/sendmmsg when the fd reports ready —
// readiness emulated at the socket layer, exactly as ADR-0002 prescribes
// for readiness backends. The code is identical on epoll, io_uring
// (multishot POLL_ADD), kqueue, and SimBackend's deliver_watch.
//
// Per-datagram semantics: each received datagram is run through a FRESH
// FramerForT<P> — datagram boundaries are message boundaries, so framer
// state must never leak across packets. A datagram may carry zero or more
// complete messages; a trailing partial message is a truncation error,
// reported via on_error (the datagram's earlier messages still deliver).
// Set UdpHandlers::on_datagram instead for raw byte delivery.

#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "afx/core/event_manager.hpp"
#include "afx/net/protocol.hpp"
#include "afx/net/sock_addr.hpp"
#include "afx/net/socket.hpp"
#include "afx/sys/inline_fn.hpp"
#include "afx/sys/types.hpp"

namespace afx {

struct UdpConfig {
    SockAddr bind = SockAddr::any(0);
    SocketOptions sock{};
    bool reuse_addr = true;
    bool reuse_port = false;          // SO_REUSEPORT fan-out when true
    std::size_t recv_batch = 32;      // datagrams per readiness event
    std::size_t max_datagram = 8192;  // per-datagram scratch size
};

// RX slot for sock::recv_datagrams: `from` + bytes received into `buf`.
struct UdpRx {
    SockAddr from{};
    MutByteSpan buf{};
    int len = 0;
};

// TX element for sock::send_datagrams.
struct UdpTx {
    SockAddr to{};
    ByteSpan bytes{};
};

namespace sock {

// SOCK_DGRAM|NONBLOCK|CLOEXEC; only datagram-safe SocketOptions are applied
// (nonblock/rcvbuf/sndbuf/timestamping — TCP-specific knobs are skipped).
Result<int> create_datagram(int family, const SocketOptions& opts);

// Batched datagram I/O — Linux uses recvmmsg/sendmmsg, other platforms a
// recvmsg/sendmsg loop. Return the count of datagrams moved, or an Error.
// recv returns 0 on EAGAIN (nothing ready); callers treat that as done.
Result<int> recv_datagrams(int fd, std::span<UdpRx> out);
Result<int> send_datagrams(int fd, std::span<const UdpTx> msgs);

// ---- multicast (M11-02) ----------------------------------------------------
// `group` is the multicast address; `iface` selects the interface (any(0)
// for the default). Source filtering (SFM) is IPv4-only; on a v6 group the
// source ops return Err::Unsupported.
Result<void> join_group(int fd, const SockAddr& group, const SockAddr& iface);
Result<void> leave_group(int fd, const SockAddr& group, const SockAddr& iface);
Result<void> join_source(int fd, const SockAddr& group, const SockAddr& src,
                         const SockAddr& iface);
Result<void> leave_source(int fd, const SockAddr& group, const SockAddr& src,
                          const SockAddr& iface);
Result<void> set_multicast_ttl(int fd, int ttl);
Result<void> set_multicast_loop(int fd, bool on);
Result<void> set_multicast_if(int fd, const SockAddr& iface);

}  // namespace sock

// ---------------------------------------------------------------------------

template <class P>
struct UdpHandlers {
    using Message = MessageForT<P>;
    // Exactly one delivery hook is required: parsed messages per datagram
    // (fresh framer per packet) or the raw datagram bytes.
    InlineFn<void(SockAddr, std::span<const Message>), 64> on_messages;
    InlineFn<void(SockAddr, ByteSpan), 64> on_datagram;
    InlineFn<void(Error), 48> on_error;
    P proto{};
};

template <class P, class EM>
class UdpSocket {
  public:
    using Framer = FramerForT<P>;
    using Message = typename Framer::Message;

    UdpSocket(EM& em, UdpConfig cfg, UdpHandlers<P> handlers)
        : em_(&em), cfg_(std::move(cfg)), handlers_(std::move(handlers)) {
        rx_.resize(cfg_.recv_batch);
        scratch_.resize(cfg_.recv_batch * cfg_.max_datagram);
        msgs_.reserve(64);
    }

    ~UdpSocket() { close(); }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    Result<void> open() {
        int fam = cfg_.bind.family() ? cfg_.bind.family() : AF_INET;
        auto fd = sock::create_datagram(fam, cfg_.sock);
        if (!fd) return fd.error();
        fd_ = *fd;
        if (auto r =
                sock::bind(fd_, cfg_.bind, cfg_.reuse_addr, cfg_.reuse_port);
            !r) {
            close();
            return r.error();
        }
        if (auto a = sock::local_addr(fd_)) bound_ = *a;
        auto w = em_->watch(fd_, Interest::Readable,
                            [this](IoId, std::int32_t mask) {
                                if (mask & CompletionFlag::ReadyRead)
                                    on_readable();
                            });
        if (!w) {
            close();
            return w.error();
        }
        watch_ = *w;
        return {};
    }

    void close() noexcept {
        if (watch_.valid()) {
            em_->unwatch(watch_);
            watch_ = IoId{};
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    // ---- sends --------------------------------------------------------------
    // Datagram sends go straight to the kernel — UDP has no stream to queue
    // into; ENOBUFS/EMSGSIZE surface to the caller as an Error.

    Result<void> send_to(const SockAddr& to, ByteSpan bytes) {
        if (fd_ < 0) return make_error(ErrorCategory::Internal, Err::Closed);
        UdpTx t{to, bytes};
        auto r = sock::send_datagrams(fd_, std::span<const UdpTx>(&t, 1));
        if (!r) return r.error();
        return {};
    }

    // Returns the number of datagrams sent (may be < msgs.size() on EAGAIN).
    Result<int> send_batch(std::span<const UdpTx> msgs) {
        if (fd_ < 0) return make_error(ErrorCategory::Internal, Err::Closed);
        return sock::send_datagrams(fd_, msgs);
    }

    // ---- multicast
    // ------------------------------------------------------------

    Result<void> join_group(const SockAddr& group,
                            const SockAddr& iface = SockAddr::any(0)) {
        return sock::join_group(fd_, group, iface);
    }
    Result<void> leave_group(const SockAddr& group,
                             const SockAddr& iface = SockAddr::any(0)) {
        return sock::leave_group(fd_, group, iface);
    }
    Result<void> join_source(const SockAddr& group, const SockAddr& src,
                             const SockAddr& iface = SockAddr::any(0)) {
        return sock::join_source(fd_, group, src, iface);
    }
    Result<void> leave_source(const SockAddr& group, const SockAddr& src,
                              const SockAddr& iface = SockAddr::any(0)) {
        return sock::leave_source(fd_, group, src, iface);
    }
    Result<void> set_multicast_ttl(int ttl) {
        return sock::set_multicast_ttl(fd_, ttl);
    }
    Result<void> set_multicast_loop(bool on) {
        return sock::set_multicast_loop(fd_, on);
    }
    Result<void> set_multicast_if(const SockAddr& iface) {
        return sock::set_multicast_if(fd_, iface);
    }

    SockAddr bound_addr() const noexcept { return bound_; }
    int fd() const noexcept { return fd_; }
    // Batch-receive stats (M11-01): drains that produced ≥1 datagram, and
    // total datagrams received. datagrams_in/drains is the batch factor.
    std::uint64_t drains() const noexcept { return drains_; }
    std::uint64_t datagrams_in() const noexcept { return datagrams_in_; }

    // §20: no write queue exists to drain — closing the fd is the whole
    // drain story for UDP.
    static void hooks_begin(void* p) { static_cast<UdpSocket*>(p)->close(); }

  private:
    void on_readable() {
        // Per-slot destination buffers (stable across the recvmmsg call).
        for (std::size_t i = 0; i < rx_.size(); ++i)
            rx_[i].buf = MutByteSpan(scratch_.data() + i * cfg_.max_datagram,
                                     cfg_.max_datagram);
        for (;;) {
            auto n = sock::recv_datagrams(fd_, rx_);
            if (!n) {
                if (handlers_.on_error) handlers_.on_error(n.error());
                return;
            }
            if (*n <= 0) return;  // EAGAIN — drained
            ++drains_;
            datagrams_in_ += std::uint64_t(*n);
            for (int i = 0; i < *n; ++i) deliver(rx_[i]);
            if (std::size_t(*n) < rx_.size()) return;  // short read: done
        }
    }

    void deliver(const UdpRx& rx) {
        ByteSpan bytes(rx.buf.data(), std::size_t(rx.len));
        if (handlers_.on_datagram) {
            handlers_.on_datagram(rx.from, bytes);
            return;
        }
        if (!handlers_.on_messages) return;
        // Fresh framer per datagram: a packet is a message boundary, never
        // a continuation of the previous packet's partial parse.
        Framer framer{handlers_.proto};
        msgs_.clear();
        std::size_t off = 0;
        while (off < bytes.size()) {
            auto pr = framer.parse(bytes.subspan(off));
            if (pr.kind == ParseResult<Message>::Kind::Message) {
                msgs_.push_back(pr.message);
                off += pr.consumed;
                if (msgs_.size() == msgs_.capacity()) {
                    handlers_.on_messages(rx.from, msgs_);
                    msgs_.clear();
                }
                continue;
            }
            if (pr.kind == ParseResult<Message>::Kind::NeedMore) {
                // Truncated datagram — a UDP packet never gets "more".
                if (handlers_.on_error)
                    handlers_.on_error(
                        make_error(ErrorCategory::Frame, Err::Truncated));
            } else if (handlers_.on_error) {
                handlers_.on_error(pr.error);
            }
            break;
        }
        if (!msgs_.empty()) handlers_.on_messages(rx.from, msgs_);
    }

    EM* em_;
    UdpConfig cfg_;
    UdpHandlers<P> handlers_;
    int fd_ = -1;
    IoId watch_{};
    SockAddr bound_{};
    std::vector<UdpRx> rx_;
    std::vector<std::byte> scratch_;
    std::vector<Message> msgs_;
    std::uint64_t drains_ = 0;
    std::uint64_t datagrams_in_ = 0;
};

// EventManager::make_udp — defined here so the EM header stays net-free.
template <class C, class B>
template <class P, class H>
Result<UdpSocket<P, BasicEventManager<C, B>>*>
BasicEventManager<C, B>::make_udp(UdpConfig cfg, H&& handlers) {
    using S = UdpSocket<P, BasicEventManager>;
    auto* s =
        new S(*this, std::move(cfg), static_cast<UdpHandlers<P>&&>(handlers));
    if (auto r = s->open(); !r) {
        delete s;
        return r.error();
    }
    ShutdownHooks hooks;
    hooks.begin = &S::hooks_begin;
    own(s, [](void* p) { delete static_cast<S*>(p); }, hooks);
    return s;
}

}  // namespace afx

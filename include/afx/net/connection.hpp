#pragma once

// Connection — DESIGN.md §13.1 (state machine), §16 (flow control).
// Owned by a TcpServer/TcpClient on the connection's EM; referenced
// cross-iteration and cross-thread only by ConnId handle.

#include <sys/socket.h>
#include <unistd.h>
#include <coroutine>
#include <cstring>
#include <vector>

#include "afx/core/event_manager.hpp"
#include "afx/core/io_buffer.hpp"
#include "afx/coro/task.hpp"
#include "afx/net/protocol.hpp"
#include "afx/net/socket.hpp"

namespace afx {

enum class ConnState : std::uint8_t {
    Connecting,
    Established,
    ShutdownWrite,
    Closing,
    Closed,
};

enum class CloseReason : std::uint8_t {
    PeerFin,     // orderly close by peer
    LocalClose,  // close() called locally
    FrameError,  // protocol validate/parse failure
    IdleTimeout,
    WriteOverflow,  // write queue exceeded policy
    Error,          // socket error
    ConnectFailed,
    Shutdown,  // EM/runtime teardown
    HeldOverflow,  // coroutine session stopped consuming (held cap hit)
};

// on_messages is required; everything else optional (empty InlineFn).
template <class P>
struct Handlers {
    using Message = MessageForT<P>;
    InlineFn<void(ConnId, std::span<const Message>), 64> on_messages;
    InlineFn<void(ConnId, Peer), 48> on_open;
    InlineFn<void(ConnId, CloseReason), 48> on_close;
    InlineFn<void(ConnId, Error), 48> on_error;
    InlineFn<void(ConnId), 48> on_writable;
    // §20 stage 2: invoked once per connection when shutdown begins, before
    // the drain wait — the app's chance to flush a final response.
    InlineFn<void(ConnId), 48> on_shutdown;
    P proto{};  // protocol instance (stateful framers allowed)
};

// ---------------------------------------------------------------------------

template <class P, class EM>
class Connection {
  public:
    using Framer = FramerForT<P>;
    using Message = typename Framer::Message;
    using SinkHandle = typename EM::SinkHandle;

    struct Params {
        FlowControl flow{};
        Duration idle_read_timeout{};
        Duration idle_write_timeout{};
        std::size_t read_buffer_size = 64u << 10;
        bool timestamping = false;  // SO_TIMESTAMPING RX stamps (§9.4)
    };

    // notify(owner, ConnId, CloseReason) is invoked via em.defer() once the
    // connection is fully torn down; the owner erases its bookkeeping there.
    Connection(EM& em, int fd, Handlers<P>& handlers, Params params,
               void* owner, void (*notify)(void*, ConnId, CloseReason))
        : em_(&em),
          fd_(fd),
          handlers_(&handlers),
          framer_{handlers.proto},
          params_(params),
          read_buf_(params.read_buffer_size),
          write_buf_(64u << 10),
          owner_(owner),
          notify_(notify) {
        sink_ = em_->register_sink(this, &Connection::dispatch, nullptr,
                                   em_->context(), &type_tag_);
        id_ = ConnId{sink_.idx, sink_.gen};
        group_ = em_->make_timer_group();
        batch_.reserve(64);
    }

    ~Connection() {
        if (fd_ >= 0) {
            em_->backend_detach(fd_);
            ::close(fd_);
        }
        if (group_.valid()) em_->cancel_group(group_);
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Resolve a ConnId to a live Connection on the owning EM's thread.
    // Returns nullptr for stale handles (§13: operations on a dead connection
    // report Closed rather than crash).
    static Connection* resolve(EM& em, ConnId id) {
        auto* s = em.sink(SinkHandle{id.idx, id.gen});
        if (!s || s->type_tag != &type_tag_) return nullptr;
        auto* c = static_cast<Connection*>(s->obj);
        return c->state_ == ConnState::Closed ? nullptr : c;
    }

    ConnId id() const noexcept { return id_; }
    SinkHandle sink() const noexcept { return sink_; }
    int fd() const noexcept { return fd_; }
    ConnState state() const noexcept { return state_; }
    EM& em() const noexcept { return *em_; }
    const Peer& peer() const noexcept { return peer_; }
    std::size_t queued_write_bytes() const noexcept {
        return write_buf_.size();
    }

    // Begin reading once Established (accept path, or post-connect).
    void start(Peer peer) {
        peer_ = peer;
        state_ = ConnState::Established;
        if (params_.timestamping) em_->backend_set_timestamping(fd_, true);
        if (handlers_->on_open) handlers_->on_open(id_, peer_);
        arm_idle_timer();
        rearm_recv();
    }

    // ---- sending
    // ---------------------------------------------------------------- Queues
    // bytes; the loop emits one sendv per connection in stage 6 (§7.3).
    [[nodiscard]] SendResult send(ByteSpan bytes) {
        if (state_ != ConnState::Established) return SendResult::Closed;
        if (write_buf_.size() + bytes.size() >
            params_.flow.write_high_watermark)
            return apply_overflow(bytes);
        auto w = write_buf_.writable(bytes.size());
        std::memcpy(w.data(), bytes.data(), bytes.size());
        write_buf_.commit(bytes.size());
        em_->note_write_pending(sink_);
        check_write_watermark();
        return SendResult::Queued;
    }

    [[nodiscard]] SendResult send_scatter(std::span<const ByteSpan> parts) {
        if (state_ != ConnState::Established) return SendResult::Closed;
        std::size_t total = 0;
        for (auto p : parts) total += p.size();
        if (write_buf_.size() + total > params_.flow.write_high_watermark)
            return apply_overflow_parts(parts);
        for (auto p : parts) {
            auto w = write_buf_.writable(p.size());
            std::memcpy(w.data(), p.data(), p.size());
            write_buf_.commit(p.size());
        }
        em_->note_write_pending(sink_);
        check_write_watermark();
        return SendResult::Queued;
    }

    void shutdown_write() {
        if (state_ != ConnState::Established) return;
        state_ = ConnState::ShutdownWrite;
        if (fd_ >= 0) ::shutdown(fd_, SHUT_WR);
    }

    void close(CloseReason r = CloseReason::LocalClose) {
        if (state_ == ConnState::Closing || state_ == ConnState::Closed) return;
        state_ = ConnState::Closing;
        teardown(r);
    }

    // Cross-thread handle (§13): send() posts the bytes to the owning EM.
    struct Handle {
        Mailbox mb;
        ConnId id;
        EM* em;

        PostResult send(ByteSpan bytes) const {
            std::vector<std::byte> copy(bytes.begin(), bytes.end());
            return mb.post([em = em, id = id, b = std::move(copy)]() mutable {
                if (auto* c = Connection::resolve(*em, id))
                    (void)c->send(ByteSpan(b.data(), b.size()));
            });
        }
        PostResult close() const {
            return mb.post([em = em, id = id] {
                if (auto* c = Connection::resolve(*em, id))
                    c->close(CloseReason::LocalClose);
            });
        }
    };
    Handle handle() const noexcept { return {em_->mailbox(), id_, em_}; }

    // ---- engine side
    // -------------------------------------------------------------
    static void dispatch(void* obj, OpKind kind, const Completion& c) {
        static_cast<Connection*>(obj)->on_completion(kind, c);
    }

    void on_completion(OpKind kind, const Completion& c) {
        switch (kind) {
            case OpKind::Connect:
                on_connect_result(c);
                break;
            case OpKind::Recv:
                on_recv(c);
                break;
            case OpKind::Send:
                on_sent(c);
                break;
            case OpKind::FlushWrite:
                flush_write();
                break;
            default:
                break;
        }
    }

    // ---- coroutine session mode (M9-04, coro/conn.hpp)
    // --------------------------------------------------
    // enable_coro() hands the connection's pacing to a session coroutine:
    // a recv completion delivers one batch to the parked waiter and the
    // socket read is NOT re-armed — the next recv() drives the next read.
    // Batches parsed while no waiter is parked (a multi-batch read) stash
    // into `held` and are handed to the next recv() without a socket read;
    // held is bounded and an overflow closes the connection.
    void enable_coro() noexcept { coro_.on = true; }
    bool coro_mode() const noexcept { return coro_.on; }

    // Ready check for RecvOp::await_ready — drains the held stash first.
    bool coro_recv_ready(std::span<const Message>* out) noexcept {
        if (coro_.held.empty()) return false;
        coro_.delivered = std::move(coro_.held);
        coro_.held.clear();
        *out = std::span<const Message>(coro_.delivered);
        return true;
    }
    // Park the coroutine until the next batch. Arms the read unless one is
    // already in flight (a re-park inside the current parse loop).
    void coro_await_recv(std::coroutine_handle<> h, coro::PromiseBase* p,
                         Result<std::span<const Message>>* out) {
        coro_.recv_h = h;
        coro_.recv_p = p;
        coro_.recv_out = out;
        if (!coro_.armed) {
            coro_.armed = true;
            arm_recv();
        }
    }
    void coro_unawait_recv() noexcept {
        coro_.recv_h = {};
        coro_.recv_p = nullptr;
        coro_.recv_out = nullptr;
    }
    // SendOp: park until the write side drains below the low watermark.
    void coro_await_send(std::coroutine_handle<> h, coro::PromiseBase* p,
                         ByteSpan bytes, SendResult* out) {
        coro_.send_h = h;
        coro_.send_p = p;
        coro_.send_bytes = bytes;
        coro_.send_out = out;
    }
    void coro_unawait_send() noexcept {
        coro_.send_h = {};
        coro_.send_p = nullptr;
        coro_.send_out = nullptr;
    }

    // Outbound connect finished: become Established or die (§13.1).
    void on_connect_result(const Completion& c) {
        if (state_ != ConnState::Connecting) return;
        if (c.result < 0) {
            if (c.result == -ECANCELED) return;
            close(CloseReason::ConnectFailed);
            return;
        }
        Peer p{};
        if (auto a = sock::peer_addr(fd_); a) p.addr = *a;
        start(p);
    }

  private:
    // ---- recv path
    // ---------------------------------------------------------------
    void arm_recv() {
        if (read_paused_ || state_ != ConnState::Established) return;
        auto w = read_buf_.writable(std::max<std::size_t>(parse_need_, 4096));
        em_->submit_recv(sink_, fd_, w);
    }

    void on_recv(const Completion& c) {
        if (coro_.on) coro_.armed = false;  // the in-flight read completed
        if (state_ != ConnState::Established) return;
        if (c.result < 0) {
            if (c.result == -EAGAIN || c.result == -ECANCELED) {
                rearm_recv();
                return;
            }
            close(CloseReason::Error);
            return;
        }
        if (c.result == 0) {
            close(CloseReason::PeerFin);
            return;
        }

        read_buf_.commit(std::size_t(c.result));
        em_->stats().bytes_in += std::uint64_t(c.result);
        em_->recorder().record(EventKind::Recv, id_.idx,
                               std::uint32_t(c.result));
        arm_idle_timer();

        // Framer inlined into the read loop (§14); batches capped by the
        // scratch vector's capacity, dispatched and refilled as needed.
        batch_.clear();
        for (;;) {
            auto pr = framer_.parse(read_buf_.readable());
            using Kind = typename std::decay_t<decltype(pr)>::Kind;
            if (pr.kind == Kind::NeedMore) {
                parse_need_ = pr.need;
                break;
            }
            if (pr.kind == Kind::Error) {
                ++em_->stats().frame_errors;
                if (handlers_->on_error) handlers_->on_error(id_, pr.error);
                close(CloseReason::FrameError);
                return;
            }
            batch_.push_back(pr.message);
            read_buf_.consume(pr.consumed);
            parse_need_ = 0;
            if (batch_.size() == batch_.capacity()) {
                dispatch_batch();
                batch_.clear();
                if (state_ != ConnState::Established) return;
            }
        }
        if (!batch_.empty()) dispatch_batch();
        // Coroutine sessions pace reads via recv(); callback mode re-arms.
        if (state_ == ConnState::Established && !coro_.on) arm_recv();
    }

    void dispatch_batch() {
        ++em_->stats().msgs_in;
        if (coro_.on) {
            if (coro_.recv_h) {
                auto h = coro_.recv_h;
                auto* p = coro_.recv_p;
                auto* out = coro_.recv_out;
                coro_unawait_recv();
                *out = std::span<const Message>(batch_);
                p->resume_under_ctx(h);  // session may re-park mid-parse
            } else if (coro_.held.size() + batch_.size() <= kCoroHeldCap) {
                coro_.held.insert(coro_.held.end(), batch_.begin(),
                                  batch_.end());
            } else {
                close(CloseReason::HeldOverflow);
            }
            return;
        }
        if (handlers_->on_messages)
            handlers_->on_messages(id_, std::span<const Message>(batch_));
    }

    // Arm a read honoring coroutine pacing: in coro mode this is the only
    // submit path and it is deduplicated by coro_.armed.
    void rearm_recv() {
        if (coro_.on) {
            if (!coro_.armed) {
                coro_.armed = true;
                arm_recv();
            }
        } else {
            arm_recv();
        }
    }

    // ---- send path
    // -----------------------------------------------------------------
    void flush_write() {
        if (write_inflight_ || write_buf_.empty()) return;
        if (state_ != ConnState::Established &&
            state_ != ConnState::ShutdownWrite)
            return;
        ByteSpan b = write_buf_.readable();
        write_inflight_ = b.size();
        em_->submit_send(sink_, fd_, b);
    }

    void on_sent(const Completion& c) {
        if (c.result < 0) {
            write_inflight_ = 0;
            if (c.result == -ECANCELED) return;
            close(CloseReason::Error);
            return;
        }
        write_inflight_ = 0;
        write_buf_.consume(std::size_t(c.result));
        em_->stats().bytes_out += std::uint64_t(c.result);
        em_->recorder().record(EventKind::Send, id_.idx,
                               std::uint32_t(c.result));

        if (backpressured_ &&
            write_buf_.size() <= params_.flow.write_low_watermark) {
            backpressured_ = false;
            if (read_paused_) {
                read_paused_ = false;
                rearm_recv();
            }
            if (coro_.on) {
                coro_writable();
            } else if (handlers_->on_writable &&
                       state_ == ConnState::Established) {
                handlers_->on_writable(id_);
            }
        }
        if (!write_buf_.empty()) em_->note_write_pending(sink_);
    }

    SendResult apply_overflow(ByteSpan bytes) {
        ++em_->stats().write_hwm_hits;
        switch (params_.flow.on_overflow) {
            case WriteOverflow::Disconnect:
                close(CloseReason::WriteOverflow);
                return SendResult::Closed;
            case WriteOverflow::DropNewest:
                ++em_->stats().write_drops;
                return SendResult::Dropped;
            case WriteOverflow::DropOldest: {
                ++em_->stats().write_drops;
                write_buf_.consume(bytes.size());  // drop the oldest prefix
                auto w = write_buf_.writable(bytes.size());
                std::memcpy(w.data(), bytes.data(), bytes.size());
                write_buf_.commit(bytes.size());
                em_->note_write_pending(sink_);
                return SendResult::Queued;
            }
            case WriteOverflow::StopReading: {
                pause_reads();
                auto w = write_buf_.writable(bytes.size());
                std::memcpy(w.data(), bytes.data(), bytes.size());
                write_buf_.commit(bytes.size());
                em_->note_write_pending(sink_);
                return SendResult::Backpressured;
            }
        }
        return SendResult::Dropped;
    }

    SendResult apply_overflow_parts(std::span<const ByteSpan> parts) {
        ++em_->stats().write_hwm_hits;
        switch (params_.flow.on_overflow) {
            case WriteOverflow::Disconnect:
                close(CloseReason::WriteOverflow);
                return SendResult::Closed;
            case WriteOverflow::DropNewest:
                ++em_->stats().write_drops;
                return SendResult::Dropped;
            case WriteOverflow::DropOldest: {
                ++em_->stats().write_drops;
                for (auto p : parts) write_buf_.consume(p.size());
                [[fallthrough]];
            }
            case WriteOverflow::StopReading:
                if (params_.flow.on_overflow == WriteOverflow::StopReading)
                    pause_reads();
                for (auto p : parts) {
                    auto w = write_buf_.writable(p.size());
                    std::memcpy(w.data(), p.data(), p.size());
                    write_buf_.commit(p.size());
                }
                em_->note_write_pending(sink_);
                return params_.flow.on_overflow == WriteOverflow::StopReading
                           ? SendResult::Backpressured
                           : SendResult::Queued;
        }
        return SendResult::Dropped;
    }

    // Coroutine-mode writable edge: retry the parked send once the write
    // side drains. If the retry still hits the watermark the waiter stays
    // parked for the next edge.
    void coro_writable() {
        if (!coro_.send_h) return;
        SendResult r = send(coro_.send_bytes);
        if (r == SendResult::Backpressured) return;  // still full; re-park
        auto h = coro_.send_h;
        auto* p = coro_.send_p;
        auto* out = coro_.send_out;
        coro_unawait_send();
        *out = r;
        p->resume_under_ctx(h);
    }

    void pause_reads() {
        read_paused_ = true;
        em_->backend_cancel(sink_, OpKind::Recv);
    }
    void check_write_watermark() {
        if (!backpressured_ &&
            write_buf_.size() >= params_.flow.write_high_watermark) {
            backpressured_ = true;
            ++em_->stats().write_hwm_hits;
            if (params_.flow.auto_pause_reads) pause_reads();
            em_->recorder().record(EventKind::Backpressure, id_.idx,
                                   std::uint32_t(write_buf_.size()));
        }
    }

    // ---- idle timers
    // ------------------------------------------------------------------
    void arm_idle_timer() {
        if (params_.idle_read_timeout.count() == 0) return;
        if (idle_timer_.valid()) em_->cancel(idle_timer_);
        ConnId me = id_;
        EM* em = em_;
        idle_timer_ = em_->after(
            params_.idle_read_timeout,
            [em, me](TimerCtx) {
                if (auto* c = Connection::resolve(*em, me))
                    c->close(CloseReason::IdleTimeout);
            },
            group_);
    }

    // ---- teardown
    // -------------------------------------------------------------------------
    void teardown(CloseReason r) {
        if (fd_ >= 0) {
            em_->backend_detach(fd_);
            ::close(fd_);
            fd_ = -1;
        }
        if (group_.valid()) {
            em_->cancel_group(group_);
            group_ = {};
        }
        ++em_->stats().conns_closed;
        if (handlers_->on_close) handlers_->on_close(id_, r);
        SinkHandle s = sink_;
        sink_ = {};
        em_->release_sink(s);
        state_ = ConnState::Closed;
        // Wake coroutine waiters after the state flips to Closed — the
        // resumed session observes the closed conn via ConnRef::valid().
        if (coro_.recv_h) {
            auto h = coro_.recv_h;
            auto* p = coro_.recv_p;
            auto* out = coro_.recv_out;
            coro_unawait_recv();
            *out = make_error(ErrorCategory::Net, Err::Closed);
            p->resume_under_ctx(h);
        }
        if (coro_.send_h) {
            auto h = coro_.send_h;
            auto* p = coro_.send_p;
            auto* out = coro_.send_out;
            coro_unawait_send();
            *out = SendResult::Closed;
            p->resume_under_ctx(h);
        }
        // Notify the owner via defer so the connection's memory is never
        // freed out from under the callback that initiated the close. That
        // guarantee only matters while the loop is actively iterating: once
        // the EM has stopped running (e.g. teardown() reached from the EM's
        // own destructor, possibly on a different thread than the one that
        // ran the loop), nothing will ever drain defer_next_ again, so
        // deferring here would silently drop the notification. In that case
        // there is also no concurrent caller to protect against reentrancy,
        // so it is safe to notify synchronously instead.
        void* owner = owner_;
        auto* notify = notify_;
        ConnId id = id_;
        if (em_->is_running())
            em_->defer([owner, notify, id, r] { notify(owner, id, r); });
        else
            notify(owner, id, r);
    }

    // Coroutine session state (M9-04). Zero-cost in callback mode except the
    // two vectors, which stay empty.
    static constexpr std::size_t kCoroHeldCap = 256;
    struct CoroState {
        std::coroutine_handle<> recv_h{};
        coro::PromiseBase* recv_p = nullptr;
        Result<std::span<const Message>>* recv_out = nullptr;
        std::coroutine_handle<> send_h{};
        coro::PromiseBase* send_p = nullptr;
        SendResult* send_out = nullptr;
        ByteSpan send_bytes{};
        std::vector<Message> held;       // unpaced-batch stash (bounded)
        std::vector<Message> delivered;  // stable storage for held returns
        bool armed = false;              // a recv submit is in flight
        bool on = false;                 // coroutine session owns this conn
    };
    CoroState coro_{};

    EM* em_;
    int fd_ = -1;
    ConnId id_{};
    SinkHandle sink_{};
    Handlers<P>* handlers_;
    Framer framer_;
    Params params_;
    IoBuffer read_buf_;
    IoBuffer write_buf_;
    std::size_t write_inflight_ = 0;
    std::size_t parse_need_ = 0;
    bool backpressured_ = false;
    bool read_paused_ = false;
    ConnState state_ = ConnState::Connecting;
    Peer peer_{};
    TimerGroup group_{};
    TimerId idle_timer_{};
    std::vector<Message> batch_;
    void* owner_;
    void (*notify_)(void*, ConnId, CloseReason);
    static inline char type_tag_{};  // one per <P, EM> instantiation
};

}  // namespace afx

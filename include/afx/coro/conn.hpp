#pragma once

// coro/conn.hpp — ConnRef and the connection awaiters (M9-03/04).
//
// A coroutine session owns its connection's pacing: `co_await c.recv()`
// returns one parsed batch (span<const Message>); while a recv is
// outstanding the socket read is armed, and once a batch is delivered the
// connection does NOT re-arm — the next recv() drives the next read.
// Backpressure is therefore free: a session that stops awaiting recv stops
// reading.
//
// Lifetime contract: a delivered batch's Message bodies point into the
// connection's read buffer — valid until the next recv() *completes*
// (issuing the call is safe; the span dies when new data lands). The
// session must consume the batch before awaiting anything that can re-arm
// reads — in practice: before the next recv().

#include <coroutine>
#include <span>

#include "afx/coro/ops.hpp"
#include "afx/coro/task.hpp"
#include "afx/net/connection.hpp"
#include "afx/net/tcp_client.hpp"
#include "afx/net/tcp_server.hpp"
#include "afx/sys/result.hpp"

namespace afx {

namespace coro {

// ---- recv ---------------------------------------------------------------
template <class P, class EM>
class RecvOp {
    using Conn = Connection<P, EM>;

  public:
    using Batch = std::span<const typename Conn::Message>;

    explicit RecvOp(EM& em, ConnId id) : em_(&em), id_(id) {}
    ~RecvOp() {
        if (parked_ && conn_) conn_->coro_unawait_recv();
    }

    bool await_ready() noexcept {
        conn_ = Conn::resolve(*em_, id_);
        if (!conn_) {
            out_ = make_error(ErrorCategory::Net, Err::Closed);
            return true;
        }
        Batch s;
        if (conn_->coro_recv_ready(&s)) {
            out_ = s;
            return true;
        }
        return false;
    }
    template <class PP>
    bool await_suspend(std::coroutine_handle<PP> h) noexcept {
        p_ = &h.promise();
        h_ = h;
        if (p_->stop_requested()) {
            out_ = cancelled_err();
            return false;
        }
        p_->set_cancel(&cancel_thunk, this);
        parked_ = true;
        conn_->coro_await_recv(h, p_, &out_);
        return true;
    }
    Result<Batch> await_resume() noexcept {
        if (p_) {  // null when await_ready short-circuited
            p_->clear_cancel();
            parked_ = false;
        }
        return out_;
    }

  private:
    static void cancel_thunk(void* a) noexcept {
        auto* s = static_cast<RecvOp*>(a);
        s->conn_->coro_unawait_recv();
        s->parked_ = false;
        s->out_ = cancelled_err();
        s->p_->resume_under_ctx(s->h_);
    }

    EM* em_;
    ConnId id_;
    Conn* conn_ = nullptr;
    PromiseBase* p_ = nullptr;
    std::coroutine_handle<> h_{};
    Result<Batch> out_{make_error(ErrorCategory::Net, Err::Closed)};
    bool parked_ = false;
};

// ---- send ----------------------------------------------------------------
// Queues bytes; suspends only on Backpressured (write side full), resuming
// when the connection drains below the low watermark. Resolves to
// SendResult — Queued/Sent mean success.
template <class P, class EM>
class SendOp {
    using Conn = Connection<P, EM>;

  public:
    SendOp(EM& em, ConnId id, ByteSpan bytes) : em_(&em), id_(id), b_(bytes) {}
    ~SendOp() {
        if (parked_ && conn_) conn_->coro_unawait_send();
    }

    bool await_ready() noexcept {
        conn_ = Conn::resolve(*em_, id_);
        if (!conn_) {
            out_ = SendResult::Closed;
            return true;
        }
        out_ = conn_->send(b_);
        return out_ != SendResult::Backpressured;
    }
    template <class PP>
    bool await_suspend(std::coroutine_handle<PP> h) noexcept {
        p_ = &h.promise();
        h_ = h;
        if (p_->stop_requested()) {
            out_ = SendResult::Closed;
            cancelled_ = true;
            return false;
        }
        p_->set_cancel(&cancel_thunk, this);
        parked_ = true;
        conn_->coro_await_send(h, p_, b_, &out_);
        return true;
    }
    SendResult await_resume() noexcept {
        if (p_) {  // null when await_ready short-circuited
            p_->clear_cancel();
            parked_ = false;
        }
        if (cancelled_) return SendResult::Closed;
        return out_;
    }

  private:
    static void cancel_thunk(void* a) noexcept {
        auto* s = static_cast<SendOp*>(a);
        s->conn_->coro_unawait_send();
        s->parked_ = false;
        s->cancelled_ = true;
        s->p_->resume_under_ctx(s->h_);
    }

    EM* em_;
    ConnId id_;
    ByteSpan b_;
    Conn* conn_ = nullptr;
    PromiseBase* p_ = nullptr;
    std::coroutine_handle<> h_{};
    SendResult out_ = SendResult::Closed;
    bool parked_ = false;
    bool cancelled_ = false;
};

// ---- connect ---------------------------------------------------------------
// `co_await connect(*client)` → Result<ConnRef<P,EM>>. Suspends until the
// client's state machine reports Connected (the conn is then Established)
// or Disconnected (all resolved addresses exhausted). Uses the client's
// on_state_change slot — don't combine with a user state callback.
template <class P, class EM>
class ConnectOp {
  public:
    explicit ConnectOp(TcpClient<P, EM>* cli) : cli_(cli) {}
    ~ConnectOp() {
        if (parked_) cli_->on_state_change({});
    }

    bool await_ready() noexcept {
        return cli_->state() == ClientState::Connected ||
               cli_->state() == ClientState::Disconnected;
    }
    template <class PP>
    bool await_suspend(std::coroutine_handle<PP> h) noexcept {
        p_ = &h.promise();
        h_ = h;
        if (p_->stop_requested()) {
            err_ = cancelled_err();
            return false;
        }
        p_->set_cancel(&cancel_thunk, this);
        parked_ = true;
        cli_->on_state_change([this](ClientState s) { on_state(s); });
        return true;
    }
    Result<ConnRef<P, EM>> await_resume() noexcept {
        if (p_) {  // null when await_ready short-circuited
            p_->clear_cancel();
            parked_ = false;
        }
        if (err_) return err_;
        if (cli_->conn())
            return ConnRef<P, EM>(cli_->conn()->em(), cli_->conn()->id());
        return make_error(ErrorCategory::Net, Err::ConnectFailed);
    }

  private:
    void on_state(ClientState s) {
        if (!parked_) return;
        if (s == ClientState::Connected || s == ClientState::Disconnected) {
            if (s == ClientState::Disconnected)
                err_ = make_error(ErrorCategory::Net, Err::ConnectFailed);
            parked_ = false;
            cli_->on_state_change({});
            p_->clear_cancel();
            p_->resume_under_ctx(h_);
        }
    }
    static void cancel_thunk(void* a) noexcept {
        auto* s = static_cast<ConnectOp*>(a);
        s->cli_->on_state_change({});
        s->parked_ = false;
        s->err_ = cancelled_err();
        s->p_->resume_under_ctx(s->h_);
    }

    TcpClient<P, EM>* cli_;
    PromiseBase* p_ = nullptr;
    std::coroutine_handle<> h_{};
    Error err_{};
    bool parked_ = false;
};

}  // namespace coro

// ConnRef<P, EM>: the coroutine-facing connection handle (DESIGN.md §17).
// Carries the EM so `session(ConnRef)` coroutines find their frame
// allocator (first-parameter convention, spike 0002).
template <class P, class EM>
class ConnRef {
  public:
    using Conn = Connection<P, EM>;
    using Message = typename Conn::Message;
    using Batch = std::span<const Message>;

    ConnRef() = default;
    ConnRef(EM& em, ConnId id) noexcept : em_(&em), id_(id) {}

    EM& em() const noexcept { return *em_; }
    ConnId id() const noexcept { return id_; }

    bool valid() const noexcept { return em_ && Conn::resolve(*em_, id_); }
    Conn* resolve() const noexcept { return Conn::resolve(*em_, id_); }

    coro::RecvOp<P, EM> recv() const noexcept {
        return coro::RecvOp<P, EM>(*em_, id_);
    }
    coro::SendOp<P, EM> send(ByteSpan b) const noexcept {
        return coro::SendOp<P, EM>(*em_, id_, b);
    }
    void close() const noexcept {
        if (auto* c = resolve()) c->close();
    }
    void shutdown_write() const noexcept {
        if (auto* c = resolve()) c->shutdown_write();
    }

  private:
    EM* em_ = nullptr;
    ConnId id_{};
};

}  // namespace afx

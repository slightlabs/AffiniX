#pragma once

// coro/ops.hpp — EM-level awaitables (M9-04): sleep and post_and_reply.
// They drive the owning EM through the type-erased EngineOps so the same
// code serves every BasicEventManager instantiation. Every awaiter:
//   * resumes on the EM it suspended on (timer fire / mailbox hop),
//   * refuses to suspend once stop is requested (fail-fast, §7.4),
//   * resumes under the task's ambient Context.
//
// Cancellation leaves a Cancelled-category Result; nothing throws.

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "afx/coro/task.hpp"
#include "afx/itc/mailbox.hpp"
#include "afx/sys/result.hpp"
#include "afx/sys/types.hpp"

namespace afx::coro {

inline Error cancelled_err() noexcept {
    return make_error(ErrorCategory::Cancelled, Err::Stopped);
}

// ---- sleep ---------------------------------------------------------------
// `co_await em.sleep(d)` → Result<void>. Resolution is the EM's timer_tick,
// same as callback-mode after().
class SleepOp {
  public:
    SleepOp(EngineOps* eng, Duration d) noexcept : eng_(eng), d_(d) {}
    ~SleepOp() {
        // Frame destroyed while still suspended (EM teardown path): retract
        // the timer so it never fires into a dead frame.
        if (token_ && eng_ && eng_->timer_cancel)
            eng_->timer_cancel(eng_->em, token_);
    }

    bool await_ready() noexcept { return false; }
    template <class P>
    bool await_suspend(std::coroutine_handle<P> h) noexcept {
        p_ = &h.promise();
        h_ = h;
        if (p_->stop_requested()) {
            out_ = cancelled_err();
            return false;
        }
        // Install the cancel thunk BEFORE arming: a request_stop landing
        // between the two would otherwise miss the interruption window.
        p_->set_cancel(&cancel_thunk, this);
        token_ = eng_->timer_after(eng_->em, d_, this, &fire_thunk);
        return true;
    }
    Result<void> await_resume() noexcept {
        p_->clear_cancel();
        token_ = nullptr;
        return out_;
    }

  private:
    static void fire_thunk(void* a) noexcept {
        auto* s = static_cast<SleepOp*>(a);
        s->token_ = nullptr;
        s->p_->clear_cancel();
        s->p_->resume_under_ctx(s->h_);
    }
    static void cancel_thunk(void* a) noexcept {
        auto* s = static_cast<SleepOp*>(a);
        void* t = s->token_;
        s->token_ = nullptr;
        s->eng_->timer_cancel(s->eng_->em, t);
        s->out_ = cancelled_err();
        s->p_->resume_under_ctx(s->h_);
    }

    EngineOps* eng_;
    Duration d_;
    PromiseBase* p_ = nullptr;
    std::coroutine_handle<> h_{};
    void* token_ = nullptr;  // TimerId packed by the EM thunk
    Result<void> out_{};
};

// ---- post_and_reply ------------------------------------------------------
// `co_await em.post_and_reply(target_mb, fn)` → Result<R>: `fn` runs on the
// target EM's thread; the result hops back through the home mailbox and
// resumes the coroutine here.
//
// The exchange state is a shared node, not the coroutine frame: the remote
// work item and the reply hop can outlive the awaiting frame (EM teardown
// may destroy a suspended root while its request is in flight). The remote
// side writes into the node; the reply resumes the coroutine only if the
// waiter is still alive. One heap allocation per call — post_and_reply is a
// cross-EM operation and the mailbox itself is shared_ptr-backed already.
template <class F>
struct PostReplyState {
    using R = std::invoke_result_t<F>;
    std::conditional_t<std::is_void_v<R>, char, std::optional<R>> res{};
    F f{};  // lives in the node: survives the awaiting frame
    std::exception_ptr ep{};
    Error err{};
    // Waiter side — valid only while `waiter_alive`; cleared by the op's
    // destructor when the frame dies before the reply lands.
    PromiseBase* p = nullptr;
    std::coroutine_handle<> h{};
    std::weak_ptr<MailboxImpl> home{};
    bool cancelled = false;
    bool waiter_alive = false;
};

template <class F>
class PostReplyOp {
    using State = PostReplyState<F>;
    using R = typename State::R;
    static_assert(std::is_move_constructible_v<R> || std::is_void_v<R>,
                  "post_and_reply result must be movable");

  public:
    PostReplyOp(Mailbox target, F f)
        : target_(target), st_(std::make_shared<State>()) {
        st_->f = std::move(f);
    }
    ~PostReplyOp() {
        st_->waiter_alive = false;  // frame died first: reply drops
    }

    bool await_ready() noexcept { return false; }
    template <class P>
    bool await_suspend(std::coroutine_handle<P> h) noexcept {
        st_->p = static_cast<PromiseBase*>(&h.promise());
        st_->h = h;
        if (st_->p->stop_requested()) {
            st_->err = cancelled_err();
            return false;
        }
        EngineOps* e = st_->p->engine;
        st_->home = e->home_mb;
        st_->waiter_alive = true;
        st_->p->set_cancel(&cancel_thunk, st_.get());
        PostResult pr =
            target_.post([st = st_] { run_remote(std::move(st)); });
        if (pr != PostResult::Ok) {
            st_->waiter_alive = false;
            st_->p->clear_cancel();
            st_->err = make_error(ErrorCategory::Itc,
                                  pr == PostResult::Full ? Err::Full
                                                         : Err::Closed);
            return false;
        }
        return true;
    }
    Result<R> await_resume() noexcept(std::is_void_v<R>) {
        st_->p->clear_cancel();
        st_->waiter_alive = false;
        if constexpr (std::is_void_v<R>) {
            if (st_->ep) std::rethrow_exception(st_->ep);
            return st_->err.ok() ? Result<void>{} : Result<void>(st_->err);
        } else {
            if (!st_->err.ok()) return st_->err;
            if (st_->ep) std::rethrow_exception(st_->ep);
            return Result<R>(std::move(*st_->res));
        }
    }

  private:
    static void run_remote(std::shared_ptr<State> st) {
        try {
            if constexpr (std::is_void_v<R>)
                st->f();
            else
                st->res.emplace(st->f());
        } catch (...) {
            st->ep = std::current_exception();
        }
        auto home = st->home.lock();
        if (!home) return;  // home EM is gone; nothing to resume
        Mailbox mb(home);
        (void)mb.post([st] { reply(*st); });
    }

    static void reply(State& st) noexcept {
        if (!st.waiter_alive) return;  // frame already destroyed
        if (st.cancelled) st.err = cancelled_err();
        st.p->clear_cancel();
        st.p->resume_under_ctx(st.h);
    }
    static void cancel_thunk(void* a) noexcept {
        // The reply hop cannot be retracted; mark and let it resume the
        // coroutine with a Cancelled error when it lands.
        static_cast<State*>(a)->cancelled = true;
    }

    Mailbox target_;
    std::shared_ptr<State> st_;
};

// `co_await afx::coro::post_and_reply(target_mb, fn)`
template <class F>
auto post_and_reply(Mailbox target, F&& f) {
    return PostReplyOp<std::decay_t<F>>(target, std::forward<F>(f));
}

}  // namespace afx::coro

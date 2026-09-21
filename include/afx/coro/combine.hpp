#pragma once

// coro/combine.hpp — task combinators (M9-05): when_all, when_any,
// with_timeout. All are single-EM: children inherit the awaiting task's
// engine and context (deadlines tighten, stop tokens share — §7.4). Each
// child's completion lands via the promise's done_fn hook; the last (or
// first, for when_any) resumes the parent — synchronously when it finished
// inline, via resume_under_ctx otherwise.
//
// Results: when_all → Result<tuple<wrapped Ts...>>, when_any →
// Result<pair<index, variant<wrapped Ts...>>>. A child that stored an
// exception rethrows it from await_resume. Cancellation requests propagate
// into every still-running child via promise request_stop().

#include <array>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <tuple>
#include <utility>
#include <variant>

#include "afx/coro/ops.hpp"
#include "afx/coro/task.hpp"
#include "afx/sys/result.hpp"

namespace afx::coro {

namespace detail {
// void-producing children contribute monostate to result tuples/variants.
template <class T>
struct Wrap {
    using type = T;
};
template <>
struct Wrap<void> {
    using type = std::monostate;
};
template <class T>
using wrap_t = typename Wrap<T>::type;

template <class T>
wrap_t<T> take_child(Task<T>& t) {
    if constexpr (std::is_void_v<T>) {
        t.promise().take();
        return std::monostate{};
    } else {
        return std::move(t.promise()).take();
    }
}
}  // namespace detail

// ---- when_all --------------------------------------------------------------
template <class... Ts>
class WhenAllOp {
    static constexpr std::size_t N = sizeof...(Ts);
    using Out = std::tuple<detail::wrap_t<Ts>...>;

  public:
    explicit WhenAllOp(Task<Ts>... ts) : tasks_(std::move(ts)...) {}
    ~WhenAllOp() {
        // Frame teardown while suspended: cancel the children but never
        // resume the (dying) parent from inside our own destructor.
        dead_ = true;
        cancel_children();
    }

    bool await_ready() noexcept { return N == 0; }

    template <class PP>
    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<PP> h) noexcept {
        parent_ = &h.promise();
        parent_h_ = h;
        parent_->set_cancel(&cancel_thunk, this);
        start_all(std::index_sequence_for<Ts...>{});
        if (wake_) return parent_h_;
        parent_suspended_ = true;
        return std::noop_coroutine();
    }

    Result<Out> await_resume() {
        if (!parent_) return Out{};  // N == 0: never suspended
        parent_->clear_cancel();
        if (parent_->stop_requested()) return cancelled_err();
        return collect(std::index_sequence_for<Ts...>{});
    }

  private:
    template <std::size_t I>
    static void done_thunk(PromiseBase*, void* a) noexcept {
        auto* s = static_cast<WhenAllOp*>(a);
        if (s->dead_) return;
        if (--s->remaining_ == 0) {
            if (s->parent_suspended_)
                s->parent_->resume_under_ctx(s->parent_h_);
            else
                s->wake_ = true;
        }
    }

    template <std::size_t... Is>
    static auto thunk_table(std::index_sequence<Is...>) {
        return std::array<void (*)(PromiseBase*, void*), N>{&done_thunk<Is>...};
    }

    template <std::size_t... Is>
    void start_all(std::index_sequence<Is...>) {
        static const auto kThunks =
            thunk_table(std::index_sequence_for<Ts...>{});
        (start_one<Is>(kThunks[Is]), ...);
    }

    template <std::size_t I>
    void start_one(void (*thunk)(PromiseBase*, void*)) {
        auto& t = std::get<I>(tasks_);
        auto& p = t.promise();
        p.engine = parent_->engine;
        p.ctx.trace = parent_->ctx.trace;
        p.ctx.deadline = parent_->ctx.deadline.earliest_of(p.ctx.deadline);
        if (parent_->ctx.stop) p.ctx.stop = parent_->ctx.stop;
        p.stopped_ = p.stopped_ || parent_->stopped_;
        p.done_fn = thunk;
        p.done_arg = this;
        p.resume_under_ctx(t.handle());
    }

    void cancel_children() noexcept {
        std::apply([](auto&... t) { (t.promise().request_stop(), ...); },
                   tasks_);
    }

    template <std::size_t... Is>
    Result<Out> collect(std::index_sequence<Is...>) {
        return Out{detail::take_child(std::get<Is>(tasks_))...};
    }

    static void cancel_thunk(void* a) noexcept {
        static_cast<WhenAllOp*>(a)->cancel_children();
    }

    std::tuple<Task<Ts>...> tasks_;
    PromiseBase* parent_ = nullptr;
    std::coroutine_handle<> parent_h_{};
    std::size_t remaining_ = N;
    bool parent_suspended_ = false;
    bool wake_ = false;
    bool dead_ = false;
};

template <class... Ts>
auto when_all(Task<Ts>... ts) {
    return WhenAllOp<Ts...>(std::move(ts)...);
}

// ---- when_any
// ---------------------------------------------------------------- First child
// to finish wins; the rest get request_stop() and unwind inline.
template <class... Ts>
class WhenAnyOp {
    static constexpr std::size_t N = sizeof...(Ts);
    using Out = std::pair<std::size_t, std::variant<detail::wrap_t<Ts>...>>;

  public:
    explicit WhenAnyOp(Task<Ts>... ts) : tasks_(std::move(ts)...) {}
    ~WhenAnyOp() {
        dead_ = true;
        cancel_children();
    }

    bool await_ready() noexcept { return false; }

    template <class PP>
    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<PP> h) noexcept {
        parent_ = &h.promise();
        parent_h_ = h;
        parent_->set_cancel(&cancel_thunk, this);
        start_all(std::index_sequence_for<Ts...>{});
        if (wake_) return parent_h_;
        parent_suspended_ = true;
        return std::noop_coroutine();
    }

    Result<Out> await_resume() {
        parent_->clear_cancel();
        if (parent_->stop_requested() && !won_) return cancelled_err();
        return winner_result(std::index_sequence_for<Ts...>{});
    }

  private:
    template <std::size_t I>
    static void done_thunk(PromiseBase*, void* a) noexcept {
        auto* s = static_cast<WhenAnyOp*>(a);
        if (s->dead_) return;
        if (s->won_) return;  // loser unwinding after cancellation
        s->won_ = true;
        s->winner_ = I;
        s->cancel_others(I);
        if (s->parent_suspended_)
            s->parent_->resume_under_ctx(s->parent_h_);
        else
            s->wake_ = true;
    }

    template <std::size_t... Is>
    static auto thunk_table(std::index_sequence<Is...>) {
        return std::array<void (*)(PromiseBase*, void*), N>{&done_thunk<Is>...};
    }
    template <std::size_t... Is>
    void start_all(std::index_sequence<Is...>) {
        static const auto kThunks =
            thunk_table(std::index_sequence_for<Ts...>{});
        (start_one<Is>(kThunks[Is]), ...);
    }
    template <std::size_t I>
    void start_one(void (*thunk)(PromiseBase*, void*)) {
        auto& p = std::get<I>(tasks_).promise();
        p.engine = parent_->engine;
        p.ctx.trace = parent_->ctx.trace;
        p.ctx.deadline = parent_->ctx.deadline.earliest_of(p.ctx.deadline);
        if (parent_->ctx.stop) p.ctx.stop = parent_->ctx.stop;
        p.stopped_ = p.stopped_ || parent_->stopped_;
        p.done_fn = thunk;
        p.done_arg = this;
        p.resume_under_ctx(std::get<I>(tasks_).handle());
    }
    void cancel_others(std::size_t except) {
        std::size_t i = 0;
        std::apply(
            [&](auto&... t) {
                ((i++ != except ? (t.promise().request_stop(), 0) : 0), ...);
            },
            tasks_);
    }
    void cancel_children() noexcept {
        std::apply([](auto&... t) { (t.promise().request_stop(), ...); },
                   tasks_);
    }

    template <std::size_t... Is>
    Result<Out> winner_result(std::index_sequence<Is...>) {
        Out out{};
        bool found = false;
        // Evaluate each candidate; only the winner's take() runs.
        std::exception_ptr ep;
        (
            [&] {
                if (found || winner_ != Is) return;
                found = true;
                try {
                    if constexpr (std::is_void_v<Ts>) {
                        std::get<Is>(tasks_).promise().take();
                        out.second.template emplace<Is>(std::monostate{});
                    } else {
                        out.second.template emplace<Is>(
                            std::move(std::get<Is>(tasks_).promise()).take());
                    }
                    out.first = Is;
                } catch (...) {
                    ep = std::current_exception();
                }
            }(),
            ...);
        if (ep) std::rethrow_exception(ep);
        return out;
    }

    static void cancel_thunk(void* a) noexcept {
        static_cast<WhenAnyOp*>(a)->cancel_children();
    }

    std::tuple<Task<Ts>...> tasks_;
    PromiseBase* parent_ = nullptr;
    std::coroutine_handle<> parent_h_{};
    std::size_t winner_ = N;
    bool won_ = false;
    bool parent_suspended_ = false;
    bool wake_ = false;
    bool dead_ = false;
};

template <class... Ts>
auto when_any(Task<Ts>... ts) {
    return WhenAnyOp<Ts...>(std::move(ts)...);
}

// ---- with_timeout
// -------------------------------------------------------------- `co_await
// with_timeout(d, task)` → Result<T>. On expiry the child gets request_stop()
// (its in-flight awaiter unwinds with Cancelled) and the parent resumes with
// Err::Expired.
template <class T>
class WithTimeoutOp {
  public:
    WithTimeoutOp(Duration d, Task<T> t) : d_(d), t_(std::move(t)) {}
    ~WithTimeoutOp() {
        dead_ = true;
        if (armed_ && eng_ && eng_->timer_cancel)
            eng_->timer_cancel(eng_->em, token_);
        // A still-parked child holds a done_fn into *this — cancel it before
        // we die; the dead_ flag keeps it from resuming the dying parent.
        if (!child_done_) t_.promise().request_stop();
    }

    bool await_ready() noexcept { return false; }

    template <class PP>
    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<PP> h) noexcept {
        parent_ = &h.promise();
        parent_h_ = h;
        if (parent_->stop_requested()) {
            err_ = cancelled_err();
            return h;
        }
        eng_ = parent_->engine;
        parent_->set_cancel(&cancel_thunk, this);
        auto& cp = t_.promise();
        cp.engine = eng_;
        cp.ctx.trace = parent_->ctx.trace;
        cp.ctx.deadline = parent_->ctx.deadline.earliest_of(cp.ctx.deadline);
        if (parent_->ctx.stop) cp.ctx.stop = parent_->ctx.stop;
        cp.done_fn = &child_thunk;
        cp.done_arg = this;
        armed_ = true;
        token_ = eng_->timer_after(eng_->em, d_, this, &timer_thunk);
        cp.resume_under_ctx(t_.handle());
        if (child_done_) return parent_h_;  // finished inline
        suspended_ = true;
        return std::noop_coroutine();
    }

    Result<T> await_resume() {
        parent_->clear_cancel();
        if (err_) return err_;
        if (timed_out_)
            return make_error(ErrorCategory::Cancelled, Err::Expired);
        if constexpr (std::is_void_v<T>) {
            t_.promise().take();
            return {};
        } else {
            return Result<T>(std::move(t_.promise()).take());
        }
    }

  private:
    // Single resumption point — the parent is resumed exactly once no
    // matter which path (child finish / timer / cancel) gets there first.
    void resume_parent() noexcept {
        if (resumed_ || dead_) return;
        resumed_ = true;
        if (suspended_) parent_->resume_under_ctx(parent_h_);
    }
    static void child_thunk(PromiseBase*, void* a) noexcept {
        auto* s = static_cast<WithTimeoutOp*>(a);
        s->child_done_ = true;
        if (s->timed_out_) return;  // timer path resumes the parent itself
        if (s->armed_) {
            s->eng_->timer_cancel(s->eng_->em, s->token_);
            s->armed_ = false;
        }
        s->resume_parent();
    }
    static void timer_thunk(void* a) noexcept {
        auto* s = static_cast<WithTimeoutOp*>(a);
        s->armed_ = false;
        s->timed_out_ = true;
        s->err_ = make_error(ErrorCategory::Cancelled, Err::Expired);
        // Unwind the child inline; its done_fn sees timed_out_ and returns.
        s->t_.promise().request_stop();
        s->resume_parent();
    }
    static void cancel_thunk(void* a) noexcept {
        auto* s = static_cast<WithTimeoutOp*>(a);
        if (s->armed_) {
            s->eng_->timer_cancel(s->eng_->em, s->token_);
            s->armed_ = false;
        }
        s->err_ = cancelled_err();
        s->t_.promise().request_stop();  // child unwinds inline
        s->resume_parent();
    }

    Duration d_;
    Task<T> t_;
    EngineOps* eng_ = nullptr;
    PromiseBase* parent_ = nullptr;
    std::coroutine_handle<> parent_h_{};
    void* token_ = nullptr;
    Error err_{};
    bool armed_ = false;
    bool timed_out_ = false;
    bool child_done_ = false;
    bool suspended_ = false;
    bool resumed_ = false;
    bool dead_ = false;
};

template <class T>
auto with_timeout(Duration d, Task<T> t) {
    return WithTimeoutOp<T>(d, std::move(t));
}

}  // namespace afx::coro

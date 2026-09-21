#pragma once

// coro/task.hpp — lazy Task<T> for the coroutine layer (DESIGN.md §17,
// ADR-0005). Opt-in over the callback core; nothing in the core depends on
// it. Every awaiter resumes on the EM it was suspended on.
//
// Frames allocate through the owning EM's FrameCache (arena-backed,
// size-bucketed — spike 0002): a coroutine's first parameter carries the EM
// (`session(ConnRef)`, `poller(EventManager&)`), which selects the
// arena-routed operator new. Tasks without an EM-carrying first parameter
// get ordinary heap frames. A frame never outlives its EM — EM teardown
// cancels and drains live roots before the arena dies.

#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

#include "afx/core/context.hpp"
#include "afx/sys/types.hpp"

namespace afx {

struct MailboxImpl;

template <class P, class EM>
class ConnRef;

namespace coro {

class PromiseBase;

// Type-erased handle the promise uses to talk to its owning EventManager
// (BasicEventManager is a template; the promise cannot name it). One
// instance lives inside each EM.
struct EngineOps {
    void* em = nullptr;
    void* (*frame_alloc)(void* em, std::size_t n) = nullptr;
    void (*frame_free)(void* em, void* p) = nullptr;
    // Root list: link at spawn, unlink at completion (self-destroy at
    // final_suspend makes the list purely observability + teardown).
    void (*root_link)(void* em, void* promise) = nullptr;
    void (*root_unlink)(void* em, void* promise) = nullptr;
    // Push/pop the task's Context as the EM's ambient context. The saved
    // slot pair lives in CtxSave on the resumer's stack — a task that runs
    // to final_suspend inside h.resume() frees its own frame, so the saved
    // pair cannot live in the promise (M9/ASAN).
    struct CtxSave {
        Context prev{};
        const Context* tls_prev = nullptr;
    };
    void (*ctx_push)(void* em, const Context& next, CtxSave* save) = nullptr;
    void (*ctx_pop)(void* em, const CtxSave& save) = nullptr;
    // Arm a one-shot timer; `fire(arg)` runs on the EM thread at expiry.
    // Returns a packed TimerId token for timer_cancel.
    void* (*timer_after)(void* em, Nanos d, void* arg,
                         void (*fire)(void*)) = nullptr;
    bool (*timer_cancel)(void* em, void* token) = nullptr;
    // Home mailbox for reply hops — weak so a dead home EM fails closed.
    std::weak_ptr<MailboxImpl> home_mb;
};

// Every frame — arena or heap — carries this header so sized/unsized
// operator delete routes back without naming the EM. 16 bytes keeps the
// frame 16-aligned for over-aligned members.
struct FrameHeader {
    EngineOps* ops;  // nullptr => plain heap frame
    void* pad;
};
static_assert(sizeof(FrameHeader) == 16);

class PromiseBase {
  public:
    EngineOps* engine = nullptr;     // owning EM; set at spawn / co_await
    std::coroutine_handle<> cont{};  // awaiting coroutine (symmetric xfer)
    Context ctx{};                   // task's ambient context (§7.4)

    // Cancellation into an in-flight awaiter: the awaiter installs a thunk
    // at suspend and clears it at resume. The flag itself lives here — no
    // shared_ptr state (StopSource allocates; a promise-local flag doesn't).
    void (*cancel_fn)(void* arg) = nullptr;
    void* cancel_arg = nullptr;
    bool stopped_ = false;

    // Intrusive list of spawned roots, for EM-teardown cancellation.
    PromiseBase* coro_prev = nullptr;
    PromiseBase* coro_next = nullptr;
    bool is_root = false;

    // EM-teardown path: destroy the suspended frame without naming the
    // promise's concrete type. Set by the promise constructor.
    void (*destroy_frame)(PromiseBase*) = nullptr;

    // Combinator hook (when_all/when_any/TaskScope): invoked at
    // final_suspend instead of symmetric-transferring to `cont`. Receives
    // this promise so the hook can identify which child finished; the hook
    // owns parent resumption.
    void (*done_fn)(PromiseBase* self, void* arg) = nullptr;
    void* done_arg = nullptr;
    // Combinator children are owned by their awaiter; scope children own
    // nobody's Task object, so they destroy their own frame at
    // final_suspend (after done_fn runs).
    bool self_destroy_ = false;

    bool stop_requested() const noexcept {
        return stopped_ || ctx.stop.stop_requested();
    }

    // Interrupt the in-flight awaiter (if cancellable) and mark the flag.
    // Always safe: a null thunk just sets the flag — the task sees
    // stop_requested() at its next suspend/resume boundary.
    void request_stop() noexcept {
        stopped_ = true;
        if (cancel_fn) {
            auto* f = cancel_fn;
            void* a = cancel_arg;
            cancel_fn = nullptr;
            cancel_arg = nullptr;
            f(a);
        }
    }

    void set_cancel(void (*f)(void*), void* a) noexcept {
        cancel_fn = f;
        cancel_arg = a;
    }
    void clear_cancel() noexcept {
        cancel_fn = nullptr;
        cancel_arg = nullptr;
    }

    // Resume `h` under this task's ambient context (§7.4 follows hops).
    void resume_under_ctx(std::coroutine_handle<> h) {
        if (engine && engine->ctx_push) {
            // Snapshot `engine` before resume: a task finishing inside
            // h.resume() destroys this promise's frame — nothing in *this
            // may be touched after the call.
            EngineOps* e = engine;
            EngineOps::CtxSave save;
            e->ctx_push(e->em, ctx, &save);
            h.resume();
            e->ctx_pop(e->em, save);
        } else {
            h.resume();
        }
    }
};

// Final suspension: symmetric transfer back to the awaiting coroutine (no
// stack growth in a loop, §17). A root self-destroys — there is no awaiting
// frame and nobody else owns the Task object.
struct FinalAwaiter {
    bool await_ready() noexcept { return false; }
    template <class P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
        P& p = h.promise();
        if (p.is_root && p.engine) {
            EngineOps* e = p.engine;
            e->root_unlink(e->em, &p);
            h.destroy();  // suspended at final_suspend — safe + frees frame
            return std::noop_coroutine();
        }
        if (p.done_fn) {
            // Combinator-owned resumption: the hook decides who runs next.
            auto* f = p.done_fn;
            void* a = p.done_arg;
            bool sd = p.self_destroy_;
            p.done_fn = nullptr;
            p.done_arg = nullptr;
            f(&p, a);
            if (sd) h.destroy();  // scope-owned child: free the frame
            return std::noop_coroutine();
        }
        return p.cont ? p.cont : std::noop_coroutine();
    }
    void await_resume() noexcept {}
};

namespace detail {
// Find the EngineOps behind a coroutine's first parameter. Recognised
// shapes: an EM-like object exposing `.coro_ops()`, and a handle exposing
// `.em()` returning such an object (ConnRef). No match => heap frame.
template <class T>
EngineOps* ops_of(T& first) {
    if constexpr (requires(T& t) {
                      { t.coro_ops() } -> std::convertible_to<EngineOps*>;
                  }) {
        return first.coro_ops();
    } else if constexpr (requires(T& t) {
                             {
                                 t.em().coro_ops()
                             } -> std::convertible_to<EngineOps*>;
                         }) {
        return first.em().coro_ops();
    } else {
        return nullptr;
    }
}

inline void* alloc_frame(EngineOps* ops, std::size_t n) {
    void* raw = nullptr;
    if (ops && ops->frame_alloc)
        raw = ops->frame_alloc(ops->em, n + sizeof(FrameHeader));
    FrameHeader* hdr;
    if (raw) {
        hdr = static_cast<FrameHeader*>(raw);
        hdr->ops = ops;
    } else {
        hdr =
            static_cast<FrameHeader*>(::operator new(n + sizeof(FrameHeader)));
        hdr->ops = nullptr;
    }
    return hdr + 1;
}

inline void free_frame(void* p) noexcept {
    if (!p) return;
    auto* hdr = static_cast<FrameHeader*>(p) - 1;
    if (hdr->ops && hdr->ops->frame_free)
        hdr->ops->frame_free(hdr->ops->em, hdr);
    else
        ::operator delete(hdr);
}
}  // namespace detail

template <class T>
class Task;

template <class T>
struct TaskStorage {
    std::optional<T> result_;
    std::exception_ptr ep_;
    template <class U>
    void store(U&& v) {
        result_.emplace(std::forward<U>(v));
    }
    T&& take() {
        if (ep_) std::rethrow_exception(ep_);
        return std::move(*result_);
    }
};
template <>
struct TaskStorage<void> {
    std::exception_ptr ep_;
    void take() {
        if (ep_) std::rethrow_exception(ep_);
    }
};

template <class T>
class TaskPromise;

// Promise common to Task<T> and Task<void>. [dcl.fct.def.coroutine] forbids
// declaring both return_value and return_void even under a requires-clause,
// so the two shapes live in separate specializations of TaskPromise below.
template <class T>
class TaskPromiseBase : public PromiseBase, public TaskStorage<T> {
  public:
    template <class First, class... Rest>
    void* operator new(std::size_t n, First& first, Rest&...) {
        return detail::alloc_frame(detail::ops_of(first), n);
    }
    void* operator new(std::size_t n) {
        return detail::alloc_frame(nullptr, n);
    }
    void operator delete(void* p) noexcept { detail::free_frame(p); }
    void operator delete(void* p, std::size_t) noexcept {
        detail::free_frame(p);
    }

    Task<T> get_return_object() noexcept {
        return Task<T>(std::coroutine_handle<TaskPromise<T>>::from_promise(
            *static_cast<TaskPromise<T>*>(this)));
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    FinalAwaiter final_suspend() noexcept { return {}; }
    void unhandled_exception() noexcept {
        this->ep_ = std::current_exception();
    }
};

template <class T>
class TaskPromise : public TaskPromiseBase<T> {
  public:
    TaskPromise() noexcept { this->destroy_frame = &self_destroy; }
    template <class U>
    void return_value(U&& v) noexcept(std::is_nothrow_move_constructible_v<U>) {
        this->store(std::forward<U>(v));
    }

  private:
    static void self_destroy(PromiseBase* p) noexcept {
        std::coroutine_handle<TaskPromise>::from_promise(
            *static_cast<TaskPromise*>(p))
            .destroy();
    }
};

template <>
class TaskPromise<void> : public TaskPromiseBase<void> {
  public:
    TaskPromise() noexcept { this->destroy_frame = &self_destroy; }
    void return_void() noexcept {}

  private:
    static void self_destroy(PromiseBase* p) noexcept {
        std::coroutine_handle<TaskPromise>::from_promise(
            *static_cast<TaskPromise*>(p))
            .destroy();
    }
};

// Task<T>: lazy, move-only, [[nodiscard]] (§17). Awaiting consumes the task
// (rvalue-only co_await); a completed child's frame is destroyed when the
// Awaiter drops the Task. A spawned root self-destroys at final_suspend.
template <class T>
class [[nodiscard]] Task {
  public:
    using promise_type = TaskPromise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

    Task() = default;
    explicit Task(handle_type h) noexcept : h_(h) {}
    Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            if (h_) h_.destroy();
            h_ = std::exchange(o.h_, {});
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() {
        // Destroying a live task that is PARKED in an awaiter strands the
        // awaiter's back-pointer into this frame — callers must cancel and
        // join first (TaskScope does). A never-started or completed frame
        // destroys cleanly anywhere.
        if (h_) h_.destroy();
    }

    handle_type handle() const noexcept { return h_; }
    promise_type& promise() const noexcept { return h_.promise(); }
    bool valid() const noexcept { return h_ != nullptr; }
    bool done() const noexcept { return !h_ || h_.done(); }

    // Resume once — used by spawn/scope machinery after wiring the engine.
    void start() {
        if (h_ && !h_.done()) h_.promise().resume_under_ctx(h_);
    }

    // Drop ownership without destroying (spawned roots self-destroy).
    handle_type release() noexcept { return std::exchange(h_, {}); }

    struct Awaiter {
        Task t;
        bool await_ready() const noexcept { return false; }
        template <class Parent>
        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<Parent> h) noexcept {
            auto& p = t.h_.promise();
            auto& pp = h.promise();
            p.cont = h;
            if (!p.engine) p.engine = pp.engine;
            // Inherit trace, tighten deadline, share stop (§7.4: never widen).
            p.ctx.trace = pp.ctx.trace;
            p.ctx.deadline = pp.ctx.deadline.earliest_of(p.ctx.deadline);
            if (pp.ctx.stop) p.ctx.stop = pp.ctx.stop;
            if (pp.stopped_) p.stopped_ = true;
            return t.h_;
        }
        decltype(auto) await_resume() {
            if constexpr (std::is_void_v<T>)
                t.promise().take();
            else
                return std::move(t.promise()).take();
        }
    };
    Awaiter operator co_await() && noexcept {
        return Awaiter{std::move(*this)};
    }

  private:
    handle_type h_{};
};

}  // namespace coro

// Public spelling: afx::CoroTask<T>. (DESIGN.md §17 writes afx::Task<T>;
// that name is already the posted-work closure in itc/mailbox.hpp, so the
// coroutine type is spelled CoroTask — noted in ADR-0005.)
template <class T>
using CoroTask = coro::Task<T>;

}  // namespace afx

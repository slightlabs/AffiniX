#pragma once

// coro/scope.hpp — TaskScope: structured concurrency for spawned work
// (M9-06). Children are owned by the scope — not EM roots — and are
// cancelled and joined before the scope can die:
//
//     afx::coro::TaskScope scope(em);
//     scope.spawn(worker(...));
//     co_await scope.join();   // suspends until all children finish
//
// request_stop() cancels every live child (each unwinds through its own
// awaiters). join() suspends the awaiting task until the last child
// finishes — children started after join() begins still count toward it
// if they spawn before the scope drains.
//
// A scope destroyed with live children force-destroys them: their awaiter
// destructors disarm timers and conn registrations, so no frame outlives
// its owner. Like everything else here: EM-thread only.

#include <coroutine>

#include "afx/coro/task.hpp"
#include "afx/core/context.hpp"

namespace afx::coro {

class TaskScope {
  public:
    explicit TaskScope(EngineOps* eng) : eng_(eng) {}
    template <class EM>
    explicit TaskScope(EM& em) : eng_(em.coro_ops()) {}

    ~TaskScope() {
        // join() is the contract; a scope dying early force-destroys its
        // children so no done_fn or frame outlives the scope.
        while (children_) {
            PromiseBase* p = children_;
            unlink(p);
            if (p->destroy_frame) p->destroy_frame(p);
        }
    }

    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;

    // Start `t` as a scope-owned child. Same context merge as em.spawn.
    void spawn(Task<void> t, Context ctx = {}) {
        auto& p = t.promise();
        p.engine = eng_;
        p.is_root = false;
        const Context& amb = afx::detail::ambient_context();
        p.ctx = ctx;
        if (!ctx.trace.valid()) p.ctx.trace = amb.trace;
        p.ctx.deadline = amb.deadline.earliest_of(ctx.deadline);
        if (!ctx.stop) p.ctx.stop = amb.stop;
        p.done_fn = &child_done_thunk;
        p.done_arg = this;
        p.self_destroy_ = true;  // scope children own their own frames
        link(&p);
        ++live_;
        p.resume_under_ctx(t.handle());
        (void)t.release();
    }

    void request_stop() noexcept {
        for (PromiseBase* p = children_; p; p = p->coro_next)
            p->request_stop();
    }

    std::size_t live() const noexcept { return live_; }

    // `co_await scope.join()` — resolves when the last child finishes.
    class JoinOp {
      public:
        explicit JoinOp(TaskScope* s) : s_(s) {}
        bool await_ready() noexcept { return s_->live_ == 0; }
        template <class PP>
        bool await_suspend(std::coroutine_handle<PP> h) noexcept {
            // Recheck before parking: children finishing between
            // await_ready and here must not strand us.
            if (s_->live_ == 0) return false;
            p_ = &h.promise();
            h_ = h;
            s_->join_p_ = p_;
            s_->join_h_ = h_;
            s_->join_parked_ = true;
            return true;
        }
        Result<void> await_resume() noexcept {
            s_->join_parked_ = false;
            return {};
        }

      private:
        TaskScope* s_;
        PromiseBase* p_ = nullptr;
        std::coroutine_handle<> h_{};
    };
    JoinOp join() noexcept { return JoinOp(this); }

  private:
    static void child_done_thunk(PromiseBase* self, void* a) noexcept {
        auto* s = static_cast<TaskScope*>(a);
        s->unlink(self);
        if (s->live_ == 0 && s->join_parked_) {
            s->join_parked_ = false;
            s->join_p_->resume_under_ctx(s->join_h_);
        }
    }

    void link(PromiseBase* p) noexcept {
        p->coro_prev = nullptr;
        p->coro_next = children_;
        if (children_) children_->coro_prev = p;
        children_ = p;
    }
    void unlink(PromiseBase* p) noexcept {
        if (p->coro_prev)
            p->coro_prev->coro_next = p->coro_next;
        else
            children_ = p->coro_next;
        if (p->coro_next) p->coro_next->coro_prev = p->coro_prev;
        p->coro_prev = p->coro_next = nullptr;
        --live_;
    }

    EngineOps* eng_;
    PromiseBase* children_ = nullptr;
    PromiseBase* join_p_ = nullptr;
    std::coroutine_handle<> join_h_{};
    std::size_t live_ = 0;
    bool join_parked_ = false;
};

}  // namespace afx::coro

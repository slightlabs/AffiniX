#pragma once

// Mailbox — DESIGN.md §11.1 and the arm/block protocol of §8.1.
// A Mailbox is a copyable, thread-safe handle used to send work to an
// EventManager. The queue is bounded by construction; OverflowPolicy decides
// what post() does on a full ring.

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

#include "afx/core/context.hpp"
#include "afx/itc/mpsc_ring.hpp"
#include "afx/sys/inline_fn.hpp"
#include "afx/sys/result.hpp"

namespace afx {

using Task = InlineFn<void(), 48>;

enum class OverflowPolicy : std::uint8_t {
    Fail,       // try_post reports Full; post() returns it
    SpinRetry,  // post() retries until space frees
    Abort,      // post() aborts the process (fail-fast shops)
};

enum class PostResult : std::uint8_t { Ok, Full, Closed };

struct PostedItem {
    Task fn;
    Context ctx;
    std::uint64_t enq_tsc = 0;
};

enum class MailboxState : std::uint8_t { Running = 0, Blocked = 1 };

// The shared state. Owned by an EventManager; Mailboxes hold a shared_ptr so
// posting to a dead EM degrades to PostResult::Closed, never a dangling write.
struct MailboxImpl {
    explicit MailboxImpl(std::size_t ring_capacity) : ring(ring_capacity) {}

    MpscRing<PostedItem> ring;
    std::atomic<MailboxState> state{MailboxState::Running};
    std::atomic<bool> dead{false};
    OverflowPolicy overflow = OverflowPolicy::Fail;

    // Wake thunk into the consumer's backend (eventfd write, or a sim hook).
    void (*wake_fn)(void*) = nullptr;
    void* wake_ctx = nullptr;

    // Counters mirrored into Stats by the consumer on drain.
    std::atomic<std::uint64_t> pushes{0};
    std::atomic<std::uint64_t> full{0};
};

class Mailbox {
  public:
    Mailbox() = default;
    explicit Mailbox(std::shared_ptr<MailboxImpl> impl)
        : impl_(std::move(impl)) {}

    bool valid() const noexcept { return impl_ != nullptr; }

    // Post with the ambient context (§7.4): the Context of whatever work item
    // is running on the calling thread, or empty outside a loop.
    PostResult post(Task&& fn) const {
        return post_with_ctx(std::move(fn), detail::ambient_context());
    }

    PostResult post_with_ctx(Task&& fn, const Context& ctx) const {
        if (!impl_ || impl_->dead.load(std::memory_order_acquire))
            return PostResult::Closed;
        return push_one({std::move(fn), ctx, rdtsc_light()});
    }

    std::size_t post_batch(std::span<Task> fns) const {
        if (!impl_ || impl_->dead.load(std::memory_order_acquire)) return 0;
        std::size_t n = 0;
        const Context& ctx = detail::ambient_context();
        for (auto& f : fns) {
            if (!impl_->ring.try_push(PostedItem{std::move(f), ctx, 0})) break;
            ++n;
        }
        if (n) after_push();
        return n;
    }

    // ---- post_and_reply (§11.4): request/response without blocking. --------
    // work() runs on this mailbox's EM and returns R; the reply is posted
    // back to `reply_to` and delivered to `on_reply` there.
    template <class F, class Cb>
    PostResult post_and_reply(F&& work, const Mailbox& reply_to,
                              Cb&& on_reply) const {
        Context ctx = detail::ambient_context();
        return post_with_ctx(
            [w = std::forward<F>(work), reply_to, ctx,
             cb = std::forward<Cb>(on_reply)]() mutable {
                auto r = w();
                reply_to.post_with_ctx(
                    [cb = std::move(cb), r = std::move(r)]() mutable {
                        cb(std::move(r));
                    },
                    ctx);
            },
            ctx);
    }

    std::size_t size_approx() const noexcept {
        return impl_ ? impl_->ring.capacity()
                     : 0;  // capacity; depth is EM-side
    }

  private:
    PostResult push_one(PostedItem&& it) const {
        for (;;) {
            if (impl_->ring.try_push(std::move(it))) {
                impl_->pushes.fetch_add(1, std::memory_order_relaxed);
                after_push();
                return PostResult::Ok;
            }
            impl_->full.fetch_add(1, std::memory_order_relaxed);
            switch (impl_->overflow) {
                case OverflowPolicy::Fail:
                    return PostResult::Full;
                case OverflowPolicy::Abort:
                    std::abort();
                case OverflowPolicy::SpinRetry:
                    if (impl_->dead.load(std::memory_order_acquire))
                        return PostResult::Closed;
                    // brief pause, then retry
                    for (int i = 0; i < 64; ++i) {}
                    continue;
            }
        }
    }

    // Arm/block protocol, producer side (§8.1): if the consumer has published
    // Blocked with seq_cst, our push happens-before its re-check of the ring,
    // so either it sees the item or we signal — never both lost.
    void after_push() const {
        if (impl_->state.load(std::memory_order_seq_cst) ==
            MailboxState::Blocked)
            if (impl_->wake_fn) impl_->wake_fn(impl_->wake_ctx);
    }

    static std::uint64_t rdtsc_light() noexcept {
#if defined(__x86_64__)
        return __builtin_ia32_rdtsc();
#else
        return 0;
#endif
    }

    std::shared_ptr<MailboxImpl> impl_;

    friend class EventManagerAccess;  // EM drains via impl_
    template <class, class>
    friend class BasicEventManager;
};

}  // namespace afx

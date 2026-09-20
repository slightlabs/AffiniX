#pragma once

// Timers — DESIGN.md §10. A 4-level × 256-slot hierarchical wheel (O(1)
// arm/cancel/amortised expiry, ≈49-day horizon at a 1 ms tick) plus a 4-ary
// min-heap carrying at() deadlines that need sub-tick precision and
// beyond-horizon timers. TimerGroup is an intrusive list through the nodes
// (§10.1), so group membership costs 16 bytes and no time.

#include <array>
#include <cstdint>
#include <vector>

#include "afx/core/context.hpp"
#include "afx/core/handle_table.hpp"
#include "afx/sys/inline_fn.hpp"
#include "afx/sys/types.hpp"

namespace afx {

enum class RepeatMode : std::uint8_t { FixedRate, FixedDelay };

struct TimerCtx {
    TimerId id;
    TimePoint scheduled;       // when it should have fired
    TimePoint now;             // when it actually fired
    std::uint32_t missed = 0;  // coalesced overruns (FixedRate only)
    Duration lateness() const { return now - scheduled; }
};

using TimerFn = InlineFn<void(TimerCtx), 48>;

// Intrusive node: lives in an EM-owned HandleTable<TimerNode, TimerId>.
// bucket_* links it into a wheel slot; group_* links it into its TimerGroup.
struct TimerNode {
    TimePoint expiry{};
    std::uint64_t expiry_tick = 0;
    Duration period{};
    RepeatMode mode = RepeatMode::FixedRate;
    TimerFn fn;

    TimerNode* bucket_prev = nullptr;
    TimerNode* bucket_next = nullptr;

    TimerNode* group_prev = nullptr;
    TimerNode* group_next = nullptr;
    TimerGroup group{};

    Context ctx{};  // ambient context captured at arm (§7.4)
    TimerId id{};
    bool cancelled = false;
    bool in_heap = false;
    bool precise_ = false;  // at(): heap, sub-tick precision
    std::size_t heap_idx = ~std::size_t(0);
};

namespace detail {

// Sentinel-terminated circular list shared by wheel buckets and groups.
inline void list_init(TimerNode* sentinel) noexcept {
    sentinel->bucket_prev = sentinel->bucket_next = sentinel;
}
inline bool list_empty(const TimerNode* sentinel) noexcept {
    return sentinel->bucket_next == sentinel;
}
inline void list_push_back(TimerNode* sentinel, TimerNode* n) noexcept {
    n->bucket_prev = sentinel->bucket_prev;
    n->bucket_next = sentinel;
    sentinel->bucket_prev->bucket_next = n;
    sentinel->bucket_prev = n;
}
inline void list_unlink(TimerNode* n) noexcept {
    n->bucket_prev->bucket_next = n->bucket_next;
    n->bucket_next->bucket_prev = n->bucket_prev;
    n->bucket_prev = n->bucket_next = nullptr;
}
inline bool list_linked(const TimerNode* n) noexcept {
    return n->bucket_next != nullptr;
}

inline void glist_unlink(TimerNode* n) noexcept {
    if (!n->group_next) return;
    n->group_prev->group_next = n->group_next;
    n->group_next->group_prev = n->group_prev;
    n->group_prev = n->group_next = nullptr;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// TimerWheel
// ---------------------------------------------------------------------------
class TimerWheel {
  public:
    static constexpr int kLevels = 4;
    static constexpr int kSlots = 256;

    explicit TimerWheel(Nanos tick) : tick_(tick) {
        for (auto& lvl : buckets_)
            for (auto& s : lvl) detail::list_init(&s);
    }

    Nanos tick() const noexcept { return tick_; }

    // Round a deadline UP to its containing tick so a timer never fires early.
    std::uint64_t tick_of(TimePoint t) const noexcept {
        auto ns = t.time_since_epoch().count();
        auto tk = tick_.count();
        return std::uint64_t((ns + tk - 1) / tk);
    }

    // Absolute tick index of "now" (floor).
    std::uint64_t floor_tick(TimePoint t) const noexcept {
        return std::uint64_t(t.time_since_epoch().count() / tick_.count());
    }

    bool fits(std::uint64_t expiry_tick,
              std::uint64_t now_tick) const noexcept {
        if (expiry_tick <= now_tick) return true;
        std::uint64_t d = expiry_tick - now_tick;
        for (int l = 0; l < kLevels; ++l) d >>= 8;
        return d == 0;  // d < 256^4
    }

    // Insert a node already carrying expiry_tick. Caller guarantees it fits.
    void insert(TimerNode& n, std::uint64_t now_tick) noexcept {
        // An overdue or same-tick node inserted from outside advance() must
        // fire on the next advance step: its raw expiry_tick residue may
        // name a level-0 slot the current sweep has already passed, which
        // would delay it by up to a full 256-tick rotation — so clamp the
        // effective tick to now_tick + 1. Cascade inserts bypass this
        // (see cascade): they run before the current slot drains, so a
        // node landing exactly on now_tick must use the current slot.
        std::uint64_t et =
            n.expiry_tick > now_tick ? n.expiry_tick : now_tick + 1;
        insert_node(n, et, now_tick);
    }

    void unlink(TimerNode& n) noexcept {
        if (detail::list_linked(&n)) {
            detail::list_unlink(&n);
            --count_;
        }
    }

    // Advance the wheel to `target_tick`, calling fire(node) on each due node.
    // Due nodes are unlinked before fire() runs; fire() may re-insert
    // (repeating timers) or release them. Bounded by max_ticks_per_call.
    template <class F>
    void advance(std::uint64_t target_tick, F&& fire) {
        // The wheel's epoch starts at 0 but a real clock's first target is
        // millions of ticks ahead; a non-empty wheel cannot snap forward
        // inside the loop (nodes sit in high-level buckets that only cascade
        // at wrap boundaries), so anchor the epoch once: rehome every queued
        // node relative to `target` and let the normal step drain the slot.
        if (!primed_) prime(target_tick);
        // Empty wheel: nothing can cascade, so snap forward. On a real
        // clock the first advance must otherwise grind one step per tick
        // from epoch to now — millions of empty buckets per poll.
        if (count_ == 0) {
            if (target_tick > now_tick_) now_tick_ = target_tick;
            return;
        }
        std::size_t steps = 0;
        while (now_tick_ < target_tick && steps++ < max_ticks_per_call_) {
            ++now_tick_;
            // A higher level's bucket cascades when the digit below it wraps:
            // level 1 cascades every 256 ticks, level 2 every 256^2, ...
            for (int l = 1; l < kLevels; ++l)
                if (now_tick_ % (std::uint64_t(kSlots) << (8 * (l - 1))) == 0)
                    cascade(l);
            expire_slot(now_tick_ & (kSlots - 1), fire);
        }
    }

    std::uint64_t now_tick() const noexcept { return now_tick_; }
    std::size_t size() const noexcept { return count_; }
    void set_max_ticks_per_call(std::size_t n) noexcept {
        max_ticks_per_call_ = n;
    }

  private:
    // First advance(): move now_tick_ to `target` and re-place every queued
    // node against it. Nodes armed before run() carry absolute ticks that the
    // pre-prime insert() placed vs epoch 0 — without this they'd sit in a
    // high-level bucket until the wheel ground through ~8e7 dead ticks.
    // Overdue nodes clamp into the slot the upcoming step drains (like the
    // cascade convention at now_tick_), so nothing lands in a dead slot.
    void prime(std::uint64_t target) noexcept {
        primed_ = true;
        TimerNode hold;
        detail::list_init(&hold);
        for (auto& lvl : buckets_)
            for (auto& s : lvl)
                while (!detail::list_empty(&s)) {
                    TimerNode* n = s.bucket_next;
                    detail::list_unlink(n);
                    detail::list_push_back(&hold, n);
                }
        count_ = 0;
        now_tick_ = target ? target - 1 : 0;
        const std::uint64_t min_et = target ? target : 1;
        while (!detail::list_empty(&hold)) {
            TimerNode* n = hold.bucket_next;
            detail::list_unlink(n);
            insert_node(
                *n, n->expiry_tick > min_et ? n->expiry_tick : min_et,
                now_tick_);
        }
    }

    void cascade(int level) noexcept {
        std::size_t home = (now_tick_ >> (8 * level)) & (kSlots - 1);
        TimerNode* s = &buckets_[level][home];
        // Move every node down to where its remaining distance belongs —
        // in place, no scratch vector: allocations are forbidden on the hot
        // path (M2-11 invariant). A node whose destination is this very slot
        // stays put; anything else unlinks and reinserts, with the walk
        // pointer captured before the move.
        TimerNode* n = s->bucket_next;
        while (n != s) {
            TimerNode* next = n->bucket_next;
            // Cascades run before the current slot drains, so a node
            // landing exactly on now_tick_ belongs in the current slot —
            // do not apply the overdue clamp used by public insert().
            std::uint64_t et =
                n->expiry_tick > now_tick_ ? n->expiry_tick : now_tick_;
            std::uint64_t d = et - now_tick_;
            int lvl = 0;
            while (d >= kSlots) {
                d >>= 8;
                ++lvl;
            }
            std::size_t dst = (et >> (8 * lvl)) & (kSlots - 1);
            if (lvl != level || dst != home) {
                detail::list_unlink(n);
                --count_;
                insert_node(*n, et, now_tick_);
            }
            n = next;
        }
    }

    void insert_node(TimerNode& n, std::uint64_t et,
                     std::uint64_t now_tick) noexcept {
        std::uint64_t d = et - now_tick;
        int level = 0;
        while (d >= kSlots) {
            d >>= 8;
            ++level;
        }
        std::size_t slot = (et >> (8 * level)) & (kSlots - 1);
        detail::list_push_back(&buckets_[level][slot], &n);
        ++count_;
    }

    template <class F>
    void expire_slot(std::size_t slot, F&& fire) {
        TimerNode* s = &buckets_[0][slot];
        while (!detail::list_empty(s)) {
            TimerNode* n = s->bucket_next;
            detail::list_unlink(n);
            --count_;
            if (n->expiry_tick > now_tick_) {
                // Cascaded early: not actually due; reinsert.
                insert(*n, now_tick_);
                continue;
            }
            fire(*n);
        }
    }

    Nanos tick_;
    std::uint64_t now_tick_ = 0;
    bool primed_ = false;
    std::size_t count_ = 0;
    std::size_t max_ticks_per_call_ = 4096;
    TimerNode buckets_[kLevels][kSlots];
};

// ---------------------------------------------------------------------------
// Four-ary min-heap over TimerNode::expiry (precise / far deadlines).
// ---------------------------------------------------------------------------
class TimerHeap {
  public:
    void push(TimerNode& n) noexcept {
        n.in_heap = true;
        n.heap_idx = heap_.size();
        heap_.push_back(&n);
        sift_up(n.heap_idx);
    }
    void remove(TimerNode& n) noexcept {
        if (!n.in_heap) return;
        std::size_t i = n.heap_idx;
        TimerNode* last = heap_.back();
        heap_.pop_back();
        n.in_heap = false;
        n.heap_idx = ~std::size_t(0);
        if (i < heap_.size()) {
            heap_[i] = last;
            last->heap_idx = i;
            sift_up(i);
            sift_down(i);
        }
    }
    TimerNode* top() noexcept { return heap_.empty() ? nullptr : heap_[0]; }
    bool empty() const noexcept { return heap_.empty(); }
    std::size_t size() const noexcept { return heap_.size(); }

    template <class F>
    void expire(TimePoint now, F&& fire) {
        while (!heap_.empty() && heap_[0]->expiry <= now) {
            TimerNode* n = heap_[0];
            remove(*n);
            fire(*n);
        }
    }

  private:
    static bool before(const TimerNode* a, const TimerNode* b) noexcept {
        return a->expiry < b->expiry;
    }
    void sift_up(std::size_t i) noexcept {
        while (i > 0) {
            std::size_t p = (i - 1) / 4;
            if (!before(heap_[i], heap_[p])) break;
            std::swap(heap_[i], heap_[p]);
            heap_[i]->heap_idx = i;
            heap_[p]->heap_idx = p;
            i = p;
        }
    }
    void sift_down(std::size_t i) noexcept {
        for (;;) {
            std::size_t best = i;
            for (std::size_t c = 4 * i + 1; c <= 4 * i + 4 && c < heap_.size();
                 ++c)
                if (before(heap_[c], heap_[best])) best = c;
            if (best == i) break;
            std::swap(heap_[i], heap_[best]);
            heap_[i]->heap_idx = i;
            heap_[best]->heap_idx = best;
            i = best;
        }
    }
    std::vector<TimerNode*> heap_;
};

// ---------------------------------------------------------------------------
// TimerGroupState: the head of an intrusive list of member timers (§10.1).
// Lives in HandleTable<TimerGroupState, TimerGroupId>.
// ---------------------------------------------------------------------------
struct TimerGroupState {
    TimerNode sentinel{};
    TimerGroupState() { sentinel.group_prev = sentinel.group_next = &sentinel; }
    void add(TimerNode& n) noexcept {
        n.group_prev = sentinel.group_prev;
        n.group_next = &sentinel;
        sentinel.group_prev->group_next = &n;
        sentinel.group_prev = &n;
    }
    bool empty() const noexcept { return sentinel.group_next == &sentinel; }
};

}  // namespace afx

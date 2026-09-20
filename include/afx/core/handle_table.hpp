#pragma once

// Generation-checked handles over an EM-owned slab (DESIGN.md §13, ADR-0003).
// Slot reuse bumps the generation so a stale handle can never alias a new
// object. Reclamation is deferred: release() invalidates the generation
// immediately but the slot does not return to the free list until reclaim(),
// so a callback cannot free memory out from under its own caller.

#include <cstdint>
#include <deque>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace afx {

template <class Tag>
struct Handle {
    std::uint32_t idx = 0;
    std::uint32_t gen = 0;
    constexpr bool valid() const noexcept { return gen != 0; }
    constexpr bool operator==(const Handle&) const = default;
};

struct ConnIdTag;
struct TimerIdTag;
struct IoIdTag;
struct TimerGroupTag;

using ConnId       = Handle<ConnIdTag>;
using TimerId      = Handle<TimerIdTag>;
using IoId         = Handle<IoIdTag>;
using TimerGroupId = Handle<TimerGroupTag>;

// Design doc names the wheel-side field `slot`; keep idx internally.
using TimerGroup = TimerGroupId;

template <class T, class HandleT>
class HandleTable {
    struct Slot {
        std::uint32_t gen = 1;
        bool alive = false;
        alignas(T) unsigned char storage[sizeof(T)];
    };

public:
    HandleTable() = default;
    // `reserve` is accepted for API compatibility but is a no-op: slots_ is a
    // deque precisely so that growth never moves existing elements (see the
    // comment on slots_ below).
    explicit HandleTable(std::size_t) {}
    ~HandleTable() { clear(); }

    HandleTable(const HandleTable&) = delete;
    HandleTable& operator=(const HandleTable&) = delete;

    template <class... A>
    std::pair<HandleT, T*> emplace(A&&... a) {
        std::uint32_t idx;
        if (!free_.empty()) {
            idx = free_.back();
            free_.pop_back();
        } else {
            idx = static_cast<std::uint32_t>(slots_.size());
            slots_.emplace_back();
        }
        Slot& s = slots_[idx];
        T* p = new (s.storage) T(std::forward<A>(a)...);
        s.alive = true;
        ++live_;
        return {HandleT{idx, s.gen}, p};
    }

    T* get(HandleT h) noexcept {
        if (!h.valid() || h.idx >= slots_.size()) return nullptr;
        Slot& s = slots_[h.idx];
        if (!s.alive || s.gen != h.gen) return nullptr;
        return reinterpret_cast<T*>(s.storage);
    }
    const T* get(HandleT h) const noexcept {
        return const_cast<HandleTable*>(this)->get(h);
    }

    bool contains(HandleT h) const noexcept { return get(h) != nullptr; }

    // Invalidate immediately, defer slot reuse to reclaim().
    void release(HandleT h) noexcept {
        T* p = get(h);
        if (!p) return;
        Slot& s = slots_[h.idx];
        p->~T();
        s.alive = false;
        --live_;
        ++s.gen;                       // stale handles fail gen check at once
        if (s.gen == 0) s.gen = 1;
        deferred_.push_back(h.idx);
    }

    void reclaim() noexcept {
        for (std::uint32_t idx : deferred_) free_.push_back(idx);
        deferred_.clear();
    }

    std::size_t size() const noexcept { return live_; }
    std::size_t capacity() const noexcept { return slots_.size(); }

    // Iterate live objects; cb(T&, HandleT).
    template <class F> void for_each(F&& cb) {
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            Slot& s = slots_[i];
            if (s.alive)
                cb(*reinterpret_cast<T*>(s.storage), HandleT{i, s.gen});
        }
    }

    void clear() noexcept {
        for_each([](T& t, HandleT) { t.~T(); });
        slots_.clear();
        free_.clear();
        deferred_.clear();
        live_ = 0;
    }

private:
    // A deque, not a vector: growing it never reallocates or moves existing
    // elements, so a T* returned by get()/emplace() stays valid for the life
    // of the slot even as the table grows. Code elsewhere (e.g. the timer
    // wheel's intrusive bucket/group lists) keeps raw T* pointers across
    // calls that can add new slots, which a reallocating container would
    // silently corrupt (stale addresses that look like a valid, connected
    // circular list until they are dereferenced or unlinked).
    std::deque<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::vector<std::uint32_t> deferred_;
    std::size_t live_ = 0;
};

} // namespace afx

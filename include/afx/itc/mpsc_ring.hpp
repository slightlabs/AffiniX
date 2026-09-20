#pragma once

// Bounded MPSC ring — Vyukov slot-sequence (DESIGN.md §11.2). Single consumer
// dequeue is wait-free; producers use a CAS claim loop.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace afx {

template <class T>
class MpscRing {
    struct Cell {
        std::atomic<std::size_t> seq{0};
        T data{};
    };

public:
    explicit MpscRing(std::size_t capacity_pow2)
        : cap_(capacity_pow2), mask_(capacity_pow2 - 1),
          cells_(new Cell[capacity_pow2]) {
        for (std::size_t i = 0; i < cap_; ++i)
            cells_[i].seq.store(i, std::memory_order_relaxed);
    }

    ~MpscRing() = default;
    MpscRing(const MpscRing&) = delete;
    MpscRing& operator=(const MpscRing&) = delete;

    std::size_t capacity() const noexcept { return cap_; }

    bool try_push(const T& v) { return push_impl(v); }
    bool try_push(T&& v)      { return push_impl(std::move(v)); }

    std::size_t try_push_bulk(std::span<const T> vs) {
        std::size_t n = 0;
        for (auto& v : vs) {
            if (!try_push(v)) break;
            ++n;
        }
        return n;
    }

    // Single-consumer dequeue.
    bool try_pop(T& out) {
        Cell* cell;
        std::size_t pos = deq_pos_.load(std::memory_order_relaxed);
        cell = &cells_[pos & mask_];
        std::size_t seq = cell->seq.load(std::memory_order_acquire);
        if (static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1) != 0)
            return false;
        deq_pos_.store(pos + 1, std::memory_order_relaxed);
        out = std::move(cell->data);
        cell->seq.store(pos + cap_, std::memory_order_release);
        return true;
    }

    template <class F>
    std::size_t drain(F&& f, std::size_t max) {
        std::size_t n = 0;
        T item;
        while (n < max && try_pop(item)) { f(std::move(item)); ++n; }
        return n;
    }

    bool empty() const noexcept {
        std::size_t pos = deq_pos_.load(std::memory_order_relaxed);
        Cell* cell = &cells_[pos & mask_];
        std::size_t seq = cell->seq.load(std::memory_order_acquire);
        return static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1) != 0;
    }

private:
    template <class V>
    bool push_impl(V&& v) {
        Cell* cell;
        std::size_t pos = enq_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            std::size_t seq = cell->seq.load(std::memory_order_acquire);
            std::intptr_t dif = static_cast<std::intptr_t>(seq)
                              - static_cast<std::intptr_t>(pos);
            if (dif == 0) {
                if (enq_pos_.compare_exchange_weak(pos, pos + 1,
                        std::memory_order_relaxed))
                    break;
            } else if (dif < 0) {
                return false;   // full
            } else {
                pos = enq_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = std::forward<V>(v);
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    const std::size_t cap_;
    const std::size_t mask_;
    std::unique_ptr<Cell[]> cells_;
    alignas(64) std::atomic<std::size_t> enq_pos_{0};
    alignas(64) std::atomic<std::size_t> deq_pos_{0};
};

} // namespace afx

#pragma once

// Bounded SPSC ring — DESIGN.md §11.2. Cache-line-padded head/tail, wait-free,
// batch claim on both sides.

#include <atomic>
#include <cstddef>
#include <new>
#include <span>
#include <utility>

namespace afx {

template <class T>
class SpscRing {
public:
    explicit SpscRing(std::size_t capacity_pow2)
        : cap_(capacity_pow2), mask_(capacity_pow2 - 1),
          buf_(new T[capacity_pow2]) {
        // capacity must be a power of two
    }

    ~SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    std::size_t capacity() const noexcept { return cap_; }

    // Producer side ----------------------------------------------------------
    bool try_push(const T& v) { return try_push_one(v); }
    bool try_push(T&& v)      { return try_push_one(std::move(v)); }

    template <class V>
    bool try_push_one(V&& v) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t h = head_.load(std::memory_order_acquire);
        if (t - h >= cap_) return false;
        buf_[t & mask_] = std::forward<V>(v);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    std::size_t try_push_bulk(std::span<const T> vs) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t h = head_.load(std::memory_order_acquire);
        std::size_t room = cap_ - (t - h);
        std::size_t n = vs.size() < room ? vs.size() : room;
        for (std::size_t i = 0; i < n; ++i) buf_[(t + i) & mask_] = vs[i];
        tail_.store(t + n, std::memory_order_release);
        return n;
    }

    // Consumer side ----------------------------------------------------------
    // Drain up to max items, invoking f(T&&) on each. Returns items drained.
    template <class F>
    std::size_t drain(F&& f, std::size_t max) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        std::size_t avail = t - h;
        std::size_t n = avail < max ? avail : max;
        for (std::size_t i = 0; i < n; ++i)
            f(std::move(buf_[(h + i) & mask_]));
        head_.store(h + n, std::memory_order_release);
        return n;
    }

    // Contiguous-claim drain: hands f() a span over a contiguous region of
    // the ring (up to max or to the wrap point). For batch-friendly consumers.
    template <class F>
    std::size_t drain_contiguous(F&& f, std::size_t max) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        std::size_t avail = t - h;
        if (!avail) return 0;
        std::size_t off = h & mask_;
        std::size_t contiguous = cap_ - off;
        std::size_t n = avail < contiguous ? avail : contiguous;
        if (n > max) n = max;
        f(std::span<T>(buf_.get() + off, n));
        head_.store(h + n, std::memory_order_release);
        return n;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    std::size_t size_approx() const noexcept {
        auto t = tail_.load(std::memory_order_acquire);
        auto h = head_.load(std::memory_order_acquire);
        return t >= h ? t - h : 0;
    }

private:
    const std::size_t cap_;
    const std::size_t mask_;
    std::unique_ptr<T[]> buf_;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

} // namespace afx

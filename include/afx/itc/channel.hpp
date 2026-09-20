#pragma once

// Typed channels — DESIGN.md §11.2. The fast path: no type erasure, no
// allocation per item, batch delivery. The receiver attaches to an EM; a push
// posts a single wakeup so the consumer drains in stage 1.

#include <array>
#include <atomic>
#include <memory>
#include <utility>

#include "afx/itc/mailbox.hpp"
#include "afx/itc/mpsc_ring.hpp"
#include "afx/itc/spsc_ring.hpp"

namespace afx {

enum class ChannelKind : std::uint8_t { SPSC, MPSC };

namespace detail {

template <class T>
struct ChannelState {
    explicit ChannelState(std::size_t cap, ChannelKind k)
        : kind(k),
          spsc(k == ChannelKind::SPSC ? cap : 2),
          mpsc(k == ChannelKind::MPSC ? cap : 2) {}

    ChannelKind kind;
    SpscRing<T> spsc;
    MpscRing<T> mpsc;
    std::atomic<bool> drain_posted{false};

    ~ChannelState() { if (teardown) teardown(cb_obj); }

    // Receiver side
    Mailbox target{};
    void*   cb_obj = nullptr;    // type-erased batch callback
    void (*cb)(void*, std::span<const T>) = nullptr;
    void (*teardown)(void*) = nullptr;

    bool push(T&& v) {
        bool ok = kind == ChannelKind::SPSC ? spsc.try_push(std::move(v))
                                          : mpsc.try_push(std::move(v));
        if (ok) poke();
        return ok;
    }
    std::size_t push_bulk(std::span<const T> vs) {
        std::size_t n = kind == ChannelKind::SPSC ? spsc.try_push_bulk(vs)
                                                  : mpsc.try_push_bulk(vs);
        if (n) poke();
        return n;
    }

    void poke() {
        // One in-flight drain task per channel: producers that arrive while
        // a drain is queued just fill the ring.
        if (!target.valid()) return;
        if (!drain_posted.exchange(true, std::memory_order_acq_rel))
            target.post([st = this] { st->drain(); });
    }

    void drain() {
        drain_posted.store(false, std::memory_order_release);
        if (!cb) return;
        if (kind == ChannelKind::SPSC) {
            spsc.drain_contiguous(
                [&](std::span<T> batch) { cb(cb_obj, batch); }, 1024);
        } else {
            // MPSC claims aren't contiguous: deliver in chunks.
            std::array<T, 64> chunk;
            std::size_t filled = 0;
            T item;
            while (mpsc.try_pop(item)) {
                chunk[filled++] = std::move(item);
                if (filled == chunk.size()) {
                    cb(cb_obj, std::span<const T>(chunk.data(), filled));
                    filled = 0;
                }
            }
            if (filled) cb(cb_obj, std::span<const T>(chunk.data(), filled));
        }
        // A push racing the flag reset may have re-posted; loop once more is
        // unnecessary — the posted drain will see the items.
    }
};

} // namespace detail

template <class T>
class ChannelTx {
public:
    ChannelTx() = default;
    explicit ChannelTx(std::shared_ptr<detail::ChannelState<T>> st)
        : st_(std::move(st)) {}

    bool try_push(T v) const { return st_->push(std::move(v)); }
    std::size_t try_push_bulk(std::span<const T> vs) const {
        return st_->push_bulk(vs);
    }
    bool valid() const noexcept { return st_ != nullptr; }

private:
    std::shared_ptr<detail::ChannelState<T>> st_;
};

template <class T>
class ChannelRx {
public:
    ChannelRx() = default;
    explicit ChannelRx(std::shared_ptr<detail::ChannelState<T>> st)
        : st_(std::move(st)) {}

    // attach(em, cb): cb(std::span<const T> batch) runs on em's thread.
    template <class EM, class F>
    void attach(EM& em, F&& cb) {
        using Fn = std::decay_t<F>;
        auto* box = new Fn(std::forward<F>(cb));
        st_->cb_obj = box;
        st_->cb = [](void* p, std::span<const T> b) { (*static_cast<Fn*>(p))(b); };
        st_->target = em.mailbox();
        // stash the box on the state for teardown
        st_->teardown = [](void* p) { delete static_cast<Fn*>(p); };
    }

    // Pollable variant for non-EM contexts (and tests).
    void poll() { st_->drain(); }

private:
    std::shared_ptr<detail::ChannelState<T>> st_;
};

template <class T>
std::pair<ChannelTx<T>, ChannelRx<T>>
channel(std::size_t capacity_pow2, ChannelKind kind = ChannelKind::SPSC) {
    auto st = std::make_shared<detail::ChannelState<T>>(capacity_pow2, kind);
    return {ChannelTx<T>(st), ChannelRx<T>(std::move(st))};
}

} // namespace afx

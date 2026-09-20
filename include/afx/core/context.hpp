#pragma once

// Context, Deadline, TraceId, StopToken — DESIGN.md §7.4, ADR-0008.
// Ambient per-work-item data that follows work across every hop.

#include <atomic>
#include <cstdint>
#include <memory>

#include "afx/sys/types.hpp"

namespace afx {

// ---------------------------------------------------------------------------
// Deadline: absolute point by which work must complete. {} == none.
// ---------------------------------------------------------------------------
class Deadline {
  public:
    Deadline() = default;
    explicit Deadline(TimePoint t) : t_(t), set_(true) {}

    static Deadline at(TimePoint t) { return Deadline(t); }
    static Deadline in(Duration d) {
        return Deadline(
            TimePoint(std::chrono::steady_clock::now().time_since_epoch() + d));
    }

    bool is_set() const noexcept { return set_; }
    TimePoint point() const noexcept { return t_; }

    bool expired(TimePoint now) const noexcept { return set_ && now >= t_; }
    Duration remaining(TimePoint now) const noexcept {
        if (!set_) return Duration::max();
        return t_ > now ? t_ - now : Duration::zero();
    }

    // A hop can only tighten a deadline, never extend it (§7.4).
    Deadline earliest_of(Deadline o) const noexcept {
        if (!set_) return o;
        if (!o.set_) return *this;
        return Deadline(std::min(t_, o.t_));
    }

    bool operator==(const Deadline&) const = default;

  private:
    Deadline(TimePoint t, bool s) : t_(t), set_(s) {}
    TimePoint t_{};
    bool set_ = false;
};

// ---------------------------------------------------------------------------
// TraceId: 16-byte trace id + 8-byte span id.
// ---------------------------------------------------------------------------
struct TraceId {
    std::uint64_t hi = 0;    // trace id high
    std::uint64_t lo = 0;    // trace id low
    std::uint64_t span = 0;  // span id

    using Short = std::uint64_t;  // compact form used by the flight recorder

    bool valid() const noexcept { return hi || lo; }
    Short short_id() const noexcept { return hi ^ lo; }
    bool operator==(const TraceId&) const = default;
};

// ---------------------------------------------------------------------------
// StopToken: cooperative cancellation shared with work items.
// ---------------------------------------------------------------------------
class StopToken {
  public:
    StopToken() = default;

    bool stop_requested() const noexcept {
        return st_ && st_->flag.load(std::memory_order_acquire);
    }
    explicit operator bool() const noexcept { return st_ != nullptr; }

  private:
    friend class StopSource;
    struct State {
        std::atomic<bool> flag{false};
    };
    explicit StopToken(std::shared_ptr<State> s) : st_(std::move(s)) {}
    std::shared_ptr<State> st_;
};

class StopSource {
  public:
    StopSource() : st_(std::make_shared<StopToken::State>()) {}
    StopToken token() const { return StopToken(st_); }
    void request() noexcept {
        st_->flag.store(true, std::memory_order_release);
    }

  private:
    std::shared_ptr<StopToken::State> st_;
};

// ---------------------------------------------------------------------------
// Context: ambient per-work-item data (§7.4).
// ---------------------------------------------------------------------------
struct Context {
    Deadline deadline{};  // absolute; unset == none
    TraceId trace{};
    StopToken stop{};
    std::uint32_t priority = 0;  // advisory; used by Fan and WorkerPool
};

namespace detail {
// Set by the EM around every dispatched work item so that post/push/dispatch
// pick up the ambient context automatically (§7.4).
inline thread_local const Context* tls_ambient = nullptr;
inline const Context& ambient_context() noexcept {
    static const Context empty{};
    return tls_ambient ? *tls_ambient : empty;
}
}  // namespace detail

}  // namespace afx

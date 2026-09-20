#pragma once

#include <chrono>
#include <cstdint>

#include "afx/sys/types.hpp"

namespace afx {

// Clock is a policy type (ADR-0001). The event loop calls Clock::now() once
// per iteration and caches it; now_uncached() re-reads the source.

class SteadyClock {
public:
    static TimePoint now() noexcept {
        return TimePoint(std::chrono::duration_cast<Nanos>(
            std::chrono::steady_clock::now().time_since_epoch()));
    }
    static TimePoint now_uncached() noexcept { return now(); }
    // Real clocks never step backwards and never need external advancement.
    static void advance(Duration) noexcept {}
    static constexpr bool kVirtual = false;
};

// Deterministic virtual clock for tests and simulation (§22.2).
// now() is only as fresh as the last advance()/set() by the harness.
class VirtualClock {
public:
    VirtualClock() : now_(TimePoint(Nanos(0))) {}
    explicit VirtualClock(TimePoint t) : now_(t) {}

    TimePoint now() const noexcept { return now_; }
    TimePoint now_uncached() const noexcept { return now_; }

    void advance(Duration d) noexcept { now_ += d; }
    void set(TimePoint t) noexcept { now_ = t; }

    static constexpr bool kVirtual = true;

private:
    TimePoint now_;
};

// TSC read for the flight recorder (§21.1). Cheap, monotonic-enough per core;
// cross-core merging requires invariant TSC, checked at startup.
std::uint64_t rdtsc() noexcept;
bool          tsc_invariant() noexcept;
// Calibrated TSC frequency in Hz, 0 if unknown/unavailable.
std::uint64_t tsc_hz() noexcept;

} // namespace afx

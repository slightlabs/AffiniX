#pragma once

// Per-EM counters (DESIGN.md §21). Plain thread-local counters — no atomics on
// the hot path; a collector reads them via a posted mailbox message.

#include <array>
#include <cstdint>

namespace afx {

struct Stats {
    // loop
    std::uint64_t iterations = 0;
    std::uint64_t idle_iterations = 0;
    std::uint64_t spins = 0;
    std::uint64_t blocks = 0;
    std::uint64_t wakeups = 0;

    // network
    std::uint64_t accepts = 0;
    std::uint64_t conns_opened = 0;
    std::uint64_t conns_closed = 0;
    std::uint64_t bytes_in = 0;
    std::uint64_t bytes_out = 0;
    std::uint64_t msgs_in = 0;
    std::uint64_t msgs_out = 0;
    std::uint64_t frame_errors = 0;

    // backpressure
    std::uint64_t write_hwm_hits = 0;
    std::uint64_t write_drops = 0;

    // mailbox
    std::uint64_t mailbox_pushes = 0;
    std::uint64_t mailbox_pops = 0;
    std::uint64_t mailbox_full = 0;

    // timers
    std::uint64_t timers_armed = 0;
    std::uint64_t timers_fired = 0;
    std::uint64_t timers_cancelled = 0;
    std::uint64_t timer_coalesced = 0;
    std::uint64_t groups_cancelled = 0;

    // deadlines
    std::uint64_t deadline_expired_before_start = 0;
    std::uint64_t deadline_expired_in_flight = 0;

    // errors
    std::uint64_t callback_errors = 0;
    std::uint64_t internal_errors = 0;

    // The derived metric to watch (§21): fraction of iterations that found no
    // work. Caller computes it; stored here as the numerator/denominator pair.
    double idle_ratio() const noexcept {
        return iterations ? double(idle_iterations) / double(iterations) : 0.0;
    }
};

// Tiny log-scale histogram (HDR-style: constant relative precision) for the
// latency metrics in §21. Buckets by significant bits — cheap enough for the
// hot path and honest at the tail.
class Histogram {
  public:
    // value_ns in nanoseconds; bucket index = floor(log2(max(v,1))) — 64
    // buckets cover 1 ns .. ~292 years.
    void record(std::uint64_t value_ns) noexcept {
        std::uint64_t v = value_ns | 1;
        unsigned b = 63 - __builtin_clzll(v);
        ++buckets_[b];
        ++count_;
        total_ += value_ns;
        if (value_ns > max_) max_ = value_ns;
        if (value_ns < min_) min_ = value_ns;
    }

    std::uint64_t count() const noexcept { return count_; }
    std::uint64_t max() const noexcept { return count_ ? max_ : 0; }
    std::uint64_t min() const noexcept { return count_ ? min_ : 0; }
    double mean() const noexcept {
        return count_ ? double(total_) / double(count_) : 0.0;
    }

    // Approximate percentile: the bucket floor (2^b) at which the cumulative
    // count first reaches the requested fraction.
    std::uint64_t percentile(double p) const noexcept {
        if (!count_) return 0;
        std::uint64_t need = static_cast<std::uint64_t>(p * count_) + 1;
        std::uint64_t cum = 0;
        for (unsigned b = 0; b < buckets_.size(); ++b) {
            cum += buckets_[b];
            if (cum >= need) return (std::uint64_t(1) << b);
        }
        return max_;
    }

    void reset() noexcept {
        buckets_.fill(0);
        count_ = total_ = 0;
        max_ = 0;
        min_ = ~std::uint64_t(0);
    }

  private:
    std::array<std::uint64_t, 64> buckets_{};
    std::uint64_t count_ = 0;
    std::uint64_t total_ = 0;
    std::uint64_t max_ = 0;
    std::uint64_t min_ = ~std::uint64_t(0);
};

struct LatencyMetrics {
    Histogram iteration_ns;
    Histogram timer_lateness_ns;
    Histogram mailbox_queue_ns;
    Histogram recv_to_handler_ns;
    Histogram write_queue_depth;
    Histogram deadline_headroom_ns;
};

}  // namespace afx

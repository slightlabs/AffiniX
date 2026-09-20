#pragma once

// Flight recorder — DESIGN.md §21.1. A fixed-size ring of fixed-size records
// written unconditionally on the hot path (~5 ns: one TSC read, one masked
// increment, one 32-byte store). Dumped on fault, stall, or demand.

#include <array>
#include <atomic>
#include <cstdint>
#include <unistd.h>

#include "afx/sys/clock.hpp"

namespace afx {

enum class EventKind : std::uint8_t {
    Accept, Recv, Frame, Send, TimerFire, StateChange,
    ItcPost, ItcRun, Wakeup, Backpressure, Drop,
    DeadlineExpired, CallbackError, User,
};

struct alignas(32) FlightRecord {
    std::uint64_t tsc;
    EventKind     kind;
    std::uint8_t  _pad[3];
    std::uint32_t handle;   // ConnId.idx / TimerId.idx / mailbox id
    std::uint32_t a, b;     // kind-specific: bytes, error, state, depth
    std::uint64_t trace;    // TraceId::Short
};
static_assert(sizeof(FlightRecord) == 32);

class FlightRecorder {
public:
    static constexpr std::size_t kCapacity = 4096;  // power of two

    void record(EventKind k, std::uint32_t handle,
                std::uint32_t a = 0, std::uint32_t b = 0,
                std::uint64_t trace = 0) noexcept {
#ifdef AFX_FLIGHT_RECORDER
        auto i = head_.fetch_add(1, std::memory_order_relaxed) & (kCapacity - 1);
        ring_[i] = FlightRecord{rdtsc(), k, {0,0,0}, handle, a, b, trace};
#else
        (void)k; (void)handle; (void)a; (void)b; (void)trace;
#endif
    }

    // Async-signal-safe-ish dump: raw records to fd via write(2) only.
    void dump(int fd) const noexcept {
        std::uint64_t h = head_.load(std::memory_order_relaxed);
        std::uint64_t n = h < kCapacity ? h : kCapacity;
        for (std::uint64_t i = 0; i < n; ++i) {
            auto idx = (h - n + i) & (kCapacity - 1);
            // best-effort; short writes are acceptable on a crash path
            (void)::write(fd, &ring_[idx], sizeof(FlightRecord));
        }
    }

    std::size_t size() const noexcept {
        auto h = head_.load(std::memory_order_relaxed);
        return h < kCapacity ? std::size_t(h) : kCapacity;
    }

private:
    std::array<FlightRecord, kCapacity> ring_{};
    std::atomic<std::uint64_t> head_{0};
};

} // namespace afx

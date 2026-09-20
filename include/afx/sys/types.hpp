#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace afx {

// Time vocabulary. Duration/Nanos are always nanoseconds; TimePoint is on the
// steady clock's tick so real and virtual clocks produce the same type.
using Nanos     = std::chrono::nanoseconds;
using Duration  = Nanos;
using TimePoint = std::chrono::time_point<std::chrono::steady_clock, Nanos>;

using namespace std::chrono_literals;

using ByteSpan    = std::span<const std::byte>;
using MutByteSpan = std::span<std::byte>;

} // namespace afx

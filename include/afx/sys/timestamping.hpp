#pragma once

// SO_TIMESTAMPING helpers — DESIGN.md §9.4, M8-06. RX timestamps arrive as a
// SCM_TIMESTAMPING control message on the data-path recvmsg (TX stamps go to
// the error queue and are not consumed here). The cmsg carries three
// timespecs: [0] software, [1] legacy hw-transformed (unused), [2] raw hw.

#include <sys/socket.h>
#include <time.h>
#include <cstdint>

#include "afx/backend/backend.hpp"

namespace afx {

// Parse the SCM_TIMESTAMPING cmsg of a just-received msghdr into `stamps` and
// return the CompletionFlag bits earned (HasSwStamp / HasHwStamp). Safe to
// call on any recvmsg result — finds nothing when timestamping is off.
inline std::uint32_t parse_rx_timestamping(msghdr& msg,
                                         Timestamps& stamps) noexcept {
    std::uint32_t flags = 0;
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_TIMESTAMPING)
            continue;
        const auto* ts =
            reinterpret_cast<const timespec*>(CMSG_DATA(c));
        std::uint64_t sw =
            std::uint64_t(ts[0].tv_sec) * 1'000'000'000ull +
            std::uint64_t(ts[0].tv_nsec);
        std::uint64_t hw =
            std::uint64_t(ts[2].tv_sec) * 1'000'000'000ull +
            std::uint64_t(ts[2].tv_nsec);
        if (sw) {
            stamps.sw_ns = sw;
            flags |= CompletionFlag::HasSwStamp;
        }
        if (hw) {
            stamps.hw_ns = hw;
            flags |= CompletionFlag::HasHwStamp;
        }
    }
    return flags;
}

// Control buffer big enough for the 3-timespec SCM_TIMESTAMPING cmsg.
inline constexpr std::size_t kTimestampingCbufSize =
    64;  // CMSG_SPACE(3 * sizeof(timespec)) is not constexpr on all targets

}  // namespace afx

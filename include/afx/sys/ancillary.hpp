#pragma once

// Ancillary-data helpers — SCM_RIGHTS harvest for the recvmsg paths shared
// by the readiness/proactor backends (M11-06). Kept in sys/ next to
// timestamping.hpp; no afx types leak in so the header stays POSIX-only.

#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>

namespace afx::sys {

// Harvest SCM_RIGHTS descriptors out of a just-filled recvmsg header into
// `buf` (capacity `cap`), appending to `total` — callers reset `total` at
// the start of a readiness drain and read it when emitting the completion.
// Descriptors past `cap` were already delivered into the process by the
// kernel: they are closed here rather than leaked. Returns true when the
// message was truncated (MSG_CTRUNC) or fds overflowed the inbox — the
// caller surfaces that as a completion flag, the stream itself is intact.
inline bool collect_rights(const msghdr& msg, int* buf, std::uint32_t cap,
                           std::uint32_t& total) noexcept {
    bool truncated = (msg.msg_flags & MSG_CTRUNC) != 0;
    for (auto* c = CMSG_FIRSTHDR(const_cast<msghdr*>(&msg)); c != nullptr;
         c = CMSG_NXTHDR(const_cast<msghdr*>(&msg), c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        auto bytes = std::size_t(c->cmsg_len) - CMSG_LEN(0);
        auto n = bytes / sizeof(int);
        const auto* fds = reinterpret_cast<const int*>(CMSG_DATA(c));
        for (std::size_t i = 0; i < n; ++i) {
            if (total < cap) {
                buf[total++] = fds[i];
            } else {
                ::close(fds[i]);  // delivered but unclaimable — don't leak
                truncated = true;
            }
        }
    }
    return truncated;
}

}  // namespace afx::sys

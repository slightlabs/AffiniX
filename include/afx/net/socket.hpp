#pragma once

// Socket helpers and options — non-template parts live in src/net/socket.cpp.

#include <cstddef>
#include <cstdint>

#include "afx/net/sock_addr.hpp"
#include "afx/sys/result.hpp"
#include "afx/sys/types.hpp"

namespace afx {

struct SocketOptions {
    bool        nodelay     = true;
    bool        quickack    = false;
    int         rcvbuf      = 0;     // 0 = leave default
    int         sndbuf      = 0;
    bool        keepalive   = false;
    Duration    keepalive_idle{};    // 0 = system default
    bool        timestamping = false; // SO_TIMESTAMPING (§9.4)
    bool        nonblock    = true;
};

// Flow control (§16): mandatory to configure, sane defaults.
enum class WriteOverflow : std::uint8_t {
    Disconnect,     // close the connection on write-queue overflow
    DropNewest,     // drop the bytes being sent now
    DropOldest,     // drop the oldest queued bytes
    StopReading,    // stop reading until the queue drains (proxy default)
};

struct FlowControl {
    std::size_t   write_high_watermark = 1u << 20;
    std::size_t   write_low_watermark  = 256u << 10;
    WriteOverflow on_overflow = WriteOverflow::Disconnect;
    bool          auto_pause_reads = true;   // pause reads over high watermark
};

enum class SendResult : std::uint8_t {
    Sent, Queued, Backpressured, Dropped, Closed,
};

namespace sock {

Result<int>  create(int family, const SocketOptions& opts);
Result<void> apply(int fd, const SocketOptions& opts);
Result<void> bind(int fd, const SockAddr& a, bool reuse_addr, bool reuse_port);
Result<void> listen(int fd, int backlog);
Result<void> set_nonblocking(int fd);
Result<SockAddr> local_addr(int fd);
Result<SockAddr> peer_addr(int fd);

} // namespace sock

} // namespace afx

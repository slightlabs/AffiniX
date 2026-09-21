#pragma once

// The I/O backend seam — DESIGN.md §9.1. The proactor model is canonical
// (ADR-0002): readiness-based backends emulate it by performing the syscall
// themselves when the fd reports ready.

#include <sys/socket.h>
#include <cstdint>
#include <span>

#include "afx/sys/result.hpp"
#include "afx/sys/types.hpp"

// Platform capability: kqueue is the native readiness API on macOS and the
// BSDs (M11-07). The kqueue backend and its tests compile only under this
// macro; on Linux the file is an empty TU.
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#define AFX_HAVE_KQUEUE 1
#endif

namespace afx {

class SockAddr;  // fwd; defined in net/sock_addr.hpp

// 64-bit completion tag: kind (8b) | slot (24b) | generation (32b).
struct UserData {
    std::uint64_t raw = 0;

    static constexpr UserData make(std::uint8_t kind, std::uint32_t slot,
                                   std::uint32_t gen) noexcept {
        return {(std::uint64_t(kind) << 56) |
                (std::uint64_t(slot & 0xFFFFFF) << 32) | gen};
    }
    constexpr std::uint8_t kind() const noexcept { return raw >> 56; }
    constexpr std::uint32_t slot() const noexcept {
        return (raw >> 32) & 0xFFFFFF;
    }
    constexpr std::uint32_t gen() const noexcept { return raw & 0xFFFFFFFF; }
    constexpr bool operator==(const UserData&) const = default;
};

enum class OpKind : std::uint8_t {
    None = 0,
    Watch,
    Recv,
    Send,
    Accept,
    Connect,
    FlushWrite,  // stage-6 pseudo-completion: connection may emit a sendv
};

enum class Interest : std::uint32_t {
    None = 0,
    Readable = 1u << 0,
    Writable = 1u << 1,
    ReadWrite = Readable | Writable,
};
constexpr Interest operator|(Interest a, Interest b) noexcept {
    return Interest(std::uint32_t(a) | std::uint32_t(b));
}
constexpr bool has(Interest i, Interest f) noexcept {
    return (std::uint32_t(i) & std::uint32_t(f)) != 0;
}

struct Timestamps {           // 24 bytes, all optional (§9.4)
    std::uint64_t hw_ns = 0;  // NIC hardware receive time (SO_TIMESTAMPING)
    std::uint64_t sw_ns = 0;  // kernel software receive time
    std::uint64_t tsc = 0;    // TSC at loop dequeue
};

struct CompletionFlag {
    static constexpr std::uint32_t HasHwStamp = 1u << 0;
    static constexpr std::uint32_t HasSwStamp = 1u << 1;
    static constexpr std::uint32_t More = 1u << 2;  // multishot
    // M11-06 SCM_RIGHTS: the recv carried descriptors into the registered
    // FdInbox; FdTrunc means MSG_CTRUNC — excess fds were closed, some lost.
    static constexpr std::uint32_t HasFds = 1u << 3;
    static constexpr std::uint32_t FdTrunc = 1u << 4;
    // For Watch-kind completions, `result` carries a readiness mask instead
    // of a byte count.
    static constexpr std::int32_t ReadyRead = 1;
    static constexpr std::int32_t ReadyWrite = 2;
    static constexpr std::int32_t ReadyErr = 4;
    static constexpr std::int32_t ReadyHangup = 8;
};

struct Completion {
    UserData user;
    std::int32_t result =
        0;  // bytes transferred, accepted fd, ready mask, or -errno
    std::uint32_t flags = 0;
    Timestamps stamps{};  // zeroed when unavailable
};

// SCM_RIGHTS landing pad (M11-06): registered per fd by the Connection; the
// backend fills `buf`/`count` from recvmsg cmsgs on each recv completion.
// The arrays are caller-owned — the backend writes them on the EM thread,
// the Connection reads them when the completion arrives.
struct FdInbox {
    int* buf = nullptr;  // caller storage, `cap` ints
    std::uint32_t cap = 0;
    std::uint32_t* count = nullptr;  // overwritten per recv: 0 or #stored
};

template <class B>
concept IoBackend =
    requires(B b, int fd, Interest i, UserData u, MutByteSpan mut, ByteSpan in,
             std::span<const ByteSpan> iov, std::span<Completion> out,
             Nanos timeout, const SockAddr& addr) {
        { b.attach(fd, i, u) } -> std::same_as<Result<void>>;
        { b.modify(fd, i, u) } -> std::same_as<Result<void>>;
        { b.detach(fd) } -> std::same_as<Result<void>>;

        { b.submit_recv(u, fd, mut) } -> std::same_as<Result<void>>;
        { b.submit_send(u, fd, in) } -> std::same_as<Result<void>>;
        { b.submit_sendv(u, fd, iov) } -> std::same_as<Result<void>>;
        { b.submit_accept(u, fd) } -> std::same_as<Result<void>>;
        { b.submit_connect(u, fd, addr) } -> std::same_as<Result<void>>;
        { b.cancel(u) } -> std::same_as<Result<void>>;
        { b.set_timestamping(fd, true) } -> std::same_as<void>;
        // Register the SCM_RIGHTS landing pad for fd; FdInbox{} disables.
        { b.set_fd_inbox(fd, FdInbox{}) } -> std::same_as<void>;
        // io_uring ring fd for MSG_RING cross-EM wakes (M8-05), -1 when the
        // backend has no ring.
        { b.wake_ring_fd() } -> std::same_as<int>;

        { b.wait(out, timeout) } -> std::same_as<int>;
        { b.wake() } -> std::same_as<void>;
        { B::kProactor } -> std::convertible_to<bool>;
    };

enum class BackendKind { Auto, Epoll, Uring, Kqueue, Sim };

}  // namespace afx

#pragma once

// UringBackend — native proactor over io_uring (DESIGN.md §9.2, M8).
// Unlike EpollBackend, the kernel performs the I/O: submit_* stage SQEs in
// the shared submission queue, wait() submits the batch and drains CQEs.
// Built directly on the io_uring syscall ABI (io_uring_setup/enter/register)
// rather than liburing: the ABI is stable, the ring code is small, and no
// external dependency is pulled into the build (§23 dependency policy).

#include <sys/socket.h>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "afx/backend/backend.hpp"
#include "afx/backend/epoll.hpp"

struct io_uring_sqe;
struct io_uring_cqe;
struct io_uring_buf_ring;

namespace afx {

struct UringConfig {
    std::uint32_t sq_entries = 256;      // io_uring_setup entries
    bool use_pbuf_ring = false;          // IORING_REGISTER_PBUF_RING recv path
    std::uint16_t pbuf_bgid = 0;         // buffer group id
    std::uint32_t pbuf_entries = 512;    // power of two
    std::uint32_t pbuf_buf_size = 4096;  // bytes per provided buffer
    bool multishot_accept = false;       // IORING_ACCEPT_MULTISHOT (M8-04)
};

// Result of IORING_REGISTER_PROBE — which opcodes the kernel supports.
struct UringCaps {
    std::uint32_t supported[(256 + 31) / 32] = {};

    bool supports(std::uint8_t op) const noexcept {
        return (supported[op / 32] >> (op % 32)) & 1u;
    }
};

class UringBackend {
  public:
    static constexpr bool kProactor = true;  // native proactor

    UringBackend();
    ~UringBackend();
    UringBackend(const UringBackend&) = delete;
    UringBackend& operator=(const UringBackend&) = delete;
    UringBackend(UringBackend&& o) noexcept;
    UringBackend& operator=(UringBackend&& o) noexcept;

    // Two-phase construction: returns the backend or the setup failure.
    // Auto uses this to fall back to epoll (§9.5).
    static Result<UringBackend> create(const UringConfig& cfg = {});
    // Capability probe for the ops this backend issues (M8-02).
    static Result<UringCaps> probe();

    bool valid() const noexcept { return ring_fd_ >= 0; }

    Result<void> attach(int fd, Interest i, UserData u);
    Result<void> modify(int fd, Interest i, UserData u);
    Result<void> detach(int fd);

    Result<void> submit_recv(UserData u, int fd, MutByteSpan buf);
    Result<void> submit_send(UserData u, int fd, ByteSpan data);
    Result<void> submit_sendv(UserData u, int fd,
                              std::span<const ByteSpan> iov);
    Result<void> submit_accept(UserData u, int listen_fd);
    Result<void> submit_connect(UserData u, int fd, const SockAddr& addr);
    Result<void> cancel(UserData u);

    int wait(std::span<Completion> out, Nanos timeout);
    void wake();  // thread-safe: eventfd write

    int wake_fd() const noexcept { return wake_fd_; }

  private:
    io_uring_sqe* alloc_sqe();    // stage one SQE, or nullptr when full
    void commit_sqes() noexcept;  // make staged SQEs visible
    int drain_cq(std::span<Completion> out) noexcept;
    Result<void> flush_full_sq();  // submit staged SQEs to make room
    int init(const UringConfig& cfg) noexcept;  // 0 or -errno
    int register_pbuf_ring(const UringConfig& cfg) noexcept;
    void provide_buffer(std::uint16_t bid) noexcept;
    void teardown() noexcept;

    // ---- ring pointers (mmap'd shared with the kernel) ----
    int ring_fd_ = -1;
    int wake_fd_ = -1;

    std::uint32_t* sq_head_ = nullptr;
    std::uint32_t* sq_tail_ = nullptr;
    std::uint32_t* sq_mask_ = nullptr;
    std::uint32_t* sq_entries_ = nullptr;
    std::uint32_t* sq_array_ = nullptr;
    io_uring_sqe* sqes_ = nullptr;

    std::uint32_t* cq_head_ = nullptr;
    std::uint32_t* cq_tail_ = nullptr;
    std::uint32_t* cq_mask_ = nullptr;
    io_uring_cqe* cqes_ = nullptr;

    void* sq_map_ = nullptr;
    void* cq_map_ = nullptr;
    void* sqes_map_ = nullptr;
    std::size_t sq_map_sz_ = 0;
    std::size_t cq_map_sz_ = 0;
    std::size_t sqes_map_sz_ = 0;
    std::uint32_t pending_ = 0;  // staged SQEs not yet visible to the kernel

    // ---- per-fd / per-op bookkeeping ----
    std::unordered_map<int, UserData> watch_;    // fd → watch tag
    std::unordered_set<std::uint64_t> swallow_;  // suppress one -ECANCELED
    std::unordered_set<std::uint64_t> armed_accepts_;  // live multishot accepts
    bool multishot_accept_ = false;

    struct SendvCtx {
        msghdr msg{};
        iovec iov[64]{};
    };
    std::unordered_map<std::uint64_t, SendvCtx> sendv_;  // tag → live msghdr

    struct ConnectCtx {
        sockaddr_storage ss{};
        socklen_t len = 0;
    };
    std::unordered_map<std::uint64_t, ConnectCtx> connect_;

    // Provided-buffer ring state (M8-03); only live when cfg.use_pbuf_ring.
    io_uring_buf_ring* pbuf_ = nullptr;
    void* pbuf_map_ = nullptr;
    std::size_t pbuf_map_sz_ = 0;
    std::vector<std::byte> pbuf_backing_;  // buffer memory
    std::uint32_t pbuf_mask_ = 0;
    std::uint16_t pbuf_bgid_ = 0;
    std::unordered_map<std::uint64_t, MutByteSpan>
        recv_dst_;  // tag → caller buf
};

// AutoBackend — runtime backend selection (§9.5). Probes io_uring at
// construction; falls back to epoll when ring setup or required-op probing
// fails. Dispatch is std::visit: one predictable indirect branch per call.
class AutoBackend {
  public:
    static constexpr bool kProactor = false;  // conservative: backend varies

    // kind: Auto/Uring try io_uring first (silent epoll fallback per §9.5);
    // Epoll selects epoll directly.
    explicit AutoBackend(BackendKind kind = BackendKind::Auto);
    ~AutoBackend() = default;
    AutoBackend(const AutoBackend&) = delete;
    AutoBackend& operator=(const AutoBackend&) = delete;
    AutoBackend(AutoBackend&&) noexcept = default;
    AutoBackend& operator=(AutoBackend&&) noexcept = default;

    BackendKind kind() const noexcept { return kind_; }

    Result<void> attach(int fd, Interest i, UserData u);
    Result<void> modify(int fd, Interest i, UserData u);
    Result<void> detach(int fd);

    Result<void> submit_recv(UserData u, int fd, MutByteSpan buf);
    Result<void> submit_send(UserData u, int fd, ByteSpan data);
    Result<void> submit_sendv(UserData u, int fd,
                              std::span<const ByteSpan> iov);
    Result<void> submit_accept(UserData u, int listen_fd);
    Result<void> submit_connect(UserData u, int fd, const SockAddr& addr);
    Result<void> cancel(UserData u);

    int wait(std::span<Completion> out, Nanos timeout);
    void wake();

    int wake_fd() const noexcept;

  private:
    std::variant<EpollBackend, UringBackend> impl_;
    BackendKind kind_ = BackendKind::Epoll;
};

}  // namespace afx

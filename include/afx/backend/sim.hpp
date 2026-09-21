#pragma once

// SimBackend — deterministic, kernel-free backend (DESIGN.md §9.3, §22.3).
// Paired with VirtualClock it lets tests drive an entire EventManager with no
// real I/O and no sleeping. The harness feeds bytes/accepts/connects in and
// inspects what the application wrote out.

#include <deque>
#include <unordered_map>
#include <vector>

#include "afx/backend/backend.hpp"

namespace afx {

class SimNet;

class SimBackend {
  public:
    static constexpr bool kProactor = true;  // completions are native here

    SimBackend() = default;

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

    // Virtual time, virtual wires: no stamps to deliver (§9.4).
    void set_timestamping(int, bool) {}
    // SCM_RIGHTS is a real-kernel facility; sim links carry bytes only, so
    // the inbox is registered but never fills (M11-06).
    void set_fd_inbox(int, FdInbox) {}
    // No kernel ring: MSG_RING wakes are io_uring-only (M8-05).
    int wake_ring_fd() const noexcept { return -1; }

    int wait(std::span<Completion> out, Nanos timeout);
    void wake() { ++wake_count_; }

    // ---- test / simulation API -------------------------------------------
    // Attach to a SimNet fabric (M10): submits then become scheduled events
    // with latency/faults instead of instant loopback. nullptr → standalone.
    void set_net(SimNet* n) noexcept { net_ = n; }
    SimNet* net() const noexcept { return net_; }

    int add_fd();  // allocate an fd — a real socket, so sock::apply/close on
                   // it behave; it is never driven by the kernel
    void deliver_connect(int fd, UserData u, int result);  // net completes
    void feed(int fd, ByteSpan bytes);                     // peer -> app bytes
    void feed(int fd, std::string_view s);
    void close_peer(int fd);                           // peer FIN
    void fail_peer(int fd, int err);                   // peer error
    void deliver_accept(int listen_fd, int peer_fd);   // pending accept fires
    void deliver_watch(int fd, Interest readiness);    // readiness event
    const std::vector<std::byte>& sent(int fd) const;  // what the app wrote
    int wake_count() const { return wake_count_; }
    bool recv_pending(int fd) const;

  private:
    struct FdState {
        Interest interest = Interest::None;
        UserData watch_ud{};
        bool recv_armed = false;
        UserData recv_ud{};
        MutByteSpan recv_buf{};
        bool accept_armed = false;
        UserData accept_ud{};
        std::deque<std::byte> inbound;
        std::vector<std::byte> outbound;
        bool peer_closed = false;
        int peer_err = 0;
        bool connect_pending = false;
        UserData connect_ud{};
    };

    void push(UserData u, std::int32_t res);
    void pump_recv(int fd, FdState& s);

    std::unordered_map<int, FdState> fds_;
    std::deque<Completion> ready_;
    int next_fd_ = 1000;
    int wake_count_ = 0;
    SimNet* net_ = nullptr;
};

}  // namespace afx

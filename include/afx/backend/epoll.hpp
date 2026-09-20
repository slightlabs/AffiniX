#pragma once

// EpollBackend — emulated proactor over edge-triggered epoll (DESIGN.md §9.2).
// submit_* record intent and arm readiness; when the fd reports ready the
// backend performs the syscall itself and synthesises a Completion.

#include <deque>
#include <unordered_map>
#include <vector>
#include <unistd.h>

#include "afx/backend/backend.hpp"

namespace afx {

class EpollBackend {
public:
    static constexpr bool kProactor = false;   // emulated, not native

    EpollBackend();
    ~EpollBackend();
    EpollBackend(const EpollBackend&) = delete;
    EpollBackend& operator=(const EpollBackend&) = delete;
    EpollBackend(EpollBackend&& o) noexcept { *this = std::move(o); }
    EpollBackend& operator=(EpollBackend&& o) noexcept {
        if (this == &o) return *this;
        if (wake_fd_ >= 0) ::close(wake_fd_);
        if (epfd_ >= 0)    ::close(epfd_);
        epfd_ = o.epfd_;         o.epfd_ = -1;
        wake_fd_ = o.wake_fd_;   o.wake_fd_ = -1;
        fds_ = std::move(o.fds_);
        ready_ = std::move(o.ready_);
        pending_events_ = std::move(o.pending_events_);
        return *this;
    }

    Result<void> attach(int fd, Interest i, UserData u);
    Result<void> modify(int fd, Interest i, UserData u);
    Result<void> detach(int fd);

    Result<void> submit_recv(UserData u, int fd, MutByteSpan buf);
    Result<void> submit_send(UserData u, int fd, ByteSpan data);
    Result<void> submit_sendv(UserData u, int fd, std::span<const ByteSpan> iov);
    Result<void> submit_accept(UserData u, int listen_fd);
    Result<void> submit_connect(UserData u, int fd, const SockAddr& addr);
    Result<void> cancel(UserData u);

    int  wait(std::span<Completion> out, Nanos timeout);
    void wake();   // thread-safe: eventfd write

    int wake_fd() const noexcept { return wake_fd_; }

private:
    struct SendReq {
        UserData ud;
        std::vector<std::byte> bytes;
        std::size_t off = 0;
    };
    struct FdState {
        bool     registered = false;
        Interest interest = Interest::None;
        UserData watch_ud{};

        bool        recv_armed = false;
        UserData    recv_ud{};
        MutByteSpan recv_buf{};

        std::deque<SendReq> sendq;

        bool     accept_armed = false;
        UserData accept_ud{};

        bool     connect_pending = false;
        UserData connect_ud{};
    };

    Result<void> ensure_registered(FdState& s, int fd);
    void         update_events(int fd, FdState& s);
    void         queue_completion(UserData u, std::int32_t res, std::uint32_t flags = 0);

    void on_readable(int fd, FdState& s, std::span<Completion>& out, int& n);
    void on_writable(int fd, FdState& s, std::span<Completion>& out, int& n);
    void on_errorish(int fd, FdState& s, std::uint32_t ev,
                     std::span<Completion>& out, int& n);
    void dispatch_event(int fd, FdState& s, std::uint32_t ev,
                        std::span<Completion>& out, int& n);

    int epfd_ = -1;
    int wake_fd_ = -1;

    std::unordered_map<int, FdState> fds_;
    std::deque<Completion> ready_;                    // completed without a wait
    std::deque<std::pair<int, std::uint32_t>> pending_events_;  // overflow
};

} // namespace afx

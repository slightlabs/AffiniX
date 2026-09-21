#pragma once

// KqueueBackend — emulated proactor over kqueue (DESIGN.md §9.2, M11-07).
// Identical structure to EpollBackend: submit_* record intent and arm
// EVFILT_READ/WRITE with EV_CLEAR; when the filter fires the backend
// performs the syscall itself and synthesises a Completion. Wake is an
// EVFILT_USER ident — no eventfd needed.
//
// The class only exists where <sys/event.h> does (AFX_HAVE_KQUEUE); the
// header is a no-op elsewhere so shared code can include it freely.

#include "afx/backend/backend.hpp"

#ifdef AFX_HAVE_KQUEUE

#include <sys/event.h>
#include <unistd.h>
#include <deque>
#include <unordered_map>
#include <vector>

namespace afx {

class KqueueBackend {
  public:
    static constexpr bool kProactor = false;  // emulated, not native

    KqueueBackend();
    ~KqueueBackend();
    KqueueBackend(const KqueueBackend&) = delete;
    KqueueBackend& operator=(const KqueueBackend&) = delete;
    KqueueBackend(KqueueBackend&& o) noexcept { *this = std::move(o); }
    KqueueBackend& operator=(KqueueBackend&& o) noexcept {
        if (this == &o) return *this;
        if (kq_ >= 0) ::close(kq_);
        kq_ = o.kq_;
        o.kq_ = -1;
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
    Result<void> submit_sendv(UserData u, int fd,
                              std::span<const ByteSpan> iov);
    Result<void> submit_accept(UserData u, int listen_fd);
    Result<void> submit_connect(UserData u, int fd, const SockAddr& addr);
    Result<void> cancel(UserData u);

    // SO_TIMESTAMPING does not exist on macOS/BSD — the flag is stored so
    // portable code compiles, but no stamps are ever delivered.
    void set_timestamping(int fd, bool on);
    // SCM_RIGHTS landing pad (M11-06): recv switches to recvmsg and
    // descriptors are harvested into the inbox per drain. FdInbox{} clears.
    void set_fd_inbox(int fd, FdInbox in);

    int wait(std::span<Completion> out, Nanos timeout);
    void wake();  // thread-safe: EVFILT_USER NOTE_TRIGGER

    int wake_fd() const noexcept { return -1; }  // EVFILT_USER needs no fd
    // No ring to target — MSG_RING wakes are io_uring-only (M8-05).
    int wake_ring_fd() const noexcept { return -1; }

  private:
    struct SendReq {
        UserData ud;
        std::vector<std::byte> bytes;
        std::size_t off = 0;
    };
    struct FdState {
        Interest interest = Interest::None;
        UserData watch_ud{};
        // Which filters are currently registered with kqueue — lets
        // update_filters skip no-op EV_DELETEs and duplicate EV_ADDs.
        bool rd_armed = false;
        bool wr_armed = false;

        bool recv_armed = false;
        UserData recv_ud{};
        MutByteSpan recv_buf{};
        FdInbox fd_inbox{};  // SCM_RIGHTS landing pad (M11-06)

        std::deque<SendReq> sendq;

        bool accept_armed = false;
        UserData accept_ud{};

        bool connect_pending = false;
        UserData connect_ud{};
    };

    void update_filters(int fd, FdState& s);
    void queue_completion(UserData u, std::int32_t res,
                          std::uint32_t flags = 0);

    void on_readable(int fd, FdState& s, std::span<Completion>& out, int& n);
    void on_writable(int fd, FdState& s, std::span<Completion>& out, int& n);
    void on_errorish(int fd, FdState& s, const kevent& ev,
                     std::span<Completion>& out, int& n);
    void dispatch_event(int fd, FdState& s, const kevent& ev,
                        std::span<Completion>& out, int& n);

    int kq_ = -1;

    std::unordered_map<int, FdState> fds_;
    std::deque<Completion> ready_;       // completed without a wait
    std::deque<kevent> pending_events_;  // overflow from a saturated wait
};

}  // namespace afx

#endif  // AFX_HAVE_KQUEUE

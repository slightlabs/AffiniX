// KqueueBackend — emulated proactor over kqueue (M11-07). A faithful port of
// EpollBackend's structure: submit_* arm EVFILT_READ/WRITE (EV_CLEAR for
// edge semantics), the wait loop performs the syscalls on readiness and
// synthesises Completions. Differences that matter:
//   - wake() is an EVFILT_USER ident, not an eventfd;
//   - accept() + fcntl stands in for accept4 (absent on macOS);
//   - MSG_NOSIGNAL is 0 here — SO_NOSIGPIPE is set at socket create;
//   - EV_EOF plays EPOLLRDHUP's role: drain pending bytes, then recv()==0.
// Compiles to an empty TU outside AFX_HAVE_KQUEUE.

#include "afx/backend/kqueue.hpp"

#ifdef AFX_HAVE_KQUEUE

#include <fcntl.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include "afx/net/sock_addr.hpp"
#include "afx/sys/ancillary.hpp"
#include "afx/sys/clock.hpp"
#include "afx/sys/compat.hpp"

namespace afx {

static_assert(IoBackend<KqueueBackend>);

namespace {
constexpr std::size_t kAcceptsPerWait = 32;
// recvmsg control buffer: sized for an SCM_RIGHTS batch (cap 8 fds).
constexpr std::size_t kAncillaryCbufSize = CMSG_SPACE(8 * sizeof(int)) > 64
                                               ? CMSG_SPACE(8 * sizeof(int))
                                               : 64;
// EVFILT_USER ident used for cross-thread wake — arbitrary constant.
constexpr std::uintptr_t kWakeIdent = 0xAF11E;

// accept() + the flags accept4 would apply (macOS has no accept4).
int accept_cloexec(int fd) {
    int cfd = ::accept(fd, nullptr, nullptr);
    if (cfd < 0) return cfd;
    int fl = ::fcntl(cfd, F_GETFL, 0);
    if (fl >= 0) ::fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
    ::fcntl(cfd, F_SETFD, FD_CLOEXEC);
    return cfd;
}
}  // namespace

KqueueBackend::KqueueBackend() {
    kq_ = ::kqueue();
    if (kq_ >= 0) {
        struct kevent ev {};
        EV_SET(&ev, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        ::kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    }
}

KqueueBackend::~KqueueBackend() {
    if (kq_ >= 0) ::close(kq_);
}

// ---------------------------------------------------------------------------

Result<void> KqueueBackend::attach(int fd, Interest i, UserData u) {
    auto& s = fds_[fd];
    s.interest = i;
    s.watch_ud = u;
    update_filters(fd, s);
    return {};
}

Result<void> KqueueBackend::modify(int fd, Interest i, UserData u) {
    auto it = fds_.find(fd);
    if (it == fds_.end()) return make_error(ErrorCategory::Sys, Err::NotFound);
    it->second.interest = i;
    it->second.watch_ud = u;
    update_filters(fd, it->second);
    return {};
}

Result<void> KqueueBackend::detach(int fd) {
    auto it = fds_.find(fd);
    if (it == fds_.end()) return {};
    // Closing the fd removes its filters implicitly; drop the state.
    fds_.erase(it);
    return {};
}

// ---------------------------------------------------------------------------

Result<void> KqueueBackend::submit_recv(UserData u, int fd, MutByteSpan buf) {
    auto& s = fds_[fd];
    if (s.recv_armed) return make_error(ErrorCategory::Internal, Err::Invalid);
    s.recv_armed = true;
    s.recv_ud = u;
    s.recv_buf = buf;
    update_filters(fd, s);
    return {};
}

Result<void> KqueueBackend::submit_send(UserData u, int fd, ByteSpan data) {
    ByteSpan arr[1] = {data};
    return submit_sendv(u, fd, arr);
}

Result<void> KqueueBackend::submit_sendv(UserData u, int fd,
                                         std::span<const ByteSpan> iov) {
    auto& s = fds_[fd];

    std::size_t total = 0;
    for (auto b : iov) total += b.size();

    if (!s.sendq.empty()) {
        // Preserve ordering: queue behind earlier sends.
        SendReq req{u, std::vector<std::byte>(total), 0};
        std::size_t off = 0;
        for (auto b : iov) {
            std::memcpy(req.bytes.data() + off, b.data(), b.size());
            off += b.size();
        }
        s.sendq.push_back(std::move(req));
        update_filters(fd, s);
        return {};
    }

    // Fast path: try the write immediately, no copy.
    iovec vec[64];
    msghdr msg{};
    msg.msg_iov = vec;
    int niov = 0;
    for (auto b : iov) {
        if (niov == 64) break;
        vec[niov].iov_base = const_cast<std::byte*>(b.data());
        vec[niov].iov_len = b.size();
        ++niov;
    }
    msg.msg_iovlen = niov;
    ssize_t w = ::sendmsg(fd, &msg, MSG_NOSIGNAL);

    if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        queue_completion(u, -errno);
        return {};
    }
    std::size_t sent = w < 0 ? 0 : std::size_t(w);
    if (sent == total) {
        queue_completion(u, std::int32_t(sent));
        return {};
    }
    // Partial/EAGAIN: copy the tail and arm EVFILT_WRITE.
    SendReq req{u, std::vector<std::byte>(total - sent), 0};
    std::size_t wpos = 0, skip = sent;
    for (auto b : iov) {
        if (skip >= b.size()) {
            skip -= b.size();
            continue;
        }
        std::size_t take = b.size() - skip;
        std::memcpy(req.bytes.data() + wpos, b.data() + skip, take);
        wpos += take;
        skip = 0;
    }
    s.sendq.push_back(std::move(req));
    update_filters(fd, s);
    return {};
}

Result<void> KqueueBackend::submit_accept(UserData u, int listen_fd) {
    auto& s = fds_[listen_fd];
    s.accept_armed = true;
    s.accept_ud = u;
    update_filters(listen_fd, s);
    return {};
}

Result<void> KqueueBackend::submit_connect(UserData u, int fd,
                                           const SockAddr& addr) {
    auto& s = fds_[fd];
    int r = ::connect(fd, addr.addr(), addr.len());
    if (r == 0) {
        queue_completion(u, 0);
        return {};
    }
    if (errno != EINPROGRESS) {
        queue_completion(u, -errno);
        return {};
    }
    s.connect_pending = true;
    s.connect_ud = u;
    update_filters(fd, s);
    return {};
}

void KqueueBackend::set_timestamping(int fd, bool on) {
    // SO_TIMESTAMPING is a Linux facility; stored nowhere, delivered never.
    (void)fd;
    (void)on;
}

void KqueueBackend::set_fd_inbox(int fd, FdInbox in) {
    fds_[fd].fd_inbox = in;
}

Result<void> KqueueBackend::cancel(UserData u) {
    for (auto& [fd, s] : fds_) {
        bool found = false;
        if (s.recv_armed && s.recv_ud == u) {
            s.recv_armed = false;
            queue_completion(u, -ECANCELED);
            found = true;
        }
        for (auto it = s.sendq.begin(); it != s.sendq.end();) {
            if (it->ud == u) {
                queue_completion(u, -ECANCELED);
                it = s.sendq.erase(it);
                found = true;
            } else
                ++it;
        }
        if (s.accept_armed && s.accept_ud == u) {
            s.accept_armed = false;
            queue_completion(u, -ECANCELED);
            found = true;
        }
        if (s.connect_pending && s.connect_ud == u) {
            s.connect_pending = false;
            queue_completion(u, -ECANCELED);
            found = true;
        }
        if (found) update_filters(fd, s);
    }
    return {};
}

// ---------------------------------------------------------------------------

int KqueueBackend::wait(std::span<Completion> out, Nanos timeout) {
    int n = 0;
    while (n < int(out.size()) && !ready_.empty()) {
        out[n++] = ready_.front();
        ready_.pop_front();
    }
    if (n == int(out.size())) return n;

    // Any pending events left over from a previous saturated wait.
    while (n < int(out.size()) && !pending_events_.empty()) {
        struct kevent ev = pending_events_.front();
        pending_events_.pop_front();
        auto it = fds_.find(int(ev.ident));
        if (it == fds_.end()) continue;
        dispatch_event(int(ev.ident), it->second, ev, out, n);
    }
    if (n == int(out.size())) return n;

    struct kevent evs[64];
    timespec ts{timeout.count() / 1'000'000'000,
                timeout.count() % 1'000'000'000};
    // Completions already queued (or a zero timeout) → poll, don't block.
    if (!ready_.empty() || n > 0 || timeout <= Nanos::zero()) ts = {0, 0};
    int r;
    do {
        r = ::kevent(kq_, nullptr, 0, evs, 64, &ts);
    } while (r < 0 && errno == EINTR);
    if (r < 0) return n;

    for (int i = 0; i < r; ++i) {
        const struct kevent& ev = evs[i];
        if (ev.filter == EVFILT_USER && ev.ident == kWakeIdent)
            continue;  // wake marker — EV_CLEAR already consumed it
        int fd = int(ev.ident);
        auto it = fds_.find(fd);
        if (it == fds_.end()) continue;
        if (n < int(out.size()))
            dispatch_event(fd, it->second, ev, out, n);
        else
            pending_events_.push_back(ev);
    }
    return n;
}

void KqueueBackend::wake() {
    struct kevent ev {};
    EV_SET(&ev, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    ::kevent(kq_, &ev, 1, nullptr, 0, nullptr);
}

// ---------------------------------------------------------------------------
// internals
// ---------------------------------------------------------------------------

void KqueueBackend::update_filters(int fd, FdState& s) {
    bool rd =
        s.recv_armed || s.accept_armed || has(s.interest, Interest::Readable);
    bool wr = !s.sendq.empty() || s.connect_pending ||
              has(s.interest, Interest::Writable);
    // Track armed state so we never EV_DELETE a filter that was never added
    // (harmless ENOENT, but a wasted syscall either way).
    if (rd != s.rd_armed) {
        struct kevent ev {};
        EV_SET(&ev, fd, EVFILT_READ, rd ? (EV_ADD | EV_CLEAR) : EV_DELETE, 0, 0,
               nullptr);
        ::kevent(kq_, &ev, 1, nullptr, 0, nullptr);
        s.rd_armed = rd;
    }
    if (wr != s.wr_armed) {
        struct kevent ev {};
        EV_SET(&ev, fd, EVFILT_WRITE, wr ? (EV_ADD | EV_CLEAR) : EV_DELETE, 0,
               0, nullptr);
        ::kevent(kq_, &ev, 1, nullptr, 0, nullptr);
        s.wr_armed = wr;
    }
}

void KqueueBackend::queue_completion(UserData u, std::int32_t res,
                                     std::uint32_t flags) {
    Completion c{};
    c.user = u;
    c.result = res;
    c.flags = flags;
    c.stamps.tsc = rdtsc();
    ready_.push_back(c);
}

void KqueueBackend::on_readable(int fd, FdState& s, std::span<Completion>& out,
                                int& n) {
    // Watch interest takes readiness completions.
    if (has(s.interest, Interest::Readable)) {
        if (n < int(out.size())) {
            Completion c{};
            c.user = s.watch_ud;
            c.result = CompletionFlag::ReadyRead;
            c.stamps.tsc = rdtsc();
            out[n++] = c;
        } else {
            struct kevent ev {};
            EV_SET(&ev, fd, EVFILT_READ, 0, 0, 0, nullptr);
            pending_events_.push_back(ev);
        }
    }

    if (s.accept_armed) {
        for (std::size_t i = 0; i < kAcceptsPerWait; ++i) {
            int cfd = accept_cloexec(fd);
            if (cfd < 0) break;
            if (n < int(out.size())) {
                Completion c{};
                c.user = s.accept_ud;
                c.result = cfd;
                c.stamps.tsc = rdtsc();
                out[n++] = c;
            } else {
                ::close(cfd);  // cannot queue it; drop
                struct kevent ev {};
                EV_SET(&ev, fd, EVFILT_READ, 0, 0, 0, nullptr);
                pending_events_.push_back(ev);
                break;
            }
        }
        return;
    }

    if (!s.recv_armed) return;

    // Emulated proactor: drain until EAGAIN or buffer full, one completion.
    std::size_t total = 0;
    std::int32_t result = 0;
    std::uint32_t n_fds = 0;  // SCM_RIGHTS harvested across this drain
    bool fds_trunc = false;
    const bool want_msg = s.fd_inbox.buf != nullptr;
    for (;;) {
        MutByteSpan rem = s.recv_buf.subspan(total);
        if (rem.empty()) {
            result = std::int32_t(total);
            break;
        }
        ssize_t r;
        if (want_msg) {
            alignas(cmsghdr) char cbuf[kAncillaryCbufSize];
            iovec iov{rem.data(), rem.size()};
            msghdr msg{};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = cbuf;
            msg.msg_controllen = sizeof(cbuf);
            r = ::recvmsg(fd, &msg, MSG_NOSIGNAL);
            if (r > 0)
                fds_trunc |= sys::collect_rights(msg, s.fd_inbox.buf,
                                                 s.fd_inbox.cap, n_fds);
        } else {
            r = ::recv(fd, rem.data(), rem.size(), MSG_NOSIGNAL);
        }
        if (r > 0) {
            total += std::size_t(r);
            continue;
        }
        if (r == 0) {
            result = total ? std::int32_t(total) : 0;
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            result = total ? std::int32_t(total) : -EAGAIN;
        } else if (errno == EINTR) {
            continue;
        } else {
            result = -errno;
        }
        break;
    }

    if (result == -EAGAIN && n_fds == 0) return;  // nothing to report yet
    s.recv_armed = false;
    update_filters(fd, s);
    if (s.fd_inbox.count) *s.fd_inbox.count = n_fds;
    std::uint32_t flags = 0;
    if (n_fds) flags |= CompletionFlag::HasFds;
    if (fds_trunc) flags |= CompletionFlag::FdTrunc;
    if (n < int(out.size())) {
        Completion c{};
        c.user = s.recv_ud;
        c.result = result;
        c.flags = flags;
        c.stamps.tsc = rdtsc();
        out[n++] = c;
    } else {
        struct kevent ev {};
        EV_SET(&ev, fd, EVFILT_READ, 0, 0, 0, nullptr);
        pending_events_.push_back(ev);
        s.recv_armed = true;  // re-arm so the event is not lost
        update_filters(fd, s);
    }
}

void KqueueBackend::on_writable(int fd, FdState& s, std::span<Completion>& out,
                                int& n) {
    if (s.connect_pending) {
        s.connect_pending = false;
        int err = 0;
        socklen_t l = sizeof(err);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
        update_filters(fd, s);
        if (n < int(out.size())) {
            Completion c{};
            c.user = s.connect_ud;
            c.result = err ? -err : 0;
            c.stamps.tsc = rdtsc();
            out[n++] = c;
        } else {
            struct kevent ev {};
            EV_SET(&ev, fd, EVFILT_WRITE, 0, 0, 0, nullptr);
            pending_events_.push_back(ev);
        }
    }

    while (!s.sendq.empty()) {
        SendReq& req = s.sendq.front();
        ssize_t w = ::send(fd, req.bytes.data() + req.off,
                           req.bytes.size() - req.off, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            Completion c{};
            c.user = req.ud;
            c.result = -errno;
            c.stamps.tsc = rdtsc();
            s.sendq.pop_front();
            update_filters(fd, s);
            if (n < int(out.size())) {
                out[n++] = c;
            } else {
                struct kevent ev {};
                EV_SET(&ev, fd, EVFILT_WRITE, 0, 0, 0, nullptr);
                pending_events_.push_back(ev);
            }
            continue;
        }
        req.off += std::size_t(w);
        if (req.off == req.bytes.size()) {
            Completion c{};
            c.user = req.ud;
            c.result = std::int32_t(req.bytes.size());
            c.stamps.tsc = rdtsc();
            s.sendq.pop_front();
            update_filters(fd, s);
            if (n < int(out.size())) {
                out[n++] = c;
            } else {
                struct kevent ev {};
                EV_SET(&ev, fd, EVFILT_WRITE, 0, 0, 0, nullptr);
                pending_events_.push_back(ev);
            }
        }
    }

    if (has(s.interest, Interest::Writable)) {
        if (n < int(out.size())) {
            Completion c{};
            c.user = s.watch_ud;
            c.result = CompletionFlag::ReadyWrite;
            c.stamps.tsc = rdtsc();
            out[n++] = c;
        } else {
            struct kevent ev {};
            EV_SET(&ev, fd, EVFILT_WRITE, 0, 0, 0, nullptr);
            pending_events_.push_back(ev);
        }
    }
}

void KqueueBackend::on_errorish(int fd, FdState& s, const struct kevent& ev,
                                std::span<Completion>& out, int& n) {
    // Surface errors through whichever op is armed. EV_EOF on a read filter
    // with a live recv means peer closed: the drain itself returns 0, so
    // this path only needs to cover the cases where nothing is armed.
    if (s.recv_armed && ev.filter == EVFILT_READ && !(ev.flags & EV_ERROR))
        return;  // EV_EOF — let the read drain produce its 0
    if (s.recv_armed) {
        s.recv_armed = false;
        int err = int(ev.data) ? int(ev.data) : 0;
        if (ev.flags & EV_ERROR) {
            socklen_t l = sizeof(err);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
        }
        update_filters(fd, s);
        if (n < int(out.size())) {
            Completion c{};
            c.user = s.recv_ud;
            c.result = err ? -err : 0;  // 0 == EOF
            c.stamps.tsc = rdtsc();
            out[n++] = c;
        } else {
            pending_events_.push_back(ev);
            s.recv_armed = true;
            update_filters(fd, s);
        }
        return;
    }
    if (s.interest != Interest::None) {
        if (n < int(out.size())) {
            Completion c{};
            c.user = s.watch_ud;
            c.result = ((ev.flags & EV_ERROR) ? CompletionFlag::ReadyErr : 0) |
                       ((ev.flags & EV_EOF) ? CompletionFlag::ReadyHangup : 0);
            c.stamps.tsc = rdtsc();
            out[n++] = c;
        } else {
            pending_events_.push_back(ev);
        }
    }
}

void KqueueBackend::dispatch_event(int fd, FdState& s, const struct kevent& ev,
                                   std::span<Completion>& out, int& n) {
    if (ev.flags & (EV_ERROR | EV_EOF)) on_errorish(fd, s, ev, out, n);
    if (ev.filter == EVFILT_READ) on_readable(fd, s, out, n);
    if (ev.filter == EVFILT_WRITE) on_writable(fd, s, out, n);
}

}  // namespace afx

#endif  // AFX_HAVE_KQUEUE

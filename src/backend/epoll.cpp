#include "afx/backend/epoll.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include "afx/net/sock_addr.hpp"
#include "afx/sys/clock.hpp"
#include "afx/sys/timestamping.hpp"

namespace afx {

namespace {
constexpr std::size_t kAcceptsPerWait = 32;

std::uint32_t to_epoll(Interest i) noexcept {
    std::uint32_t e = 0;
    if (has(i, Interest::Readable)) e |= EPOLLIN;
    if (has(i, Interest::Writable)) e |= EPOLLOUT;
    return e;
}
}  // namespace

EpollBackend::EpollBackend() {
    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (epfd_ >= 0 && wake_fd_ >= 0) {
        epoll_event ev{};
        ev.events = EPOLLIN;  // level-triggered: any pending wake counts once
        ev.data.fd = wake_fd_;
        ::epoll_ctl(epfd_, EPOLL_CTL_ADD, wake_fd_, &ev);
    }
}

EpollBackend::~EpollBackend() {
    if (wake_fd_ >= 0) ::close(wake_fd_);
    if (epfd_ >= 0) ::close(epfd_);
}

// ---------------------------------------------------------------------------

Result<void> EpollBackend::attach(int fd, Interest i, UserData u) {
    auto& s = fds_[fd];
    s.interest = i;
    s.watch_ud = u;
    if (!s.registered) {
        if (auto r = ensure_registered(s, fd); !r) return r;
    }
    update_events(fd, s);
    return {};
}

Result<void> EpollBackend::modify(int fd, Interest i, UserData u) {
    auto it = fds_.find(fd);
    if (it == fds_.end()) return make_error(ErrorCategory::Sys, Err::NotFound);
    it->second.interest = i;
    it->second.watch_ud = u;
    update_events(fd, it->second);
    return {};
}

Result<void> EpollBackend::detach(int fd) {
    auto it = fds_.find(fd);
    if (it == fds_.end()) return {};
    if (it->second.registered) ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    fds_.erase(it);
    return {};
}

// ---------------------------------------------------------------------------

Result<void> EpollBackend::submit_recv(UserData u, int fd, MutByteSpan buf) {
    auto& s = fds_[fd];
    if (s.recv_armed) return make_error(ErrorCategory::Internal, Err::Invalid);
    s.recv_armed = true;
    s.recv_ud = u;
    s.recv_buf = buf;
    if (!s.registered) {
        if (auto r = ensure_registered(s, fd); !r) return r;
    }
    update_events(fd, s);
    return {};
}

Result<void> EpollBackend::submit_send(UserData u, int fd, ByteSpan data) {
    ByteSpan arr[1] = {data};
    return submit_sendv(u, fd, arr);
}

Result<void> EpollBackend::submit_sendv(UserData u, int fd,
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
        update_events(fd, s);
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
    // Partial/EAGAIN: copy the tail and arm EPOLLOUT.
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
    if (!s.registered) {
        if (auto r = ensure_registered(s, fd); !r) return r;
    }
    update_events(fd, s);
    return {};
}

Result<void> EpollBackend::submit_accept(UserData u, int listen_fd) {
    auto& s = fds_[listen_fd];
    s.accept_armed = true;
    s.accept_ud = u;
    if (!s.registered) {
        if (auto r = ensure_registered(s, listen_fd); !r) return r;
    }
    update_events(listen_fd, s);
    return {};
}

Result<void> EpollBackend::submit_connect(UserData u, int fd,
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
    if (!s.registered) {
        if (auto rr = ensure_registered(s, fd); !rr) return rr;
    }
    update_events(fd, s);
    return {};
}

void EpollBackend::set_timestamping(int fd, bool on) {
    // May run before any submit_* (Connection::start calls it first): create
    // the FdState entry rather than requiring prior registration.
    fds_[fd].timestamping = on;
}

Result<void> EpollBackend::cancel(UserData u) {
    bool found = false;
    for (auto& [fd, s] : fds_) {
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
        if (found) update_events(fd, s);
        found = false;
    }
    return {};
}

// ---------------------------------------------------------------------------

int EpollBackend::wait(std::span<Completion> out, Nanos timeout) {
    int n = 0;
    while (n < int(out.size()) && !ready_.empty()) {
        out[n++] = ready_.front();
        ready_.pop_front();
    }
    if (n == int(out.size())) return n;

    // Any pending events left over from a previous saturated wait.
    while (n < int(out.size()) && !pending_events_.empty()) {
        auto [fd, ev] = pending_events_.front();
        pending_events_.pop_front();
        auto it = fds_.find(fd);
        if (it == fds_.end()) continue;
        dispatch_event(fd, it->second, ev, out, n);
    }
    if (n == int(out.size())) return n;

    epoll_event evs[64];
    int r;
    if (!ready_.empty() || n > 0 || timeout <= Nanos::zero()) {
        timespec ts{0, 0};
        do {
            r = ::epoll_pwait2(epfd_, evs, 64, &ts, nullptr);
        } while (r < 0 && errno == EINTR);
    } else {
        timespec ts{timeout.count() / 1'000'000'000,
                    timeout.count() % 1'000'000'000};
        do {
            r = ::epoll_pwait2(epfd_, evs, 64, &ts, nullptr);
        } while (r < 0 && errno == EINTR);
    }
    if (r < 0) return n;

    for (int i = 0; i < r; ++i) {
        int fd = evs[i].data.fd;
        std::uint32_t ev = evs[i].events;
        if (fd == wake_fd_) {
            std::uint64_t v;
            while (::read(wake_fd_, &v, sizeof(v)) == sizeof(v)) {}
            continue;
        }
        auto it = fds_.find(fd);
        if (it == fds_.end()) continue;
        if (n < int(out.size()))
            dispatch_event(fd, it->second, ev, out, n);
        else
            pending_events_.emplace_back(fd, ev);
    }
    return n;
}

void EpollBackend::wake() {
    std::uint64_t one = 1;
    if (::write(wake_fd_, &one, sizeof(one)) <
        0) { /* full: a wake is already pending */
    }
}

// ---------------------------------------------------------------------------
// internals
// ---------------------------------------------------------------------------

Result<void> EpollBackend::ensure_registered(FdState& s, int fd) {
    epoll_event ev{};
    ev.events = EPOLLET | EPOLLRDHUP;
    ev.data.fd = fd;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) return last_errno();
    s.registered = true;
    return {};
}

void EpollBackend::update_events(int fd, FdState& s) {
    if (!s.registered) return;
    std::uint32_t ev = EPOLLET | EPOLLRDHUP | to_epoll(s.interest);
    if (s.recv_armed || s.accept_armed) ev |= EPOLLIN;
    if (!s.sendq.empty() || s.connect_pending) ev |= EPOLLOUT;
    epoll_event e{};
    e.events = ev;
    e.data.fd = fd;
    ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &e);
}

void EpollBackend::queue_completion(UserData u, std::int32_t res,
                                    std::uint32_t flags) {
    Completion c{};
    c.user = u;
    c.result = res;
    c.flags = flags;
    c.stamps.tsc = rdtsc();
    ready_.push_back(c);
}

void EpollBackend::on_readable(int fd, FdState& s, std::span<Completion>& out,
                               int& n) {
    // Watch interest takes readiness completions.
    if (has(s.interest, Interest::Readable)) {
        if (n < int(out.size())) {
            Completion c{};
            c.user = s.watch_ud;
            c.result = CompletionFlag::ReadyRead;
            c.stamps.tsc = rdtsc();
            out[n++] = c;
        } else
            pending_events_.emplace_back(fd, EPOLLIN);
    }

    if (s.accept_armed) {
        for (std::size_t i = 0; i < kAcceptsPerWait; ++i) {
            int cfd =
                ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (cfd < 0) break;
            if (n < int(out.size())) {
                Completion c{};
                c.user = s.accept_ud;
                c.result = cfd;
                c.stamps.tsc = rdtsc();
                out[n++] = c;
            } else {
                // Completion capacity exhausted: stash fd for next wait.
                pending_events_.emplace_back(fd, EPOLLIN);
                ::close(cfd);  // cannot queue it; drop
                break;
            }
        }
        return;
    }

    if (!s.recv_armed) return;

    // Emulated proactor: drain until EAGAIN or buffer full, one completion.
    std::size_t total = 0;
    std::int32_t result = 0;
    Timestamps stamps{};
    std::uint32_t ts_flags = 0;
    for (;;) {
        MutByteSpan rem = s.recv_buf.subspan(total);
        if (rem.empty()) {
            result = std::int32_t(total);
            break;
        }
        ssize_t r;
#ifdef AFX_WITH_TIMESTAMPING
        if (s.timestamping) {
            // SO_TIMESTAMPING delivers RX stamps as a cmsg on the data path;
            // keep the newest read's stamps (they describe the latest byte).
            alignas(cmsghdr) char cbuf[kTimestampingCbufSize];
            iovec iov{rem.data(), rem.size()};
            msghdr msg{};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = cbuf;
            msg.msg_controllen = sizeof(cbuf);
            r = ::recvmsg(fd, &msg, MSG_NOSIGNAL);
            if (r > 0) ts_flags |= parse_rx_timestamping(msg, stamps);
        } else
#endif
        {
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

    if (result == -EAGAIN) return;  // nothing to report yet
    s.recv_armed = false;
    update_events(fd, s);
    if (n < int(out.size())) {
        Completion c{};
        c.user = s.recv_ud;
        c.result = result;
        c.flags = ts_flags;
        c.stamps.tsc = rdtsc();
        c.stamps.hw_ns = stamps.hw_ns;
        c.stamps.sw_ns = stamps.sw_ns;
        out[n++] = c;
    } else {
        pending_events_.emplace_back(fd, EPOLLIN);
        s.recv_armed = true;  // re-arm so the event is not lost
        update_events(fd, s);
    }
}

void EpollBackend::on_writable(int fd, FdState& s, std::span<Completion>& out,
                               int& n) {
    if (s.connect_pending) {
        s.connect_pending = false;
        int err = 0;
        socklen_t l = sizeof(err);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
        update_events(fd, s);
        if (n < int(out.size())) {
            Completion c{};
            c.user = s.connect_ud;
            c.result = err ? -err : 0;
            c.stamps.tsc = rdtsc();
            out[n++] = c;
        } else
            pending_events_.emplace_back(fd, EPOLLOUT);
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
            update_events(fd, s);
            if (n < int(out.size()))
                out[n++] = c;
            else
                pending_events_.emplace_back(fd, EPOLLOUT);
            continue;
        }
        req.off += std::size_t(w);
        if (req.off == req.bytes.size()) {
            Completion c{};
            c.user = req.ud;
            c.result = std::int32_t(req.bytes.size());
            c.stamps.tsc = rdtsc();
            s.sendq.pop_front();
            update_events(fd, s);
            if (n < int(out.size()))
                out[n++] = c;
            else
                pending_events_.emplace_back(fd, EPOLLOUT);
        }
    }

    if (has(s.interest, Interest::Writable)) {
        if (n < int(out.size())) {
            Completion c{};
            c.user = s.watch_ud;
            c.result = CompletionFlag::ReadyWrite;
            c.stamps.tsc = rdtsc();
            out[n++] = c;
        } else
            pending_events_.emplace_back(fd, EPOLLOUT);
    }
}

void EpollBackend::on_errorish(int fd, FdState& s, std::uint32_t ev,
                               std::span<Completion>& out, int& n) {
    // Surface errors through whichever op is armed.
    if (s.recv_armed) {
        s.recv_armed = false;
        int err = 0;
        socklen_t l = sizeof(err);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
        update_events(fd, s);
        if (n < int(out.size())) {
            Completion c{};
            c.user = s.recv_ud;
            c.result = err ? -err : 0;  // 0 == EOF on HUP/RDHUP
            c.stamps.tsc = rdtsc();
            out[n++] = c;
        } else {
            pending_events_.emplace_back(fd, ev);
            s.recv_armed = true;
            update_events(fd, s);
        }
        return;
    }
    if (s.interest != Interest::None) {
        if (n < int(out.size())) {
            Completion c{};
            c.user = s.watch_ud;
            c.result =
                ((ev & EPOLLERR) ? CompletionFlag::ReadyErr : 0) |
                ((ev & (EPOLLHUP | EPOLLRDHUP)) ? CompletionFlag::ReadyHangup
                                                : 0);
            c.stamps.tsc = rdtsc();
            out[n++] = c;
        } else
            pending_events_.emplace_back(fd, ev);
    }
}

void EpollBackend::dispatch_event(int fd, FdState& s, std::uint32_t ev,
                                  std::span<Completion>& out, int& n) {
    if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) on_errorish(fd, s, ev, out, n);
    if (ev & EPOLLIN) on_readable(fd, s, out, n);
    if (ev & EPOLLOUT) on_writable(fd, s, out, n);
}

}  // namespace afx

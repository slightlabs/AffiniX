#include "afx/net/socket.hpp"

#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#if defined(AFX_WITH_TIMESTAMPING) && defined(__linux__)
#include <linux/net_tstamp.h>  // SOF_TIMESTAMPING_*
#endif

#include "afx/sys/compat.hpp"

namespace afx::sock {

namespace {

// socket() with NONBLOCK/CLOEXEC is Linux/FreeBSD; macOS takes plain
// SOCK_STREAM/SOCK_DGRAM and gets the flags via fcntl.
Result<int> socket_nb(int family, int type) {
#ifdef SOCK_NONBLOCK
    int fd = ::socket(family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#else
    int fd = ::socket(family, type, 0);
#endif
    if (fd < 0) return last_errno();
#ifndef SOCK_NONBLOCK
    if (auto r = set_nonblocking(fd); !r) {
        ::close(fd);
        return r.error();
    }
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
#ifdef SO_NOSIGPIPE
    // MSG_NOSIGNAL doesn't exist here — SO_NOSIGPIPE is the equivalent.
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return fd;
}

}  // namespace

Result<void> set_nonblocking(int fd) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl < 0) return last_errno();
    if (::fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return last_errno();
    return {};
}

Result<int> create(int family, const SocketOptions& opts) {
    auto fd = socket_nb(family, SOCK_STREAM);
    if (!fd) return fd;
    if (auto r = apply(*fd, opts); !r) {
        ::close(*fd);
        return r.error();
    }
    return fd;
}

Result<void> apply(int fd, const SocketOptions& o) {
    auto set = [&](int lvl, int name, int v) -> Result<void> {
        if (::setsockopt(fd, lvl, name, &v, sizeof(v)) < 0) return last_errno();
        return {};
    };
    // TCP-specific options are meaningless (or fail EOPNOTSUPP) on Unix
    // sockets — the family is fixed at socket() and readable via
    // getsockname even before bind.
    sockaddr_storage ss{};
    socklen_t sl = sizeof(ss);
    int fam = AF_INET;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &sl) == 0)
        fam = ss.ss_family;
    bool inet = fam == AF_INET || fam == AF_INET6;
    if (o.nonblock)
        if (auto r = set_nonblocking(fd); !r) return r;
    if (inet && o.nodelay)
        if (auto r = set(IPPROTO_TCP, TCP_NODELAY, 1); !r) return r;
#ifdef TCP_QUICKACK  // Linux-only
    if (inet && o.quickack)
        if (auto r = set(IPPROTO_TCP, TCP_QUICKACK, 1); !r) return r;
#endif
    if (o.rcvbuf)
        if (auto r = set(SOL_SOCKET, SO_RCVBUF, o.rcvbuf); !r) return r;
    if (o.sndbuf)
        if (auto r = set(SOL_SOCKET, SO_SNDBUF, o.sndbuf); !r) return r;
    if (inet && o.keepalive) {
        if (auto r = set(SOL_SOCKET, SO_KEEPALIVE, 1); !r) return r;
        if (o.keepalive_idle.count()) {
#ifdef TCP_KEEPIDLE  // Linux; macOS spells it TCP_KEEPALIVE
            int secs = int(o.keepalive_idle.count() / 1'000'000'000);
            if (auto r = set(IPPROTO_TCP, TCP_KEEPIDLE, secs); !r) return r;
#elif defined(TCP_KEEPALIVE)
            int secs = int(o.keepalive_idle.count() / 1'000'000'000);
            if (auto r = set(IPPROTO_TCP, TCP_KEEPALIVE, secs); !r) return r;
#endif
        }
    }
#ifdef AFX_WITH_TIMESTAMPING
    if (o.timestamping) {
        int flags = SOF_TIMESTAMPING_RX_HARDWARE |
                    SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
        if (auto r = set(SOL_SOCKET, SO_TIMESTAMPING, flags); !r) return r;
    }
#endif
    return {};
}

Result<void> bind(int fd, const SockAddr& a, bool reuse_addr, bool reuse_port) {
    int one = 1;
    if (reuse_addr)
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (reuse_port)
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    if (::bind(fd, a.addr(), a.len()) < 0) return last_errno();
    return {};
}

Result<void> listen(int fd, int backlog) {
    if (::listen(fd, backlog) < 0) return last_errno();
    return {};
}

Result<SockAddr> local_addr(int fd) {
    SockAddr a;
    socklen_t l = sizeof(a);
    auto& ss = *a.addr();
    if (::getsockname(fd, &ss, &l) < 0) return last_errno();
    return a;
}

Result<SockAddr> peer_addr(int fd) {
    SockAddr a;
    socklen_t l = sizeof(a);
    auto& ss = *a.addr();
    if (::getpeername(fd, &ss, &l) < 0) return last_errno();
    return a;
}

Result<std::size_t> send_fds(int fd, ByteSpan payload,
                             std::span<const int> fds) {
    if (payload.empty() || fds.empty())
        return make_error(ErrorCategory::Net, Err::Invalid);
    msghdr msg{};
    iovec iov{const_cast<std::byte*>(payload.data()), payload.size()};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char cbuf[CMSG_SPACE(8 * sizeof(int))];
    if (fds.size() > 8) return make_error(ErrorCategory::Net, Err::Invalid);
    msg.msg_control = cbuf;
    msg.msg_controllen = CMSG_SPACE(fds.size() * sizeof(int));
    auto* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(fds.size() * sizeof(int));
    std::memcpy(CMSG_DATA(c), fds.data(), fds.size() * sizeof(int));
    ssize_t r;
    do {
        r = ::sendmsg(fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (r < 0 && errno == EINTR);
    if (r < 0) return last_errno();
    return std::size_t(r);
}

}  // namespace afx::sock

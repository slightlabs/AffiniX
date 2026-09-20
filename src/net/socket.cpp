#include "afx/net/socket.hpp"

#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace afx::sock {

Result<void> set_nonblocking(int fd) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl < 0) return last_errno();
    if (::fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return last_errno();
    return {};
}

Result<int> create(int family, const SocketOptions& opts) {
    int fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return last_errno();
    if (auto r = apply(fd, opts); !r) {
        ::close(fd);
        return r.error();
    }
    return fd;
}

Result<void> apply(int fd, const SocketOptions& o) {
    auto set = [&](int lvl, int name, int v) -> Result<void> {
        if (::setsockopt(fd, lvl, name, &v, sizeof(v)) < 0) return last_errno();
        return {};
    };
    if (o.nonblock)
        if (auto r = set_nonblocking(fd); !r) return r;
    if (o.nodelay)
        if (auto r = set(IPPROTO_TCP, TCP_NODELAY, 1); !r) return r;
    if (o.quickack)
        if (auto r = set(IPPROTO_TCP, TCP_QUICKACK, 1); !r) return r;
    if (o.rcvbuf)
        if (auto r = set(SOL_SOCKET, SO_RCVBUF, o.rcvbuf); !r) return r;
    if (o.sndbuf)
        if (auto r = set(SOL_SOCKET, SO_SNDBUF, o.sndbuf); !r) return r;
    if (o.keepalive) {
        if (auto r = set(SOL_SOCKET, SO_KEEPALIVE, 1); !r) return r;
        if (o.keepalive_idle.count())
            if (auto r = set(IPPROTO_TCP, TCP_KEEPIDLE,
                             int(o.keepalive_idle.count() / 1'000'000'000));
                !r)
                return r;
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

}  // namespace afx::sock

// Datagram socket syscalls — M11-01/02. recvmmsg/sendmmsg are GNU
// extensions, so this file opts into _GNU_SOURCE and provides a portable
// recvmsg/sendmsg fallback for the kqueue/macOS build.

#ifdef __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include "afx/net/udp_socket.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace afx::sock {

Result<int> create_datagram(int family, const SocketOptions& o) {
#ifdef SOCK_NONBLOCK
    int fd = ::socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#else
    // macOS: socket() takes no flag bits — fcntl applies them after.
    int fd = ::socket(family, SOCK_DGRAM, 0);
#endif
    if (fd < 0) return last_errno();
#ifndef SOCK_NONBLOCK
    {
        int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
#endif
    auto fail = [&](Error e) -> Result<int> {
        ::close(fd);
        return e;
    };
    auto set = [&](int lvl, int name, int v) -> Result<void> {
        if (::setsockopt(fd, lvl, name, &v, sizeof(v)) < 0) return last_errno();
        return {};
    };
    // Only the datagram-safe options — TCP knobs would fail with EOPNOTSUPP.
    if (o.rcvbuf)
        if (auto r = set(SOL_SOCKET, SO_RCVBUF, o.rcvbuf); !r)
            return fail(r.error());
    if (o.sndbuf)
        if (auto r = set(SOL_SOCKET, SO_SNDBUF, o.sndbuf); !r)
            return fail(r.error());
#ifdef AFX_WITH_TIMESTAMPING
    if (o.timestamping) {
        int flags = SOF_TIMESTAMPING_RX_HARDWARE |
                    SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
        if (auto r = set(SOL_SOCKET, SO_TIMESTAMPING, flags); !r)
            return fail(r.error());
    }
#endif
    return fd;
}

// ---- batched datagram I/O
// ----------------------------------------------------

Result<int> recv_datagrams(int fd, std::span<UdpRx> out) {
    if (out.empty()) return 0;
#ifdef __linux__
    // One recvmmsg for the whole batch.
    std::vector<mmsghdr> hdrs(out.size());
    std::vector<iovec> iov(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        iov[i].iov_base = out[i].buf.data();
        iov[i].iov_len = out[i].buf.size();
        auto& h = hdrs[i].msg_hdr;
        h.msg_name = out[i].from.addr();
        h.msg_namelen = socklen_t(sizeof(sockaddr_storage));
        h.msg_iov = &iov[i];
        h.msg_iovlen = 1;
    }
    int n = ::recvmmsg(fd, hdrs.data(), unsigned(out.size()), MSG_DONTWAIT,
                       nullptr);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return last_errno();
    }
    for (int i = 0; i < n; ++i) out[i].len = int(hdrs[i].msg_len);
    return n;
#else
    // Portable fallback: recvmsg per datagram until the batch fills or the
    // socket reports EAGAIN.
    int n = 0;
    for (auto& slot : out) {
        iovec iov{slot.buf.data(), slot.buf.size()};
        msghdr h{};
        h.msg_name = slot.from.addr();
        h.msg_namelen = socklen_t(sizeof(sockaddr_storage));
        h.msg_iov = &iov;
        h.msg_iovlen = 1;
        ssize_t r = ::recvmsg(fd, &h, MSG_DONTWAIT);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (n) break;
            return last_errno();
        }
        slot.len = int(r);
        ++n;
    }
    return n;
#endif
}

Result<int> send_datagrams(int fd, std::span<const UdpTx> msgs) {
    if (msgs.empty()) return 0;
#ifdef __linux__
    std::vector<mmsghdr> hdrs(msgs.size());
    std::vector<iovec> iov(msgs.size());
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        iov[i].iov_base = const_cast<std::byte*>(msgs[i].bytes.data());
        iov[i].iov_len = msgs[i].bytes.size();
        auto& h = hdrs[i].msg_hdr;
        h.msg_name = const_cast<SockAddr&>(msgs[i].to).addr();
        h.msg_namelen = msgs[i].to.len();
        h.msg_iov = &iov[i];
        h.msg_iovlen = 1;
    }
    int n = ::sendmmsg(fd, hdrs.data(), unsigned(msgs.size()), MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return last_errno();
    }
    return n;
#else
    int n = 0;
    for (auto& m : msgs) {
        iovec iov{const_cast<std::byte*>(m.bytes.data()), m.bytes.size()};
        msghdr h{};
        h.msg_name = const_cast<SockAddr&>(m.to).addr();
        h.msg_namelen = m.to.len();
        h.msg_iov = &iov;
        h.msg_iovlen = 1;
        ssize_t r = ::sendmsg(fd, &h, MSG_DONTWAIT);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (n) break;
            return last_errno();
        }
        ++n;
    }
    return n;
#endif
}

// ---- multicast
// ---------------------------------------------------------------
//
// `group`/`src` carry the multicast/source address; `iface` supplies the
// interface address (in_addr for v4, interface index in port() for v6 —
// SockAddr::any(0) selects the default).

Result<void> join_group(int fd, const SockAddr& group, const SockAddr& iface) {
    if (group.family() == AF_INET) {
        ip_mreq m{};
        m.imr_multiaddr =
            reinterpret_cast<const sockaddr_in*>(group.addr())->sin_addr;
        m.imr_interface =
            iface.family() == AF_INET
                ? reinterpret_cast<const sockaddr_in*>(iface.addr())->sin_addr
                : in_addr{INADDR_ANY};
        if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) < 0)
            return last_errno();
        return {};
    }
    if (group.family() == AF_INET6) {
        ipv6_mreq m{};
        m.ipv6mr_multiaddr =
            reinterpret_cast<const sockaddr_in6*>(group.addr())->sin6_addr;
        m.ipv6mr_interface =
            iface.family() == AF_INET6
                ? reinterpret_cast<const sockaddr_in6*>(iface.addr())
                      ->sin6_scope_id
                : 0;
        if (::setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &m, sizeof(m)) < 0)
            return last_errno();
        return {};
    }
    return make_error(ErrorCategory::Net, Err::Invalid);
}

Result<void> leave_group(int fd, const SockAddr& group, const SockAddr& iface) {
    if (group.family() == AF_INET) {
        ip_mreq m{};
        m.imr_multiaddr =
            reinterpret_cast<const sockaddr_in*>(group.addr())->sin_addr;
        m.imr_interface =
            iface.family() == AF_INET
                ? reinterpret_cast<const sockaddr_in*>(iface.addr())->sin_addr
                : in_addr{INADDR_ANY};
        if (::setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &m, sizeof(m)) < 0)
            return last_errno();
        return {};
    }
    if (group.family() == AF_INET6) {
        ipv6_mreq m{};
        m.ipv6mr_multiaddr =
            reinterpret_cast<const sockaddr_in6*>(group.addr())->sin6_addr;
        m.ipv6mr_interface =
            iface.family() == AF_INET6
                ? reinterpret_cast<const sockaddr_in6*>(iface.addr())
                      ->sin6_scope_id
                : 0;
        if (::setsockopt(fd, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &m, sizeof(m)) < 0)
            return last_errno();
        return {};
    }
    return make_error(ErrorCategory::Net, Err::Invalid);
}

Result<void> join_source(int fd, const SockAddr& group, const SockAddr& src,
                         const SockAddr& iface) {
    // Source-specific multicast (RFC 3678 SFM) is IPv4-only on Linux/macOS.
    if (group.family() != AF_INET || src.family() != AF_INET)
        return make_error(ErrorCategory::Net, Err::Unsupported);
    ip_mreq_source m{};
    m.imr_multiaddr =
        reinterpret_cast<const sockaddr_in*>(group.addr())->sin_addr;
    m.imr_sourceaddr =
        reinterpret_cast<const sockaddr_in*>(src.addr())->sin_addr;
    m.imr_interface =
        iface.family() == AF_INET
            ? reinterpret_cast<const sockaddr_in*>(iface.addr())->sin_addr
            : in_addr{INADDR_ANY};
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, &m, sizeof(m)) <
        0)
        return last_errno();
    return {};
}

Result<void> leave_source(int fd, const SockAddr& group, const SockAddr& src,
                          const SockAddr& iface) {
    if (group.family() != AF_INET || src.family() != AF_INET)
        return make_error(ErrorCategory::Net, Err::Unsupported);
    ip_mreq_source m{};
    m.imr_multiaddr =
        reinterpret_cast<const sockaddr_in*>(group.addr())->sin_addr;
    m.imr_sourceaddr =
        reinterpret_cast<const sockaddr_in*>(src.addr())->sin_addr;
    m.imr_interface =
        iface.family() == AF_INET
            ? reinterpret_cast<const sockaddr_in*>(iface.addr())->sin_addr
            : in_addr{INADDR_ANY};
    if (::setsockopt(fd, IPPROTO_IP, IP_DROP_SOURCE_MEMBERSHIP, &m, sizeof(m)) <
        0)
        return last_errno();
    return {};
}

Result<void> set_multicast_ttl(int fd, int ttl) {
    // IP_MULTICAST_TTL vs IPV6_MULTICAST_HOPS: pick by the socket's family.
    sockaddr_storage ss{};
    socklen_t l = sizeof(ss);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &l) < 0)
        return last_errno();
    int v = ttl;
    if (ss.ss_family == AF_INET6) {
        if (::setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &v, sizeof(v)) <
            0)
            return last_errno();
        return {};
    }
    if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &v, sizeof(v)) < 0)
        return last_errno();
    return {};
}

Result<void> set_multicast_loop(int fd, bool on) {
    int v = on ? 1 : 0;
    if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &v, sizeof(v)) < 0)
        return last_errno();
    return {};
}

Result<void> set_multicast_if(int fd, const SockAddr& iface) {
    if (iface.family() == AF_INET) {
        in_addr a =
            reinterpret_cast<const sockaddr_in*>(iface.addr())->sin_addr;
        if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof(a)) < 0)
            return last_errno();
        return {};
    }
    if (iface.family() == AF_INET6) {
        unsigned idx =
            reinterpret_cast<const sockaddr_in6*>(iface.addr())->sin6_scope_id;
        if (::setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &idx,
                         sizeof(idx)) < 0)
            return last_errno();
        return {};
    }
    return make_error(ErrorCategory::Net, Err::Invalid);
}

}  // namespace afx::sock

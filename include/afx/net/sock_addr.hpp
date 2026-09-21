#pragma once

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstdint>
#include <string>

#include "afx/sys/result.hpp"

namespace afx {

// SockAddr — a resolved socket address (IPv4/IPv6/Unix). Parsing a numeric
// "ip:port" is synchronous and never touches DNS; hostname resolution is the
// async resolver's job (§28.2), not this type's.
class SockAddr {
  public:
    SockAddr() = default;

    static Result<SockAddr> parse(std::string_view ip, std::uint16_t port);
    static SockAddr ipv4(std::uint32_t addr_be, std::uint16_t port);
    static SockAddr any(std::uint16_t port);       // 0.0.0.0:port
    static SockAddr loopback(std::uint16_t port);  // 127.0.0.1:port
    // Unix-domain path (M11-05). Filesystem paths only — no abstract '\0'
    // namespace (a leading '@' is kept literal, not translated).
    static Result<SockAddr> unix_domain(std::string_view path);
    // Filesystem path for AF_UNIX addrs, empty view otherwise.
    std::string_view unix_path() const noexcept;

    const sockaddr* addr() const noexcept {
        return reinterpret_cast<const sockaddr*>(&ss_);
    }
    sockaddr* addr() noexcept { return reinterpret_cast<sockaddr*>(&ss_); }
    socklen_t len() const noexcept {
        return ss_.ss_family == AF_INET6  ? sizeof(sockaddr_in6)
               : ss_.ss_family == AF_INET ? sizeof(sockaddr_in)
               : ss_.ss_family == AF_UNIX ? sizeof(sockaddr_un)
                                          : socklen_t(sizeof(ss_));
    }
    int family() const noexcept { return ss_.ss_family; }
    std::uint16_t port() const noexcept;

    std::string to_string() const;

    bool operator==(const SockAddr&) const = default;

  private:
    sockaddr_storage ss_{};
};

// Endpoint is an unresolved target: a host name plus a port. `connect` resolves
// it asynchronously; SockAddr is what a resolved endpoint looks like.
struct Endpoint {
    std::string host;
    std::uint16_t port = 0;
};

struct Peer {
    SockAddr addr;
};

}  // namespace afx

#include "afx/net/sock_addr.hpp"

#include <arpa/inet.h>
#include <cstring>

namespace afx {

SockAddr SockAddr::ipv4(std::uint32_t addr_be, std::uint16_t port) {
    SockAddr a;
    auto* in = reinterpret_cast<sockaddr_in*>(&a.ss_);
    in->sin_family = AF_INET;
    in->sin_addr.s_addr = addr_be;
    in->sin_port = htons(port);
    return a;
}

SockAddr SockAddr::any(std::uint16_t port) {
    return ipv4(INADDR_ANY, port);
}

SockAddr SockAddr::loopback(std::uint16_t port) {
    return ipv4(htonl(INADDR_LOOPBACK), port);
}

Result<SockAddr> SockAddr::parse(std::string_view ip, std::uint16_t port) {
    std::string s(ip);
    sockaddr_in in4{};
    if (inet_pton(AF_INET, s.c_str(), &in4.sin_addr) == 1)
        return ipv4(in4.sin_addr.s_addr, port);

    sockaddr_in6 in6{};
    if (inet_pton(AF_INET6, s.c_str(), &in6.sin6_addr) == 1) {
        SockAddr a;
        auto* out = reinterpret_cast<sockaddr_in6*>(&a.ss_);
        out->sin6_family = AF_INET6;
        out->sin6_addr = in6.sin6_addr;
        out->sin6_port = htons(port);
        return a;
    }
    return make_error(ErrorCategory::Net, Err::Invalid);
}

std::uint16_t SockAddr::port() const noexcept {
    if (ss_.ss_family == AF_INET)
        return ntohs(reinterpret_cast<const sockaddr_in*>(&ss_)->sin_port);
    if (ss_.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&ss_)->sin6_port);
    return 0;
}

std::string SockAddr::to_string() const {
    char buf[INET6_ADDRSTRLEN]{};
    if (ss_.ss_family == AF_INET) {
        auto* in = reinterpret_cast<const sockaddr_in*>(&ss_);
        inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf));
        return std::string(buf) + ":" + std::to_string(ntohs(in->sin_port));
    }
    if (ss_.ss_family == AF_INET6) {
        auto* in = reinterpret_cast<const sockaddr_in6*>(&ss_);
        inet_ntop(AF_INET6, &in->sin6_addr, buf, sizeof(buf));
        return std::string("[") + buf +
               "]:" + std::to_string(ntohs(in->sin6_port));
    }
    return "<unknown>";
}

}  // namespace afx

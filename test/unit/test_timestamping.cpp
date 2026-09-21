#include <doctest/doctest.h>

// M8-06: SO_TIMESTAMPING → Completion::stamps plumbing. The parser test runs
// always; live-path tests need AFX_WITH_TIMESTAMPING (the socket option
// exists only under that flag) and use loopback TCP, where software stamps
// are deterministic but hardware stamps are absent — exactly the "zero when
// unavailable" contract §9.4 specifies.

#include <sys/socket.h>
#include <unistd.h>
#include <cstring>

#include "afx/backend/epoll.hpp"
#include "afx/core/event_manager.hpp"
#include "afx/net/socket.hpp"
#include "afx/sys/timestamping.hpp"

#ifdef AFX_WITH_URING
#include "afx/backend/uring.hpp"
#endif

using namespace afx;
using namespace std::chrono_literals;

#ifdef SCM_TIMESTAMPING  // Linux-only cmsg type; parser is a no-op elsewhere

TEST_CASE("timestamping: parse_rx_timestamping reads SCM_TIMESTAMPING") {
    alignas(cmsghdr) char cbuf[kTimestampingCbufSize]{};
    std::byte data[8]{};
    iovec iov{data, sizeof(data)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    auto* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_TIMESTAMPING;
    c->cmsg_len = CMSG_LEN(3 * sizeof(timespec));
    auto* ts = reinterpret_cast<timespec*>(CMSG_DATA(c));
    ts[0] = timespec{10, 500};  // software
    ts[1] = timespec{0, 0};     // legacy hw-transformed (ignored)
    ts[2] = timespec{20, 750};  // raw hardware

    Timestamps stamps{};
    std::uint32_t flags = parse_rx_timestamping(msg, stamps);
    CHECK((flags & CompletionFlag::HasSwStamp) != 0);
    CHECK((flags & CompletionFlag::HasHwStamp) != 0);
    CHECK(stamps.sw_ns == 10'000'000'500ull);
    CHECK(stamps.hw_ns == 20'000'000'750ull);
}

#endif  // SCM_TIMESTAMPING

TEST_CASE("timestamping: empty control buffer yields no flags") {
    std::byte data[8]{};
    iovec iov{data, sizeof(data)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    Timestamps stamps{};
    CHECK(parse_rx_timestamping(msg, stamps) == 0);
    CHECK(stamps.sw_ns == 0);
    CHECK(stamps.hw_ns == 0);
}

#ifdef AFX_WITH_TIMESTAMPING

namespace {

// Accepted-side loopback pair: returns {server-side fd, client fd}.
struct LoopbackPair {
    int srv = -1, cli = -1;
    ~LoopbackPair() {
        if (srv >= 0) ::close(srv);
        if (cli >= 0) ::close(cli);
        if (lfd >= 0) ::close(lfd);
    }
    int lfd = -1;
};

LoopbackPair make_tcp_pair(bool ts_on) {
    LoopbackPair p;
    SocketOptions o{};
    o.timestamping = ts_on;
    auto ls = sock::create(AF_INET, SocketOptions{});
    if (!ls) return p;
    p.lfd = *ls;
    if (!sock::bind(p.lfd, SockAddr::loopback(0), true, false) ||
        !sock::listen(p.lfd, 4))
        return p;
    auto bound = sock::local_addr(p.lfd);
    if (!bound) return p;

    auto cs = sock::create(AF_INET, SocketOptions{});
    if (!cs) return p;
    p.cli = *cs;
    if (::connect(p.cli, bound->addr(), bound->len()) < 0 &&
        errno != EINPROGRESS)
        return p;
    p.srv = ::accept4(p.lfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (p.srv >= 0) (void)sock::apply(p.srv, o);
    return p;
}

}  // namespace

TEST_CASE("timestamping: epoll recv delivers software stamps") {
    LoopbackPair p = make_tcp_pair(true);
    REQUIRE(p.srv >= 0);

    EpollBackend b;
    b.set_timestamping(p.srv, true);
    std::byte buf[64]{};
    UserData u = UserData::make(std::uint8_t(OpKind::Recv), 1, 1);
    REQUIRE(b.submit_recv(u, p.srv, MutByteSpan(buf)).has_value());

    const char m[] = "stamp-me";
    REQUIRE(::send(p.cli, m, sizeof(m), MSG_NOSIGNAL) == ssize_t(sizeof(m)));

    Completion out[8];
    Completion got{};
    for (int i = 0; i < 100 && !got.user.raw; ++i) {
        int n = b.wait(out, 50ms);
        for (int j = 0; j < n; ++j)
            if (out[j].user.raw == u.raw) got = out[j];
    }
    REQUIRE(got.user.raw == u.raw);
    CHECK(got.result == ssize_t(sizeof(m)));
    // Loopback has no NIC: software stamp yes, hardware stamp zero.
    CHECK((got.flags & CompletionFlag::HasSwStamp) != 0);
    CHECK(got.stamps.sw_ns != 0);
    CHECK(got.stamps.hw_ns == 0);
    CHECK(got.stamps.tsc != 0);
}

TEST_CASE("timestamping: wire histograms fill on stamped recv") {
    using EM = BasicEventManager<SteadyClock, EpollBackend>;
    EM em(EventManagerConfig{.wait = WaitStrategy::Block}, SteadyClock{},
          EpollBackend{});
    LoopbackPair p = make_tcp_pair(true);
    REQUIRE(p.srv >= 0);
    em.backend_set_timestamping(p.srv, true);

    bool got = false;
    auto h = em.register_sink(&got, [](void* g, OpKind, const Completion& c) {
        *static_cast<bool*>(g) = c.result > 0;
    });
    std::byte buf[64]{};
    REQUIRE(em.submit_recv(h, p.srv, MutByteSpan(buf)).has_value());
    const char m[] = "w";
    REQUIRE(::send(p.cli, m, sizeof(m), MSG_NOSIGNAL) == ssize_t(sizeof(m)));

    for (int i = 0; i < 400 && !got; ++i) em.poll_once();
    CHECK(got);
    // dequeue→handler is TSC-derived (always measurable); kernel→dequeue and
    // recv→handler need the software stamp.
    CHECK(em.latency().dequeue_to_handler_ns.count() > 0);
    CHECK(em.latency().kernel_to_dequeue_ns.count() > 0);
    CHECK(em.latency().recv_to_handler_ns.count() > 0);
    em.release_sink(h);
}

TEST_CASE("timestamping: epoll recv without the option stays stamp-free") {
    LoopbackPair p = make_tcp_pair(false);
    REQUIRE(p.srv >= 0);

    EpollBackend b;  // no set_timestamping call
    std::byte buf[64]{};
    UserData u = UserData::make(std::uint8_t(OpKind::Recv), 1, 1);
    REQUIRE(b.submit_recv(u, p.srv, MutByteSpan(buf)).has_value());
    const char m[] = "plain";
    REQUIRE(::send(p.cli, m, sizeof(m), MSG_NOSIGNAL) == ssize_t(sizeof(m)));

    Completion out[8];
    Completion got{};
    for (int i = 0; i < 100 && !got.user.raw; ++i) {
        int n = b.wait(out, 50ms);
        for (int j = 0; j < n; ++j)
            if (out[j].user.raw == u.raw) got = out[j];
    }
    REQUIRE(got.user.raw == u.raw);
    CHECK(got.flags == 0);
    CHECK(got.stamps.sw_ns == 0);
    CHECK(got.stamps.tsc != 0);
}

#ifdef AFX_WITH_URING
TEST_CASE("timestamping: uring recvmsg delivers software stamps") {
    auto res = UringBackend::create();
    if (!res) {
        MESSAGE("io_uring unavailable — skipping");
        return;
    }
    auto& b = *res;
    LoopbackPair p = make_tcp_pair(true);
    REQUIRE(p.srv >= 0);

    b.set_timestamping(p.srv, true);
    std::byte buf[64]{};
    UserData u = UserData::make(std::uint8_t(OpKind::Recv), 1, 1);
    REQUIRE(b.submit_recv(u, p.srv, MutByteSpan(buf)).has_value());
    const char m[] = "stamp-me";
    REQUIRE(::send(p.cli, m, sizeof(m), MSG_NOSIGNAL) == ssize_t(sizeof(m)));

    Completion out[8];
    Completion got{};
    for (int i = 0; i < 100 && !got.user.raw; ++i) {
        int n = b.wait(out, 50ms);
        for (int j = 0; j < n; ++j)
            if (out[j].user.raw == u.raw) got = out[j];
    }
    REQUIRE(got.user.raw == u.raw);
    CHECK(got.result == ssize_t(sizeof(m)));
    CHECK((got.flags & CompletionFlag::HasSwStamp) != 0);
    CHECK(got.stamps.sw_ns != 0);
}
#endif  // AFX_WITH_URING

#endif  // AFX_WITH_TIMESTAMPING

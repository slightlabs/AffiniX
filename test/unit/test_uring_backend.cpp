#include <doctest/doctest.h>

// io_uring backend tests (M8). Compiled only when AFX_WITH_URING is on; each
// case additionally skips at runtime when ring setup is denied (containers,
// seccomp, io_uring_disabled sysctl), because "backend unavailable" is a
// supported configuration, not a failure.

#ifdef AFX_WITH_URING

#include <linux/io_uring.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <thread>
#include <unordered_map>

#include "afx/backend/uring.hpp"
#include "afx/core/event_manager.hpp"
#include "afx/net/sock_addr.hpp"
#include "afx/net/socket.hpp"

using namespace afx;
using namespace std::chrono_literals;

static_assert(IoBackend<UringBackend>);
static_assert(IoBackend<AutoBackend>);

namespace {

UserData tag(OpKind k, std::uint32_t slot = 1) {
    return UserData::make(std::uint8_t(k), slot, 1);
}

// Waits (bounded) for a completion carrying `want`; returns a zeroed
// Completion on timeout. Non-matching completions are retained in `stash` so
// nothing is lost when several ops are in flight.
using CompStash = std::unordered_map<std::uint64_t, Completion>;

Completion find_completion(UringBackend& b, CompStash& stash,
                           std::uint64_t want, int max_waits = 60) {
    if (auto it = stash.find(want); it != stash.end()) {
        Completion c = it->second;
        stash.erase(it);
        return c;
    }
    Completion out[32];
    for (int i = 0; i < max_waits; ++i) {
        int n = b.wait(out, 50ms);
        for (int j = 0; j < n; ++j) {
            if (out[j].user.raw == want) return out[j];
            stash[out[j].user.raw] = out[j];
        }
    }
    return {};
}

struct SocketPair {
    int a = -1, b = -1;
    SocketPair() {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                         fds) == 0) {
            a = fds[0];
            b = fds[1];
        }
    }
    ~SocketPair() {
        if (a >= 0) ::close(a);
        if (b >= 0) ::close(b);
    }
};

#define AFX_URING_OR_SKIP(be)                                    \
    auto be##_res = UringBackend::create();                      \
    if (!be##_res) {                                             \
        MESSAGE("io_uring unavailable (", be##_res.error().code, \
                ") — skipping");                                 \
        return;                                                  \
    }                                                            \
    auto& be = *be##_res

}  // namespace

TEST_CASE("uring backend: ring setup and feature probe") {
    auto caps = UringBackend::probe();
    if (!caps) {
        MESSAGE("io_uring probe failed — skipping");
        return;
    }
    CHECK(caps->supports(IORING_OP_POLL_ADD));
    CHECK(caps->supports(IORING_OP_RECV));
    CHECK(caps->supports(IORING_OP_SEND));
    CHECK(caps->supports(IORING_OP_ACCEPT));
    CHECK(caps->supports(IORING_OP_CONNECT));
    CHECK(caps->supports(IORING_OP_ASYNC_CANCEL));

    AFX_URING_OR_SKIP(b);
    CHECK(b.valid());
    CHECK(b.wake_fd() >= 0);
}

TEST_CASE("uring backend: recv and send over socketpair") {
    AFX_URING_OR_SKIP(b);
    SocketPair sp;
    REQUIRE(sp.a >= 0);

    std::byte buf[16]{};
    REQUIRE(
        b.submit_recv(tag(OpKind::Recv), sp.a, MutByteSpan(buf)).has_value());

    const char msg[] = "hello";
    REQUIRE(::write(sp.b, msg, sizeof(msg)) == ssize_t(sizeof(msg)));

    CompStash stash;
    Completion c = find_completion(b, stash, tag(OpKind::Recv).raw);
    REQUIRE(c.user.raw == tag(OpKind::Recv).raw);
    CHECK(c.result == ssize_t(sizeof(msg)));
    CHECK(std::memcmp(buf, msg, sizeof(msg)) == 0);

    // And the reverse direction: kernel performs the send.
    const char out_msg[] = "world!";
    ByteSpan out{reinterpret_cast<const std::byte*>(out_msg), sizeof(out_msg)};
    REQUIRE(b.submit_send(tag(OpKind::Send, 2), sp.a, out).has_value());
    Completion s = find_completion(b, stash, tag(OpKind::Send, 2).raw);
    REQUIRE(s.user.raw == tag(OpKind::Send, 2).raw);
    CHECK(s.result == ssize_t(sizeof(out_msg)));

    char got[16]{};
    REQUIRE(::read(sp.b, got, sizeof(got)) == ssize_t(sizeof(out_msg)));
    CHECK(std::memcmp(got, out_msg, sizeof(out_msg)) == 0);
}

TEST_CASE("uring backend: sendv gathers into one completion") {
    AFX_URING_OR_SKIP(b);
    SocketPair sp;
    REQUIRE(sp.a >= 0);

    const char m1[] = "abc";
    const char m2[] = "defg";
    ByteSpan iov[2] = {
        {reinterpret_cast<const std::byte*>(m1), sizeof(m1)},
        {reinterpret_cast<const std::byte*>(m2), sizeof(m2)},
    };
    REQUIRE(b.submit_sendv(tag(OpKind::Send), sp.a, iov).has_value());

    CompStash stash;
    Completion c = find_completion(b, stash, tag(OpKind::Send).raw);
    REQUIRE(c.user.raw == tag(OpKind::Send).raw);
    CHECK(c.result == ssize_t(sizeof(m1) + sizeof(m2)));

    char got[16]{};
    REQUIRE(::read(sp.b, got, sizeof(got)) == ssize_t(sizeof(m1) + sizeof(m2)));
    CHECK(std::memcmp(got, m1, sizeof(m1) - 1) == 0);
    CHECK(std::memcmp(got + sizeof(m1), m2, sizeof(m2)) == 0);
}

TEST_CASE("uring backend: watch reports readiness, detach stops it") {
    AFX_URING_OR_SKIP(b);
    SocketPair sp;
    REQUIRE(sp.a >= 0);

    UserData w = tag(OpKind::Watch);
    REQUIRE(b.attach(sp.a, Interest::Readable, w).has_value());

    const char msg[] = "x";
    REQUIRE(::write(sp.b, msg, 1) == 1);
    CompStash stash;
    Completion c = find_completion(b, stash, w.raw);
    REQUIRE(c.user.raw == w.raw);
    CHECK((c.result & CompletionFlag::ReadyRead) != 0);

    REQUIRE(b.detach(sp.a).has_value());
}

TEST_CASE("uring backend: watch modify keeps the same tag working") {
    AFX_URING_OR_SKIP(b);
    SocketPair sp;
    REQUIRE(sp.a >= 0);

    UserData w = tag(OpKind::Watch);
    REQUIRE(b.attach(sp.a, Interest::Readable, w).has_value());
    REQUIRE(b.modify(sp.a, Interest::ReadWrite, w).has_value());

    const char msg[] = "y";
    REQUIRE(::write(sp.b, msg, 1) == 1);
    CompStash stash;
    Completion c = find_completion(b, stash, w.raw);
    REQUIRE(c.user.raw == w.raw);
    CHECK((c.result & CompletionFlag::ReadyRead) != 0);
    REQUIRE(b.detach(sp.a).has_value());
}

TEST_CASE("uring backend: accept and connect on loopback") {
    AFX_URING_OR_SKIP(b);

    auto lsock = sock::create(AF_INET, SocketOptions{});
    REQUIRE(lsock);
    int lfd = *lsock;
    REQUIRE(sock::bind(lfd, SockAddr::loopback(0), true, false));
    REQUIRE(sock::listen(lfd, 16));

    sockaddr_storage ss{};
    socklen_t slen = sizeof(ss);
    REQUIRE(::getsockname(lfd, reinterpret_cast<sockaddr*>(&ss), &slen) == 0);
    SockAddr bound;
    std::memcpy(bound.addr(), &ss, slen);

    auto csock = sock::create(AF_INET, SocketOptions{});
    REQUIRE(csock);

    REQUIRE(b.submit_accept(tag(OpKind::Accept), lfd).has_value());
    REQUIRE(b.submit_connect(tag(OpKind::Connect), *csock, bound).has_value());

    CompStash stash;
    Completion conn = find_completion(b, stash, tag(OpKind::Connect).raw);
    Completion acc = find_completion(b, stash, tag(OpKind::Accept).raw);
    REQUIRE(conn.user.raw == tag(OpKind::Connect).raw);
    CHECK(conn.result == 0);
    REQUIRE(acc.user.raw == tag(OpKind::Accept).raw);
    CHECK(acc.result > 0);

    if (acc.result > 0) ::close(acc.result);
    ::close(*csock);
    ::close(lfd);
}

TEST_CASE("uring backend: cancel delivers -ECANCELED") {
    AFX_URING_OR_SKIP(b);
    SocketPair sp;
    REQUIRE(sp.a >= 0);

    std::byte buf[16]{};
    UserData r = tag(OpKind::Recv);
    REQUIRE(b.submit_recv(r, sp.a, MutByteSpan(buf)).has_value());
    REQUIRE(b.cancel(r).has_value());

    CompStash stash;
    Completion c = find_completion(b, stash, r.raw);
    REQUIRE(c.user.raw == r.raw);
    CHECK(c.result == -ECANCELED);
}

TEST_CASE("uring backend: provided-buffer ring recv") {
    UringConfig cfg;
    cfg.use_pbuf_ring = true;
    auto res = UringBackend::create(cfg);
    if (!res) {
        MESSAGE("pbuf-ring setup unavailable — skipping");
        return;
    }
    auto& b = *res;
    SocketPair sp;
    REQUIRE(sp.a >= 0);

    std::byte buf[16]{};
    REQUIRE(
        b.submit_recv(tag(OpKind::Recv), sp.a, MutByteSpan(buf)).has_value());

    const char msg[] = "ringbuf";
    REQUIRE(::write(sp.b, msg, sizeof(msg)) == ssize_t(sizeof(msg)));

    CompStash stash;
    Completion c = find_completion(b, stash, tag(OpKind::Recv).raw);
    REQUIRE(c.user.raw == tag(OpKind::Recv).raw);
    CHECK(c.result == ssize_t(sizeof(msg)));
    CHECK(std::memcmp(buf, msg, sizeof(msg)) == 0);
}

TEST_CASE("uring backend: idle wait returns at deadline; wake interrupts") {
    AFX_URING_OR_SKIP(b);

    Completion out[8];
    auto t0 = std::chrono::steady_clock::now();
    int n = b.wait(out, 40ms);
    auto dt = std::chrono::steady_clock::now() - t0;
    CHECK(n == 0);
    CHECK(dt < 5s);

    // wake() from another thread must interrupt a blocked wait.
    std::thread waker([&b] {
        std::this_thread::sleep_for(20ms);
        b.wake();
    });
    t0 = std::chrono::steady_clock::now();
    n = b.wait(out, 10s);
    dt = std::chrono::steady_clock::now() - t0;
    waker.join();
    CHECK(dt < 5s);  // returned early, not at the 10s deadline
}

TEST_CASE("uring backend: drives a BasicEventManager end to end") {
    AFX_URING_OR_SKIP(b);
    using EM = BasicEventManager<SteadyClock, UringBackend>;
    EM em(EventManagerConfig{.wait = WaitStrategy::Spin}, SteadyClock{},
          std::move(b));

    SocketPair sp;
    REQUIRE(sp.a >= 0);
    bool ready = false;
    auto id = em.watch(sp.a, Interest::Readable, [&](IoId, int mask) {
        ready = (mask & CompletionFlag::ReadyRead) != 0;
    });
    REQUIRE(id);

    const char msg[] = "w";
    REQUIRE(::write(sp.b, msg, 1) == 1);
    // A spin poll may run before the CQE lands; pump a few times.
    for (int i = 0; i < 100 && !ready; ++i) em.poll_once();
    CHECK(ready);
    em.unwatch(*id);
}

TEST_CASE("auto backend: picks a working backend and logs its kind") {
    AutoBackend a;
    MESSAGE("AutoBackend selected kind = ", int(a.kind()));
    CHECK((a.kind() == BackendKind::Uring || a.kind() == BackendKind::Epoll));

    // Whichever was picked must actually work end to end.
    SocketPair sp;
    REQUIRE(sp.a >= 0);
    std::byte buf[8]{};
    UserData r = tag(OpKind::Recv);
    REQUIRE(a.submit_recv(r, sp.a, MutByteSpan(buf)).has_value());
    const char msg[] = "z";
    REQUIRE(::write(sp.b, msg, 1) == 1);

    Completion out[8];
    bool got = false;
    for (int i = 0; i < 60 && !got; ++i) {
        int n = a.wait(out, 50ms);
        for (int j = 0; j < n; ++j)
            if (out[j].user.raw == r.raw) {
                got = true;
                CHECK(out[j].result == 1);
            }
    }
    CHECK(got);
}

#endif  // AFX_WITH_URING

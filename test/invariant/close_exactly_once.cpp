// Invariant test (IMPLEMENTATION_PLAN.md M6 exit criterion): on_close fires
// exactly once on every termination path — peer FIN, local close, framing
// error, peer reset, idle timeout, EM shutdown.

#include <doctest/doctest.h>

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <memory>
#include <vector>

#include "../proto.hpp"
#include "../test_env.hpp"
#include "afx/net/connection.hpp"
#include "afx/net/tcp_server.hpp"

using namespace afx;
using afx::test::echo_frame;
using afx::test::EchoMsg;
using afx::test::EchoProto;
using afx::test::TestEnv;

namespace {

using EM = afx::test::TestEnv::EM;
using Conn = Connection<EchoProto, EM>;

struct CloseLog {
    std::vector<CloseReason> reasons;
    int owner_notifies = 0;
};

// Handlers must outlive the Connection that references them; keep per-case
// blocks in a registry cleaned at the end of each case.
std::vector<std::unique_ptr<Handlers<EchoProto>>>& handler_registry() {
    static std::vector<std::unique_ptr<Handlers<EchoProto>>> v;
    return v;
}

std::unique_ptr<Conn> make_conn(TestEnv& env, int cfd, CloseLog& log,
                                Conn::Params params = {}) {
    auto h = std::make_unique<Handlers<EchoProto>>();
    h->on_close = [&](ConnId, CloseReason r) { log.reasons.push_back(r); };
    Handlers<EchoProto>* hp = h.get();
    handler_registry().push_back(std::move(h));
    auto conn = std::make_unique<Conn>(
        env.em, cfd, *hp, params, &log, [](void* p, ConnId, CloseReason) {
            static_cast<CloseLog*>(p)->owner_notifies++;
        });
    conn->start(Peer{SockAddr::loopback(4321)});
    return conn;
}

// After the first close, extra activity must not re-trigger on_close.
void assert_quiet(TestEnv& env, int cfd, CloseLog& log, CloseReason expected) {
    REQUIRE(log.reasons.size() == 1);
    CHECK(log.reasons[0] == expected);
    auto f = echo_frame("after-close");
    env.sim().feed(cfd, ByteSpan(f.data(), f.size()));
    env.pump(4);
    CHECK(log.reasons.size() == 1);
    CHECK(log.owner_notifies == 1);
}

}  // namespace

TEST_CASE("invariant: on_close exactly once — peer FIN") {
    TestEnv env;
    CloseLog log;
    int cfd = env.sim().add_fd();
    auto conn = make_conn(env, cfd, log);

    auto f = echo_frame("hello");
    env.sim().feed(cfd, ByteSpan(f.data(), f.size()));
    env.pump(2);
    CHECK(log.reasons.empty());

    env.sim().close_peer(cfd);
    env.pump(2);
    assert_quiet(env, cfd, log, CloseReason::PeerFin);
    handler_registry().clear();
}

TEST_CASE("invariant: on_close exactly once — local close") {
    TestEnv env;
    CloseLog log;
    int cfd = env.sim().add_fd();
    auto conn = make_conn(env, cfd, log);

    conn->close(CloseReason::LocalClose);
    conn->close(CloseReason::LocalClose);  // idempotent
    env.pump(2);
    assert_quiet(env, cfd, log, CloseReason::LocalClose);
    handler_registry().clear();
}

TEST_CASE("invariant: on_close exactly once — framing error") {
    TestEnv env;
    CloseLog log;
    int cfd = env.sim().add_fd();
    auto conn = make_conn(env, cfd, log);

    auto bad = echo_frame("x");
    bad[0] = std::byte(0x00);  // break the magic byte
    env.sim().feed(cfd, ByteSpan(bad.data(), bad.size()));
    env.pump(2);
    assert_quiet(env, cfd, log, CloseReason::FrameError);
    handler_registry().clear();
}

TEST_CASE("invariant: on_close exactly once — peer reset") {
    TestEnv env;
    CloseLog log;
    int cfd = env.sim().add_fd();
    auto conn = make_conn(env, cfd, log);

    env.sim().fail_peer(cfd, ECONNRESET);
    env.pump(2);
    assert_quiet(env, cfd, log, CloseReason::Error);
    handler_registry().clear();
}

TEST_CASE("invariant: on_close exactly once — idle timeout") {
    TestEnv env;
    CloseLog log;
    int cfd = env.sim().add_fd();
    Conn::Params p;
    p.idle_read_timeout = 10ms;
    auto conn = make_conn(env, cfd, log, p);

    env.advance(10ms);
    env.pump(2);
    assert_quiet(env, cfd, log, CloseReason::IdleTimeout);
    handler_registry().clear();
}

TEST_CASE("invariant: on_close exactly once — EM shutdown closes conns") {
    CloseLog log;
    {
        TestEnv env;
        Handlers<EchoProto> h;
        h.on_close = [&](ConnId, CloseReason r) { log.reasons.push_back(r); };
        ServerConfig cfg;
        cfg.bind = SockAddr::loopback(0);  // real socket, sim-driven accept
        cfg.sock.nodelay = false;          // TCP_NODELAY won't apply to AF_UNIX
        auto srv = env.em.make_server<EchoProto>(cfg, std::move(h));
        REQUIRE(srv.has_value());

        // The accepted fd must be a real socket — apply() and teardown run
        // real syscalls on it — so hand the sim a socketpair end.
        int pair[2];
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) ==
                0);
        env.sim().deliver_accept((*srv)->listen_fd(), pair[0]);
        env.pump(2);
        CHECK((*srv)->connection_count() == 1);
        CHECK(log.reasons.empty());
        ::close(pair[1]);
    }  // ~EM -> ~TcpServer -> close(Shutdown) on the live connection
    REQUIRE(log.reasons.size() == 1);
    CHECK(log.reasons[0] == CloseReason::Shutdown);
}

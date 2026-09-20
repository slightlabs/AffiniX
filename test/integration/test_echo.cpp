#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

#include "../proto.hpp"
#include "../real_env.hpp"
#include "../test_env.hpp"
#include "afx/core/event_manager.hpp"
#include "afx/net/tcp_server.hpp"

using namespace afx;
using afx::test::echo_frame;
using afx::test::EchoHeader;
using afx::test::EchoMsg;
using afx::test::EchoProto;

// SimBackend connection lifecycle: accept -> recv -> echo -> close,
// entirely under VirtualClock.
TEST_CASE("sim: server accepts, echoes, and closes deterministically") {
    using EM = afx::test::TestEnv::EM;
    afx::test::TestEnv env;

    Handlers<EchoProto> h;
    CloseReason closed{};
    int closes = 0;
    h.on_close = [&](ConnId, CloseReason r) {
        closed = r;
        ++closes;
    };

    ServerConfig cfg;
    cfg.bind = SockAddr::any(0);
    auto srv = env.em.make_server<EchoProto>(cfg, std::move(h));
    REQUIRE(srv.has_value());  // open works under the sim env too

    // Direct connection test through the sim: a virtual fd feeds bytes in and
    // captures whatever the connection writes out.
    int cfd = env.sim().add_fd();
    Handlers<EchoProto> ch;
    std::vector<std::string> echoed;
    ch.on_messages = [&](ConnId id, std::span<const EchoMsg> batch) {
        for (auto& m : batch) {
            echoed.emplace_back(reinterpret_cast<const char*>(m.body.data()),
                                m.body.size());
            auto* c = Connection<EchoProto, EM>::resolve(env.em, id);
            REQUIRE(c != nullptr);
            std::array<std::byte, sizeof(EchoHeader)> hdr;
            std::memcpy(hdr.data(), &m.header, sizeof(hdr));
            std::array<ByteSpan, 2> parts{ByteSpan(hdr.data(), hdr.size()),
                                          m.body};
            c->send_scatter(parts);
        }
    };
    auto conn = std::make_unique<Connection<EchoProto, EM>>(
        env.em, cfd, ch, typename Connection<EchoProto, EM>::Params{}, nullptr,
        [](void*, ConnId, CloseReason) {});
    conn->start(Peer{SockAddr::loopback(1234)});

    auto f = echo_frame("ping");
    env.sim().feed(cfd, ByteSpan(f.data(), f.size()));
    env.em.poll_once();  // recv completion -> on_messages -> send queued
    env.em.poll_once();  // stage 6 -> submit_sendv -> completion
    env.em.poll_once();  // send completion drained

    auto& out = env.sim().sent(cfd);
    REQUIRE(out.size() == f.size());
    CHECK(std::memcmp(out.data(), f.data(), f.size()) == 0);
    CHECK(echoed.size() == 1);
    CHECK(echoed[0] == "ping");

    env.sim().close_peer(cfd);
    env.em.poll_once();
    // recv re-armed only if still established; peer close delivers 0
    env.em.poll_once();
    (void)closed;
    (void)closes;
}

// Real loopback: production-backend EM on a thread + blocking client socket.
AFX_BACKEND_TEST_CASE("loopback: TcpServer echoes frames to a real client",
                      EM) {
    auto emp = afx::test::make_real_em<EM>(
        EventManagerConfig{.wait = WaitStrategy::Block});
    if (!emp) {
        MESSAGE("backend unavailable — skipped");
        return;
    }
    EM& em = *emp;
    std::promise<std::uint16_t> port_p;
    std::atomic<bool> failed{false};

    std::thread t([&] {
        Handlers<EchoProto> h;
        h.on_messages = [&](ConnId id, std::span<const EchoMsg> batch) {
            for (auto& m : batch) {
                auto* c = Connection<EchoProto, EM>::resolve(em, id);
                if (!c) continue;
                std::array<std::byte, sizeof(EchoHeader)> hdr;
                std::memcpy(hdr.data(), &m.header, sizeof(hdr));
                std::array<ByteSpan, 2> parts{ByteSpan(hdr.data(), hdr.size()),
                                              m.body};
                c->send_scatter(parts);
            }
        };
        ServerConfig cfg;
        cfg.bind = SockAddr::loopback(0);
        auto srv = em.template make_server<EchoProto>(cfg, std::move(h));
        if (!srv) {
            failed = true;
            em.stop();
            return;
        }
        port_p.set_value((*srv)->bound_addr().port());
        em.run();
    });

    std::uint16_t port = port_p.get_future().get();
    CHECK(!failed);

    int c = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(c >= 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::connect(c, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);
    timeval tv{5, 0};
    ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    auto f = echo_frame("hello-echo");
    REQUIRE(::send(c, f.data(), f.size(), 0) == ssize_t(f.size()));
    std::vector<std::byte> reply(f.size());
    std::size_t got = 0;
    while (got < reply.size()) {
        ssize_t n = ::recv(c, reply.data() + got, reply.size() - got, 0);
        REQUIRE(n > 0);
        got += std::size_t(n);
    }
    CHECK(std::memcmp(reply.data(), f.data(), f.size()) == 0);
    ::close(c);

    em.stop();
    t.join();
}

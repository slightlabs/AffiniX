#include <doctest/doctest.h>
#include <cerrno>

// M12-01..05 integration: the AdminServer runs a Block-mode EM on its own
// thread, serves the HTTP/1.1 subset through AffiniX's own custom-framing
// seam (HttpProto over make_server), and gathers cross-shard state via
// mailbox posts. Requests below go over a real loopback socket — the whole
// stack is exercised end to end.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include "../proto.hpp"
#include "afx/admin/server.hpp"
#include "afx/net/tcp_client.hpp"
#include "afx/net/tcp_server.hpp"
#include "afx/runtime.hpp"
#include "afx/sys/log.hpp"

using namespace afx;
using afx::test::echo_frame;
using afx::test::EchoHeader;
using afx::test::EchoMsg;
using afx::test::EchoProto;

namespace {

bool wait_until(std::function<bool()> pred, int ms = 5000) {
    for (int i = 0; i < ms; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

struct HttpReply {
    int status = 0;
    std::string body;
};

// One blocking request/response — the admin endpoint always closes.
HttpReply fetch(std::uint16_t port, std::string_view request) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    timeval tv{3, 0};  // a gather that never answers must fail, not hang
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    REQUIRE(::send(fd, request.data(), request.size(), 0) ==
            (ssize_t)request.size());
    // No SHUT_WR: a half-close is a PeerFin and tears the conn down before
    // async gather responses can be written. The server closes after
    // answering (Connection: close), which ends our read loop.
    std::string raw;
    char buf[8192];
    for (;;) {
        auto n = ::recv(fd, buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        raw.append(buf, std::size_t(n));
    }
    ::close(fd);

    HttpReply r;
    auto sp = raw.find(' ');
    if (sp != std::string::npos) r.status = std::atoi(raw.c_str() + sp + 1);
    auto body_at = raw.find("\r\n\r\n");
    if (body_at != std::string::npos) r.body = raw.substr(body_at + 4);
    return r;
}

HttpReply get(std::uint16_t port, std::string_view path) {
    std::string req = "GET ";
    req += path;
    req += " HTTP/1.1\r\nhost: x\r\n\r\n";
    return fetch(port, req);
}

HttpReply post(std::uint16_t port, std::string_view path) {
    std::string req = "POST ";
    req += path;
    req += " HTTP/1.1\r\nhost: x\r\ncontent-length: 0\r\n\r\n";
    return fetch(port, req);
}

}  // namespace

TEST_CASE("admin: endpoint sweep on a running runtime") {
    Runtime rt(Topology::detect());
    ThreadConfig cfg;
    cfg.placement = Placement::None;
    cfg.em.wait = WaitStrategy::Block;
    auto g = rt.spawn_group("w", 2, cfg);
    rt.start();
    g.mailboxes();

    admin::AdminServer admin(rt);
    auto bound = admin.start();
    REQUIRE(bound.has_value());
    const auto port = bound->port();
    REQUIRE(port != 0);

    SUBCASE("healthz") {
        auto r = get(port, "/healthz");
        CHECK(r.status == 200);
        CHECK(r.body == "ok\n");
    }
    SUBCASE("version") {
        auto r = get(port, "/version");
        CHECK(r.status == 200);
        CHECK(r.body.find("\"version\"") != std::string::npos);
    }
    SUBCASE("stats gathers both shards") {
        auto r = get(port, "/stats");
        CHECK(r.status == 200);
        CHECK(r.body.find("\"iterations\"") != std::string::npos);
        CHECK(r.body.find("\"name\":\"w-0\"") != std::string::npos);
        CHECK(r.body.find("\"name\":\"w-1\"") != std::string::npos);
    }
    SUBCASE("metrics exposes prometheus text") {
        auto r = get(port, "/metrics");
        CHECK(r.status == 200);
        CHECK(r.body.find("afx_iterations_total") != std::string::npos);
        CHECK(r.body.find("afx_iteration_ns") != std::string::npos);
    }
    SUBCASE("placement reports shard rows") {
        auto r = get(port, "/placement");
        CHECK(r.status == 200);
        CHECK(r.body.find("\"running\":true") != std::string::npos);
    }
    SUBCASE("config reports wait strategy") {
        auto r = get(port, "/config");
        CHECK(r.status == 200);
        CHECK(r.body.find("wait=block") != std::string::npos);
    }
    SUBCASE("flight returns the binary dump format") {
        auto r = get(port, "/flight");
        CHECK(r.status == 200);
        CHECK(r.body.size() >= 16 * 2);  // one header per shard
        CHECK(r.body.substr(0, 8) == "AFXFLT01");
    }
    SUBCASE("unknown path is 404") {
        auto r = get(port, "/nope");
        CHECK(r.status == 404);
    }
    SUBCASE("index lists the routes") {
        auto r = get(port, "/");
        CHECK(r.status == 200);
        CHECK(r.body.find("/stats") != std::string::npos);
    }
    SUBCASE("stall_threshold toggle applies to one shard") {
        auto r = post(port, "/admin/stall_threshold?ns=12345&shard=1");
        CHECK(r.status == 200);
        auto c = get(port, "/config");
        // Only shard 1 carries the new threshold.
        CHECK(c.body.find("stall_threshold_ns=12345") != std::string::npos);
    }
    SUBCASE("log_level toggle accepts and replies") {
        auto r = post(port, "/admin/log_level?level=debug");
        CHECK(r.status == 200);
        set_log_level(LogLevel::Info);  // restore for other tests
    }
    SUBCASE("malformed request closes the connection") {
        auto r = fetch(port, "GARBAGE\r\n\r\n");
        CHECK(r.status == 0);  // frame error → close, no HTTP reply
    }
    SUBCASE("pipelined request after a close is dropped") {
        // Connection: close — one request per conn; the pipelined second
        // request must not produce a second response.
        auto r = fetch(port,
                       "GET /healthz HTTP/1.1\r\nh: x\r\n\r\n"
                       "GET /healthz HTTP/1.1\r\nh: x\r\n\r\n");
        CHECK(r.status == 200);
        CHECK(r.body == "ok\n");
    }

    admin.stop();
    rt.shutdown(5s);
    rt.join();
}

TEST_CASE("admin: /conns shows live connections on the shard") {
    Runtime rt(Topology::detect());
    ThreadConfig cfg;
    cfg.placement = Placement::None;
    cfg.em.wait = WaitStrategy::Block;
    auto g = rt.spawn_group("w", 1, cfg);

    std::atomic<bool> client_up{false};
    g.each([&](EventManager& em) {
        Handlers<EchoProto> sh;
        sh.on_messages = [&em](ConnId id, std::span<const EchoMsg> ms) {
            auto* c = Connection<EchoProto, EventManager>::resolve(em, id);
            if (!c) return;
            for (auto& m : ms) {
                std::array<std::byte, sizeof(EchoHeader)> hdr;
                std::memcpy(hdr.data(), &m.header, sizeof(hdr));
                std::array<ByteSpan, 2> parts{ByteSpan(hdr.data(), hdr.size()),
                                              m.body};
                (void)c->send_scatter(parts);
            }
        };
        auto srv = em.make_server<EchoProto>(
            ServerConfig{.bind = SockAddr::loopback(0)}, std::move(sh));
        REQUIRE(srv.has_value());
        REQUIRE((*srv)->open().has_value());
        auto port = (*srv)->bound_addr().port();

        Handlers<EchoProto> ch;
        ch.on_messages = [](ConnId, std::span<const EchoMsg>) {};
        ClientConfig cc;
        cc.target = {"127.0.0.1", port};
        cc.auto_reconnect = false;
        auto cli = em.make_client<EchoProto>(cc, std::move(ch));
        REQUIRE(cli.has_value());
        (*cli)->start();
        client_up = true;
    });
    rt.start();
    g.mailboxes();
    REQUIRE(wait_until([&] { return client_up.load(); }));

    admin::AdminServer admin(rt);
    auto bound = admin.start();
    REQUIRE(bound.has_value());

    // Poll until the client connection is Established server-side, then
    // the conn table must carry both roles.
    std::string body;
    REQUIRE(wait_until([&] {
        auto r = get(bound->port(), "/conns");
        if (r.status != 200) return false;
        body = r.body;
        return body.find("\"role\":\"server\"") != std::string::npos &&
               body.find("\"role\":\"client\"") != std::string::npos;
    }));
    CHECK(body.find("\"state\":\"established\"") != std::string::npos);

    admin.stop();
    rt.shutdown(5s);
    rt.join();
}

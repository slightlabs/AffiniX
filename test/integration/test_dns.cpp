#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../proto.hpp"
#include "../real_env.hpp"
#include "afx/net/dns.hpp"
#include "afx/net/tcp_client.hpp"
#include "afx/net/tcp_server.hpp"

using namespace afx;
using afx::test::echo_frame;
using afx::test::EchoHeader;
using afx::test::EchoMsg;
using afx::test::EchoProto;

// Async resolver (M11-03) + Happy Eyeballs in TcpClient (M11-04). The
// resolver runs getaddrinfo on its own EM thread; the requester's wheel
// owns the deadline, so "slow resolver" tests need no external DNS.
namespace {

template <class F>
bool wait_for(F&& f) {
    for (int i = 0; i < 4000; ++i) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

struct ResolveOutcome {
    std::atomic<int> fired{0};
    std::atomic<int> ok{0};
    std::atomic<int> expired{0};
    std::atomic<int> failed{0};
    std::vector<SockAddr> addrs;
    std::mutex mu;

    afx::Resolver::Cb cb() {
        return [this](Result<Resolver::AddrList> r) {
            ++fired;
            if (r) {
                ++ok;
                std::lock_guard g(mu);
                addrs = *r;
            } else if (r.error().code == std::uint16_t(Err::Expired)) {
                ++expired;
            } else {
                ++failed;
            }
        };
    }
};

}  // namespace

AFX_BACKEND_TEST_CASE("dns: localhost resolves to loopback addresses", EM) {
    auto emp = afx::test::make_real_em<EM>(
        EventManagerConfig{.wait = WaitStrategy::SpinThenBlock});
    if (!emp) {
        MESSAGE("backend unavailable — skipped");
        return;
    }
    EM& em = *emp;
    Resolver resolver;
    auto out = std::make_shared<ResolveOutcome>();

    std::thread t([&] { em.run(); });
    CHECK(em.mailbox().post([&] {
        resolver.resolve("localhost", 80, em, 5s, out->cb());
    }) == PostResult::Ok);
    CHECK(wait_for([&] { return out->fired.load() == 1; }));
    CHECK(out->ok.load() == 1);
    {
        std::lock_guard g(out->mu);
        REQUIRE(!out->addrs.empty());
        bool loopback = false;
        for (auto& a : out->addrs)
            if (a.family() == AF_INET || a.family() == AF_INET6)
                loopback = true;
        CHECK(loopback);
    }
    em.stop();
    t.join();
}

AFX_BACKEND_TEST_CASE("dns: slow resolver loses to the requester deadline",
                      EM) {
    auto emp = afx::test::make_real_em<EM>(
        EventManagerConfig{.wait = WaitStrategy::SpinThenBlock});
    if (!emp) {
        MESSAGE("backend unavailable — skipped");
        return;
    }
    EM& em = *emp;
    Resolver resolver;
    resolver.set_testing_delay(400ms);  // deliberately slow (M11 exit test)
    auto out = std::make_shared<ResolveOutcome>();
    std::atomic<int> timer_fired{0};

    std::thread t([&] { em.run(); });
    CHECK(em.mailbox().post([&] {
        resolver.resolve("localhost", 80, em, 30ms, out->cb());
        // A timer firing while the resolve is outstanding proves
        // the EM was never blocked by getaddrinfo.
        em.after(50ms, [&](TimerCtx) { ++timer_fired; });
    }) == PostResult::Ok);
    // Both fire before the delayed resolve returns (~400ms): expiry at
    // 30ms, the probe timer at 50ms — proving the loop ran throughout.
    CHECK(wait_for(
        [&] { return out->expired.load() == 1 && timer_fired.load() == 1; }));
    CHECK(out->ok.load() == 0);
    em.stop();
    t.join();
}

AFX_BACKEND_TEST_CASE("dns: unresolvable name reports ResolveFailed", EM) {
    auto emp = afx::test::make_real_em<EM>(
        EventManagerConfig{.wait = WaitStrategy::SpinThenBlock});
    if (!emp) {
        MESSAGE("backend unavailable — skipped");
        return;
    }
    EM& em = *emp;
    Resolver resolver;
    auto out = std::make_shared<ResolveOutcome>();

    std::thread t([&] { em.run(); });
    CHECK(em.mailbox().post([&] {
        resolver.resolve("nonexistent.invalid.afx", 80, em, 10s, out->cb());
    }) == PostResult::Ok);
    CHECK(wait_for([&] { return out->fired.load() == 1; }));
    CHECK(out->failed.load() == 1);
    em.stop();
    t.join();
}

AFX_BACKEND_TEST_CASE("happy eyeballs: TcpClient resolves and connects", EM) {
    auto emp = afx::test::make_real_em<EM>(
        EventManagerConfig{.wait = WaitStrategy::SpinThenBlock});
    if (!emp) {
        MESSAGE("backend unavailable — skipped");
        return;
    }
    EM& em = *emp;
    Resolver resolver;
    std::atomic<int> echoes{0};
    std::atomic<int> opens{0};

    std::thread t([&] {
        Handlers<EchoProto> sh;
        sh.on_messages = [&](ConnId id, std::span<const EchoMsg> batch) {
            for (auto& m : batch) {
                auto* c = Connection<EchoProto, EM>::resolve(em, id);
                if (!c) continue;
                std::array<std::byte, sizeof(EchoHeader)> hdr;
                std::memcpy(hdr.data(), &m.header, sizeof(hdr));
                std::array<ByteSpan, 2> parts{ByteSpan(hdr.data(), hdr.size()),
                                              m.body};
                (void)c->send_scatter(parts);
            }
        };
        ServerConfig scfg;
        scfg.bind = SockAddr::loopback(0);
        auto srv = em.template make_server<EchoProto>(scfg, std::move(sh));
        REQUIRE(srv.has_value());
        auto port = (*srv)->bound_addr().port();

        Handlers<EchoProto> ch;
        ch.on_open = [&](ConnId id, Peer) {
            ++opens;
            auto* c = Connection<EchoProto, EM>::resolve(em, id);
            if (!c) return;
            auto f = echo_frame("he-hello");
            (void)c->send(ByteSpan(f.data(), f.size()));
        };
        ch.on_messages = [&](ConnId, std::span<const EchoMsg> b) {
            echoes += int(b.size());
        };
        ClientConfig cc;
        // "localhost" resolves to ::1 and 127.0.0.1; the server listens on
        // v4 loopback only — the v6 attempt refuses fast and Happy
        // Eyeballs fails over (or wins outright) on v4.
        cc.target = Endpoint{"localhost", port};
        cc.resolver = &resolver;
        cc.auto_reconnect = false;
        auto cli = em.template make_client<EchoProto>(cc, std::move(ch));
        REQUIRE(cli.has_value());
        em.run();
    });
    CHECK(wait_for([&] { return echoes.load() >= 1; }));
    CHECK(opens.load() == 1);
    em.stop();
    t.join();
}

AFX_BACKEND_TEST_CASE("happy eyeballs: async resolve failure surfaces error",
                      EM) {
    auto emp = afx::test::make_real_em<EM>(
        EventManagerConfig{.wait = WaitStrategy::SpinThenBlock});
    if (!emp) {
        MESSAGE("backend unavailable — skipped");
        return;
    }
    EM& em = *emp;
    Resolver resolver;
    std::atomic<int> errors{0};

    std::thread t([&] {
        Handlers<EchoProto> ch;
        ch.on_error = [&](ConnId, Error e) {
            if (e.code == std::uint16_t(Err::ResolveFailed)) ++errors;
        };
        ClientConfig cc;
        cc.target = Endpoint{"nonexistent.invalid.afx", 1234};
        cc.resolver = &resolver;
        cc.auto_reconnect = false;
        auto cli = em.template make_client<EchoProto>(cc, std::move(ch));
        REQUIRE(cli.has_value());
        em.run();
    });
    CHECK(wait_for([&] { return errors.load() == 1; }));
    em.stop();
    t.join();
}

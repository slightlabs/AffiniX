#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../proto.hpp"
#include "../real_env.hpp"
#include "afx/net/udp_socket.hpp"

using namespace afx;
using afx::test::echo_frame;
using afx::test::EchoHeader;
using afx::test::EchoMsg;
using afx::test::EchoProto;

// UDP sockets ride the generic watch() seam, so the same test body runs on
// every backend (M8-11 parity) — the socket does recvmmsg itself on
// readiness, exactly like a readiness backend emulates proactor.
namespace {

template <class EM>
struct UdpPair {
    EM& em;
    UdpSocket<EchoProto, EM>* a = nullptr;
    UdpSocket<EchoProto, EM>* b = nullptr;
    std::uint16_t a_port = 0, b_port = 0;
    std::atomic<int> b_msgs{0};
    std::atomic<int> b_errors{0};
    std::vector<std::string> b_bodies;
    SockAddr last_from{};

    // raw=true arms on_datagram (raw bytes); otherwise on_messages (parsed).
    // Runs on the EM thread (posted to the mailbox) — watch() is EM-affine.
    void build(bool raw = false) {
        UdpConfig ca;
        ca.bind = SockAddr::loopback(0);
        auto sa = em.template make_udp<EchoProto>(ca, UdpHandlers<EchoProto>{});
        REQUIRE(sa.has_value());
        a = *sa;
        a_port = a->bound_addr().port();

        UdpHandlers<EchoProto> hb;
        if (raw) {
            hb.on_datagram = [&](SockAddr from, ByteSpan bytes) {
                last_from = from;
                ++b_msgs;
                EchoHeader h{};
                std::memcpy(&h, bytes.data(), sizeof(h));
                b_bodies.emplace_back(
                    reinterpret_cast<const char*>(bytes.data() + sizeof(h)),
                    be16(h.len_be));
            };
        } else {
            hb.on_messages = [&](SockAddr from, std::span<const EchoMsg> ms) {
                last_from = from;
                b_msgs += int(ms.size());
                for (auto& m : ms)
                    b_bodies.emplace_back(
                        reinterpret_cast<const char*>(m.body.data()),
                        m.body.size());
            };
        }
        hb.on_error = [&](Error) { ++b_errors; };
        UdpConfig cb;
        cb.bind = SockAddr::loopback(0);
        auto sb = em.template make_udp<EchoProto>(cb, std::move(hb));
        REQUIRE(sb.has_value());
        b = *sb;
        b_port = b->bound_addr().port();
    }
};

// Wait up to ~2s for a predicate, polling at 1ms.
template <class F>
bool wait_for(F&& f) {
    for (int i = 0; i < 2000; ++i) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

}  // namespace

AFX_BACKEND_TEST_CASE("udp: send_to delivers a parsed datagram on loopback",
                      EM) {
    auto emp = afx::test::make_real_em<EM>(
        EventManagerConfig{.wait = WaitStrategy::SpinThenBlock});
    if (!emp) {
        MESSAGE("backend unavailable — skipped");
        return;
    }
    EM& em = *emp;
    UdpPair<EM> p{em};

    std::thread t([&] { em.run(); });
    CHECK(em.mailbox().post([&] { p.build(); }) == PostResult::Ok);
    CHECK(wait_for([&] { return p.b != nullptr && p.b_port != 0; }));

    std::atomic<bool> sent{false};
    CHECK(em.mailbox().post([&] {
        auto f = echo_frame("hello-udp");
        auto r = p.a->send_to(SockAddr::loopback(p.b_port),
                              ByteSpan(f.data(), f.size()));
        sent = r.has_value();
    }) == PostResult::Ok);
    CHECK(wait_for([&] { return p.b_msgs.load() == 1; }));
    CHECK(sent.load());
    REQUIRE(p.b_bodies.size() == 1);
    CHECK(p.b_bodies[0] == "hello-udp");
    // Source address is the sender's real bound port.
    CHECK(p.last_from.port() == p.a_port);
    em.stop();
    t.join();
}

AFX_BACKEND_TEST_CASE("udp: raw on_datagram path delivers whole packets", EM) {
    auto emp = afx::test::make_real_em<EM>(
        EventManagerConfig{.wait = WaitStrategy::SpinThenBlock});
    if (!emp) {
        MESSAGE("backend unavailable — skipped");
        return;
    }
    EM& em = *emp;
    UdpPair<EM> p{em};

    std::thread t([&] { em.run(); });
    CHECK(em.mailbox().post([&] { p.build(/*raw=*/true); }) == PostResult::Ok);
    CHECK(wait_for([&] { return p.b != nullptr && p.b_port != 0; }));

    CHECK(em.mailbox().post([&] {
        auto f = echo_frame("raw-bytes");
        (void)p.a->send_to(SockAddr::loopback(p.b_port),
                           ByteSpan(f.data(), f.size()));
    }) == PostResult::Ok);
    CHECK(wait_for([&] { return p.b_msgs.load() == 1; }));
    REQUIRE(p.b_bodies.size() == 1);
    CHECK(p.b_bodies[0] == "raw-bytes");
    em.stop();
    t.join();
}

AFX_BACKEND_TEST_CASE("udp: recvmmsg batch drains a burst", EM) {
    auto emp = afx::test::make_real_em<EM>(
        EventManagerConfig{.wait = WaitStrategy::SpinThenBlock});
    if (!emp) {
        MESSAGE("backend unavailable — skipped");
        return;
    }
    EM& em = *emp;
    UdpPair<EM> p{em};
    constexpr int kBurst = 64;

    std::thread t([&] { em.run(); });
    CHECK(em.mailbox().post([&] { p.build(); }) == PostResult::Ok);
    CHECK(wait_for([&] { return p.b != nullptr && p.b_port != 0; }));

    CHECK(em.mailbox().post([&] {
        std::vector<UdpTx> batch;
        std::vector<std::vector<std::byte>> frames;
        frames.reserve(kBurst);
        batch.reserve(kBurst);
        for (int i = 0; i < kBurst; ++i) {
            frames.push_back(echo_frame("m" + std::to_string(i)));
            batch.push_back(
                {SockAddr::loopback(p.b_port),
                 ByteSpan(frames.back().data(), frames.back().size())});
        }
        (void)p.a->send_batch(batch);
    }) == PostResult::Ok);
    CHECK(wait_for([&] { return p.b_msgs.load() == kBurst; }));
    CHECK(p.b_errors.load() == 0);
    em.stop();
    t.join();
}

AFX_BACKEND_TEST_CASE("udp: truncated datagram reports a Frame error", EM) {
    auto emp = afx::test::make_real_em<EM>(
        EventManagerConfig{.wait = WaitStrategy::SpinThenBlock});
    if (!emp) {
        MESSAGE("backend unavailable — skipped");
        return;
    }
    EM& em = *emp;
    UdpPair<EM> p{em};

    std::thread t([&] { em.run(); });
    CHECK(em.mailbox().post([&] { p.build(); }) == PostResult::Ok);
    CHECK(wait_for([&] { return p.b != nullptr && p.b_port != 0; }));

    // Cut a frame short: header says N body bytes, datagram carries fewer.
    CHECK(em.mailbox().post([&] {
        auto f = echo_frame("truncated-body");
        f.resize(f.size() - 3);
        (void)p.a->send_to(SockAddr::loopback(p.b_port),
                           ByteSpan(f.data(), f.size()));
    }) == PostResult::Ok);
    CHECK(wait_for([&] { return p.b_errors.load() >= 1; }));
    CHECK(p.b_msgs.load() == 0);
    em.stop();
    t.join();
}

AFX_BACKEND_TEST_CASE("udp: multicast join/leave and source filter", EM) {
    auto emp = afx::test::make_real_em<EM>(
        EventManagerConfig{.wait = WaitStrategy::SpinThenBlock});
    if (!emp) {
        MESSAGE("backend unavailable — skipped");
        return;
    }
    EM& em = *emp;
    UdpPair<EM> p{em};
    std::atomic<int> done{0};

    std::thread t([&] { em.run(); });
    CHECK(em.mailbox().post([&] {
        p.build();
        auto group = SockAddr::parse("239.7.7.7", 0);
        REQUIRE(group.has_value());
        auto src = SockAddr::parse("127.0.0.1", 0);
        REQUIRE(src.has_value());
        // Membership ops are synchronous setsockopt — the loop
        // needn't be reading for them to land. Keep any-source and
        // source-filtered joins on different groups: mixing them
        // makes the membership source-only, and a plain
        // IP_DROP_MEMBERSHIP then fails EADDRNOTAVAIL.
        CHECK(p.b->join_group(*group).has_value());
        auto group2 = SockAddr::parse("239.7.7.8", 0);
        REQUIRE(group2.has_value());
        CHECK(p.b->join_source(*group2, *src).has_value());
        CHECK(p.b->leave_source(*group2, *src).has_value());
        CHECK(p.b->set_multicast_ttl(4).has_value());
        CHECK(p.b->set_multicast_loop(true).has_value());
        CHECK(p.b->leave_group(*group).has_value());
        done = 1;
    }) == PostResult::Ok);
    CHECK(wait_for([&] { return done.load() == 1; }));
    em.stop();
    t.join();
}

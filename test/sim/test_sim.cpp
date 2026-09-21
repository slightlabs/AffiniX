// test/sim — deterministic simulation suite (M10). Every scenario runs
// fully on VirtualClock + SimBackend + SimNet: no real I/O, no sleeping,
// and every assertion is reproducible from the printed seed.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../proto.hpp"
#include "afx/net/connection.hpp"
#include "afx/net/tcp_client.hpp"
#include "afx/net/tcp_server.hpp"
#include "afx/sim/runtime.hpp"

using namespace afx;
using afx::test::echo_frame;
using afx::test::EchoHeader;
using afx::test::EchoMsg;
using afx::test::EchoProto;

namespace {

using SimEM = SimRuntime::EM;
using Conn = Connection<EchoProto, SimEM>;

// An echo handler bound to a specific shard EM.
Handlers<EchoProto> echo_handlers(SimEM& em) {
    Handlers<EchoProto> h;
    h.on_messages = [&em](ConnId id, std::span<const EchoMsg> batch) {
        for (auto& m : batch) {
            auto* c = Conn::resolve(em, id);
            if (!c) continue;
            std::array<std::byte, sizeof(EchoHeader)> hdr;
            std::memcpy(hdr.data(), &m.header, sizeof(hdr));
            std::array<ByteSpan, 2> parts{ByteSpan(hdr.data(), hdr.size()),
                                          m.body};
            (void)c->send_scatter(parts);
        }
    };
    return h;
}

// Two-shard echo: shard 0 serves on loopback, shard 1's client sends n_msgs
// on open and collects replies into `got`.
struct EchoFixture {
    EchoFixture(SimRuntime& s, std::uint64_t n) : sim(s), n_msgs(n) {}

    SimRuntime& sim;
    std::uint64_t n_msgs;
    std::vector<std::string> got;
    Conn* client_conn = nullptr;
    int server_opens = 0;
    int client_opens = 0;
    int closes = 0;
    CloseReason last_close{};
    std::uint16_t port = 0;

    void build() {
        sim.add_shard([&](SimEM& em) {
            Handlers<EchoProto> h = echo_handlers(em);
            h.on_open = [&](ConnId, Peer) { ++server_opens; };
            h.on_close = [&](ConnId, CloseReason r) {
                ++closes;
                last_close = r;
            };
            ServerConfig cfg;
            cfg.bind = SockAddr::loopback(0);
            auto srv = em.template make_server<EchoProto>(cfg, std::move(h));
            REQUIRE(srv.has_value());
            port = (*srv)->bound_addr().port();
        });
        sim.add_shard([&](SimEM& em) {
            Handlers<EchoProto> h;
            h.on_open = [&](ConnId id, Peer) {
                ++client_opens;
                client_conn = Conn::resolve(em, id);
                REQUIRE(client_conn != nullptr);
                for (std::uint64_t i = 0; i < n_msgs; ++i) {
                    auto f = echo_frame("m" + std::to_string(i));
                    (void)client_conn->send(ByteSpan(f.data(), f.size()));
                }
            };
            h.on_messages = [&](ConnId, std::span<const EchoMsg> batch) {
                for (auto& m : batch)
                    got.emplace_back(
                        reinterpret_cast<const char*>(m.body.data()),
                        m.body.size());
            };
            h.on_close = [&](ConnId, CloseReason r) {
                ++closes;
                last_close = r;
            };
            ClientConfig cc;
            cc.target = Endpoint{"127.0.0.1", port};
            cc.auto_reconnect = false;  // reconnect jitter is RNG — keep pure
            auto cli = em.template make_client<EchoProto>(cc, std::move(h));
            REQUIRE(cli.has_value());
        });
    }

    void client_send(std::string_view body) {
        sim.post(
            1,
            [c = client_conn, f = echo_frame(body)]() mutable {
                if (c) (void)c->send(ByteSpan(f.data(), f.size()));
            },
            Nanos(0));
    }
};

}  // namespace

// ---- M10-02: virtual time only moves when the scheduler says so -----------

TEST_CASE("sim: virtual time advances only on demand") {
    SimRuntime sim(1);
    sim.add_shard();
    CHECK(sim.now().time_since_epoch() == Nanos(0));
    sim.run_for(250ms);
    CHECK(sim.now().time_since_epoch() == Nanos(250ms));
    sim.run_for(100ms);
    CHECK(sim.now().time_since_epoch() == Nanos(350ms));
}

TEST_CASE("sim: timers fire at exact virtual instants across shards") {
    SimRuntime sim(7);
    std::vector<std::pair<int, std::int64_t>> fired;
    sim.add_shard([&](SimEM& em) {
        em.after(50ms, [&](TimerCtx tc) {
            fired.push_back({0, tc.now.time_since_epoch().count()});
        });
        em.after(10ms, [&](TimerCtx tc) {
            fired.push_back({1, tc.now.time_since_epoch().count()});
        });
    });
    sim.add_shard([&](SimEM& em) {
        em.after(30ms, [&](TimerCtx tc) {
            fired.push_back({2, tc.now.time_since_epoch().count()});
        });
    });
    sim.run_for(100ms);
    REQUIRE(fired.size() == 3);
    CHECK(fired[0].second == Nanos(10ms).count());
    CHECK(fired[1].second == Nanos(30ms).count());
    CHECK(fired[2].second == Nanos(50ms).count());
}

// ---- M10-03: deterministic ITC --------------------------------------------

TEST_CASE("sim: scheduled posts land at deterministic instants") {
    SimRuntime sim(3);
    std::vector<std::int64_t> hits;
    sim.add_shard();
    sim.add_shard([&](SimEM& em) {
        em.defer([&] { hits.push_back(-1); });  // same-shard defer at t=0
    });
    sim.post(0, [&] { hits.push_back(Nanos(5ms).count()); }, 5ms);
    sim.post(0, [&] { hits.push_back(999); }, 5ms);
    sim.post(1, [&] { hits.push_back(777); }, 1ms);
    sim.run_for(20ms);
    REQUIRE(hits.size() == 4);
    CHECK(hits[0] == -1);                  // setup-time defer ran first
    CHECK(hits[1] == 777);                 // 1ms post
    CHECK(hits[2] == Nanos(5ms).count());  // 5ms posts, in enqueue order
    CHECK(hits[3] == 999);
}

// ---- M10-06: replay — same seed, byte-identical trace ----------------------

TEST_CASE("sim: same seed produces identical event traces") {
    auto run = [](std::uint64_t seed) {
        SimRuntime sim(seed);
        sim.inject(FaultProfile{.min_latency = Nanos(0), .max_latency = 5ms});
        EchoFixture fx{sim, 3};
        fx.build();
        sim.post(0, [] {}, 2ms);
        sim.run_for(200ms);
        return sim.trace();
    };
    auto a = run(11);
    auto b = run(11);
    REQUIRE(!a.empty());
    CHECK(a == b);
}

TEST_CASE("sim: different seeds give different interleavings") {
    auto run = [](std::uint64_t seed) {
        SimRuntime sim(seed);
        sim.inject(FaultProfile{.partial_reads = true,
                                .min_latency = Nanos(0),
                                .max_latency = 9ms});
        EchoFixture fx{sim, 8};
        fx.build();
        sim.run_for(300ms);
        return sim.trace();
    };
    CHECK(run(1) != run(2));
}

// ---- M10-07: echo scenario through the fabric ------------------------------

TEST_CASE("sim: client connects, echoes, and closes through SimNet") {
    SimRuntime sim(5);
    EchoFixture fx{sim, 4};
    fx.build();
    sim.run_for(1s);
    CHECK(fx.client_opens == 1);
    CHECK(fx.server_opens == 1);
    REQUIRE(fx.got.size() == 4);
    for (int i = 0; i < 4; ++i) CHECK(fx.got[i] == "m" + std::to_string(i));
}

TEST_CASE("sim: orderly close delivers FIN to the peer") {
    SimRuntime sim(6);
    EchoFixture fx{sim, 1};
    fx.build();
    sim.run_for(500ms);
    REQUIRE(fx.got.size() == 1);
    // Client closes; the server should see PeerFin through the net.
    sim.post(1, [c = fx.client_conn] { c->close(CloseReason::LocalClose); });
    sim.run_for(500ms);
    CHECK(fx.closes >= 2);  // both ends observed a close
}

TEST_CASE("sim: connect refused when nothing listens") {
    SimRuntime sim(1);
    int closes = 0;
    CloseReason reason{};
    sim.add_shard([&](SimEM& em) {
        Handlers<EchoProto> h;
        h.on_messages = [](ConnId, std::span<const EchoMsg>) {};
        h.on_close = [&](ConnId, CloseReason r) {
            ++closes;
            reason = r;
        };
        ClientConfig cc;
        cc.target = Endpoint{"127.0.0.1", 9};
        cc.auto_reconnect = false;
        auto cli = em.template make_client<EchoProto>(cc, std::move(h));
        REQUIRE(cli.has_value());
    });
    sim.run_for(10s);  // refused delivery + connect-timeout budget
    CHECK(closes >= 1);
    CHECK(reason == CloseReason::ConnectFailed);
}

// ---- M10-04: fault injection
// -------------------------------------------------

TEST_CASE("sim: packet_loss=1 delivers nothing") {
    SimRuntime sim(9);
    sim.inject(FaultProfile{.packet_loss = 1.0});
    EchoFixture fx{sim, 2};
    fx.build();
    sim.run_for(2s);
    CHECK(fx.client_opens == 1);  // control plane still completes
    CHECK(fx.got.empty());
    CHECK(sim.net().dropped() > 0);
}

TEST_CASE("sim: partial_reads fragment but framing reassembles") {
    SimRuntime sim(13);
    sim.inject(FaultProfile{.partial_reads = true});
    EchoFixture fx{sim, 6};
    fx.build();
    sim.run_for(2s);
    REQUIRE(fx.got.size() == 6);
    for (int i = 0; i < 6; ++i) CHECK(fx.got[i] == "m" + std::to_string(i));
}

TEST_CASE("sim: partition blackholes traffic until it heals") {
    SimRuntime sim(17);
    EchoFixture fx{sim, 1};
    fx.build();
    sim.run_for(500ms);
    REQUIRE(fx.got.size() == 1);
    REQUIRE(sim.net().links() == 1);

    sim.net().partition(0, 200ms);
    auto dropped0 = sim.net().dropped();
    // A send that crosses during the blackout is lost — the sim does not
    // model TCP retransmit.
    sim.post(
        1,
        [c = fx.client_conn] {
            if (c) {
                auto f = echo_frame("lost-in-partition");
                (void)c->send(ByteSpan(f.data(), f.size()));
            }
        },
        10ms);
    sim.run_for(100ms);  // still inside the window
    CHECK(sim.net().dropped() > dropped0);
    CHECK(fx.got.size() == 1);

    sim.run_for(500ms);  // heal + new traffic crosses
    fx.client_send("after-heal");
    sim.run_for(200ms);
    CHECK(fx.got.size() == 2);
    CHECK(fx.got[1] == "after-heal");
}

TEST_CASE("sim: reset_chance=1 resets the link") {
    SimRuntime sim(21);
    sim.inject(FaultProfile{.reset_chance = 1.0});
    EchoFixture fx{sim, 1};
    fx.build();
    sim.run_for(1s);
    CHECK(fx.closes >= 1);
    CHECK(fx.last_close == CloseReason::Error);
}

TEST_CASE("sim: slow_peer adds latency without loss") {
    SimRuntime sim(23);
    sim.inject(FaultProfile{.slow_peer = 1.0, .slow_latency = 40ms});
    EchoFixture fx{sim, 1};
    fx.build();
    // At t<40ms nothing can have arrived back yet.
    sim.run_for(30ms);
    CHECK(fx.got.empty());
    sim.run_for(500ms);
    CHECK(fx.got.size() == 1);
}

// ---- M10-05: invariant checkers
// ------------------------------------------------

TEST_CASE("sim: per-step invariant hook sees every step") {
    SimRuntime sim(4);
    EchoFixture fx{sim, 1};
    fx.build();
    std::size_t steps_seen = 0;
    bool time_monotone = true;
    TimePoint last{};
    sim.on_step([&](SimRuntime& s) {
        ++steps_seen;
        if (s.now() < last) time_monotone = false;
        last = s.now();
        for (std::size_t i = 0; i < s.size(); ++i)
            if (s.shard(i).stats().mailbox_full > 0) s.mark_invariant_broken();
    });
    sim.run_for(300ms);
    CHECK(steps_seen == sim.steps());
    CHECK(time_monotone);
    CHECK(sim.invariants_held());
}

// ---- M10 exit: multi-shard proxy scenario under 1s wall
// ----------------------

TEST_CASE("sim: 3-shard proxy scenario completes fast") {
    // client (shard 2) → proxy (shard 1) → upstream echo (shard 0) → back.
    SimRuntime sim(99);

    struct Proxy {
        Conn* upstream = nullptr;
        Conn* front = nullptr;
    };
    auto* proxy = new Proxy{};  // lives for the scenario

    std::uint16_t upstream_port = 0, front_port = 0;

    sim.add_shard([&](SimEM& em) {
        Handlers<EchoProto> h = echo_handlers(em);
        ServerConfig cfg;
        cfg.bind = SockAddr::loopback(0);
        auto srv = em.template make_server<EchoProto>(cfg, std::move(h));
        REQUIRE(srv.has_value());
        upstream_port = (*srv)->bound_addr().port();
    });

    sim.add_shard([&](SimEM& em) {
        Handlers<EchoProto> up;
        up.on_messages = [&, proxy](ConnId, std::span<const EchoMsg> batch) {
            if (!proxy->front) return;
            for (auto& m : batch) {
                std::array<std::byte, sizeof(EchoHeader)> hdr;
                std::memcpy(hdr.data(), &m.header, sizeof(hdr));
                std::array<ByteSpan, 2> parts{ByteSpan(hdr.data(), hdr.size()),
                                              m.body};
                (void)proxy->front->send_scatter(parts);
            }
        };
        up.on_open = [&, proxy](ConnId id, Peer) {
            proxy->upstream = Conn::resolve(em, id);
        };
        Handlers<EchoProto> front;
        front.on_open = [&, proxy](ConnId id, Peer) {
            proxy->front = Conn::resolve(em, id);
        };
        front.on_messages = [&, proxy](ConnId, std::span<const EchoMsg> batch) {
            if (!proxy->upstream) return;
            for (auto& m : batch) {
                std::array<std::byte, sizeof(EchoHeader)> hdr;
                std::memcpy(hdr.data(), &m.header, sizeof(hdr));
                std::array<ByteSpan, 2> parts{ByteSpan(hdr.data(), hdr.size()),
                                              m.body};
                (void)proxy->upstream->send_scatter(parts);
            }
        };
        ServerConfig cfg;
        cfg.bind = SockAddr::loopback(0);
        auto srv = em.template make_server<EchoProto>(cfg, std::move(front));
        REQUIRE(srv.has_value());
        front_port = (*srv)->bound_addr().port();
        ClientConfig cc;
        cc.target = Endpoint{"127.0.0.1", upstream_port};
        cc.auto_reconnect = false;
        auto cli = em.template make_client<EchoProto>(cc, std::move(up));
        REQUIRE(cli.has_value());
    });

    int replies = 0;
    sim.add_shard([&](SimEM& em) {
        Handlers<EchoProto> h;
        h.on_open = [&](ConnId id, Peer) {
            auto* c = Conn::resolve(em, id);
            REQUIRE(c != nullptr);
            for (int i = 0; i < 200; ++i) {
                auto f = echo_frame("payload" + std::to_string(i));
                (void)c->send(ByteSpan(f.data(), f.size()));
            }
        };
        h.on_messages = [&](ConnId, std::span<const EchoMsg> batch) {
            replies += int(batch.size());
        };
        ClientConfig cc;
        cc.target = Endpoint{"127.0.0.1", front_port};
        cc.auto_reconnect = false;
        auto cli = em.template make_client<EchoProto>(cc, std::move(h));
        REQUIRE(cli.has_value());
    });

    auto wall0 = std::chrono::steady_clock::now();
    sim.run_for(50s);
    auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - wall0)
                       .count();
    MESSAGE("proxy scenario: ", wall_ms, "ms wall, steps=", sim.steps(),
            ", replies=", replies, ", trace=", sim.trace().size());
    CHECK(wall_ms < 1000);  // M10 exit: well under a second
    CHECK(replies == 200);  // full round trip through the proxy
    delete proxy;
}

// ---- M10-05/06 exit: a seeded fault reproduces an injected bug -------------

TEST_CASE("sim: seeded fault reproduces an app bug identically") {
    // Deliberately-buggy app: it assumes the first send always lands — no
    // retry, no close handling. Under packet loss some sends vanish and the
    // app silently loses messages. The SAME seed must lose the SAME
    // messages in the SAME order — the failure is replayable from the seed.
    auto run = [](std::uint64_t seed) {
        SimRuntime sim(seed);
        // Lossy but not dead: ~30% of deliveries vanish.
        sim.inject(FaultProfile{.packet_loss = 0.3});
        std::vector<std::string> got;
        std::uint16_t port = 0;
        sim.add_shard([&](SimEM& em) {
            Handlers<EchoProto> h = echo_handlers(em);
            ServerConfig cfg;
            cfg.bind = SockAddr::loopback(0);
            auto srv = em.template make_server<EchoProto>(cfg, std::move(h));
            REQUIRE(srv.has_value());
            port = (*srv)->bound_addr().port();
        });
        sim.add_shard([&](SimEM& em) {
            Handlers<EchoProto> h;
            h.on_open = [&](ConnId id, Peer) {
                auto* c = Conn::resolve(em, id);
                REQUIRE(c != nullptr);
                // One send per timer tick → one delivery per message →
                // per-message loss draws (a batched sendv would be one
                // all-or-nothing draw).
                auto n = std::make_shared<int>(0);
                em.every(
                    1ms,
                    [c, n](TimerCtx) mutable {
                        if (++*n > 32) return;
                        auto f = echo_frame("m" + std::to_string(*n - 1));
                        (void)c->send(ByteSpan(f.data(), f.size()));
                    },
                    1ms);
            };
            h.on_messages = [&](ConnId, std::span<const EchoMsg> batch) {
                for (auto& m : batch)
                    got.emplace_back(
                        reinterpret_cast<const char*>(m.body.data()),
                        m.body.size());
            };
            ClientConfig cc;
            cc.target = Endpoint{"127.0.0.1", port};
            cc.auto_reconnect = false;
            auto cli = em.template make_client<EchoProto>(cc, std::move(h));
            REQUIRE(cli.has_value());
        });
        sim.run_for(10s);
        return std::pair{got, sim.trace()};
    };
    auto [got_a, trace_a] = run(4);
    auto [got_b, trace_b] = run(4);
    // Identical loss pattern, identical trace — the bug replays exactly.
    CHECK(trace_a == trace_b);
    CHECK(got_a == got_b);
    CHECK(got_a.size() < 32);  // the injected bug lost messages
    CHECK(!got_a.empty());     // but the link wasn't dead
}

TEST_CASE("sim: run_until_idle drains to quiescence") {
    SimRuntime sim(8);
    int timers = 0;
    sim.add_shard(
        [&](SimEM& em) { em.every(5ms, [&](TimerCtx) { ++timers; }, 5ms); });
    // A repeating timer never goes idle — bound the drain.
    CHECK(!sim.run_until_idle(50));
    CHECK(timers > 0);
    CHECK(sim.now().time_since_epoch() > Nanos(0));
}

// ---- M10-08: nightly randomized-seed campaign
// --------------------------------
//
// AFX_SIM_SEED picks the campaign seed (nightly sweeps many). The profile is
// derived from the seed so "AFX_SIM_SEED=<n> afx_tests -tc='sim:campaign*'"
// reproduces a failure exactly — the seed and the trace tail print on any
// failure, which is what the nightly job files.

TEST_CASE("sim:campaign seeded fault sweep") {
    std::uint64_t seed = 1;
    if (const char* s = std::getenv("AFX_SIM_SEED"))
        seed = std::strtoull(s, nullptr, 10);
    CAPTURE(seed);

    // Derive the whole fault profile from the seed — every knob moves.
    std::mt19937_64 rng(seed);
    auto p = [&] { return std::generate_canonical<double, 53>(rng); };
    FaultProfile f;
    f.packet_loss = p() * 0.02;  // ≤2%
    f.partial_reads = p() < 0.5;
    f.slow_peer = p() * 0.4;
    f.slow_latency = Nanos(std::int64_t(p() * 20e6));
    f.reset_chance = p() * 0.001;
    f.partition_chance = p() * 0.01;
    f.partition_window = {Nanos(std::int64_t(p() * 5e6)),
                          Nanos(std::int64_t(5e6 + p() * 50e6))};
    f.min_latency = Nanos(0);
    f.max_latency = Nanos(std::int64_t(p() * 10e6));

    SimRuntime sim(seed);
    sim.inject(f);

    constexpr int kMsgs = 64;
    std::vector<std::string> got;
    std::uint16_t port = 0;
    sim.add_shard([&](SimEM& em) {
        Handlers<EchoProto> h = echo_handlers(em);
        ServerConfig cfg;
        cfg.bind = SockAddr::loopback(0);
        auto srv = em.template make_server<EchoProto>(cfg, std::move(h));
        REQUIRE(srv.has_value());
        port = (*srv)->bound_addr().port();
    });
    sim.add_shard([&](SimEM& em) {
        Handlers<EchoProto> h;
        h.on_open = [&](ConnId id, Peer) {
            auto* c = Conn::resolve(em, id);
            REQUIRE(c != nullptr);
            for (int i = 0; i < kMsgs; ++i) {
                auto fr = echo_frame("m" + std::to_string(i));
                (void)c->send(ByteSpan(fr.data(), fr.size()));
            }
        };
        h.on_messages = [&](ConnId, std::span<const EchoMsg> batch) {
            for (auto& m : batch)
                got.emplace_back(reinterpret_cast<const char*>(m.body.data()),
                                 m.body.size());
        };
        ClientConfig cc;
        cc.target = Endpoint{"127.0.0.1", port};
        cc.auto_reconnect = false;
        auto cli = em.template make_client<EchoProto>(cc, std::move(h));
        REQUIRE(cli.has_value());
    });

    // Invariant hook: virtual time is monotone; mailboxes never overflowed.
    sim.on_step([last = TimePoint{}](SimRuntime& s) mutable {
        if (s.now() < last) s.mark_invariant_broken();
        last = s.now();
        for (std::size_t i = 0; i < s.size(); ++i)
            if (s.shard(i).stats().mailbox_full > 0) s.mark_invariant_broken();
    });

    sim.run_for(60s);

    // Invariants that must hold under ANY fault profile:
    //  * replies are a subsequence of the sent stream (ordered, no dupes,
    //    no corruption) — loss may shrink it, never reorder it;
    //  * the virtual clock only moved forward;
    //  * the run finished the whole 60 virtual seconds.
    int prev = -1;
    bool ordered = true;
    for (auto& g : got) {
        if (g.rfind("m", 0) != 0) {
            ordered = false;
            break;
        }
        int idx = std::atoi(g.c_str() + 1);
        if (idx <= prev || idx >= kMsgs) ordered = false;
        prev = idx;
    }
    if (!ordered || !sim.invariants_held()) {
        // Dump enough state to replay: seed + trace tail.
        MESSAGE("AFX_SIM_SEED=", seed, " trace_events=", sim.trace().size());
        for (std::size_t i = sim.trace().size() > 20 ? sim.trace().size() - 20
                                                     : 0;
             i < sim.trace().size(); ++i) {
            const auto& r = sim.trace()[i];
            MESSAGE("  ev seq=", r.seq, " due_ns=", r.due_ns,
                    " kind=", int(r.kind), " fd=", r.fd, " aux=", r.aux,
                    " bytes=", r.bytes_hash);
        }
    }
    CHECK(ordered);
    CHECK(sim.invariants_held());
    CHECK(sim.now().time_since_epoch() == Nanos(60s));
}

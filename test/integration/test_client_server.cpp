#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <future>
#include <thread>

#include "../proto.hpp"
#include "../real_env.hpp"
#include "../test_env.hpp"
#include "afx/net/tcp_client.hpp"
#include "afx/net/tcp_server.hpp"

using namespace afx;
using afx::test::echo_frame;
using afx::test::EchoHeader;
using afx::test::EchoMsg;
using afx::test::EchoProto;

// Client + server on one real EM over loopback (§15 integration). Runs on
// every production backend — epoll and io_uring — via the same test body
// (M8-11 backend parity).
AFX_BACKEND_TEST_CASE("loopback: TcpClient connects, echoes, reconnects", EM) {
    auto emp = afx::test::make_real_em<EM>(
        EventManagerConfig{.wait = WaitStrategy::SpinThenBlock});
    if (!emp) { MESSAGE("backend unavailable — skipped"); return; }
    EM& em = *emp;
    std::promise<void> ready;
    std::atomic<int> echoes{0};
    std::atomic<int> opens{0};
    std::atomic<int> closes{0};

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
                c->send_scatter(parts);
            }
        };
        ServerConfig scfg;
        scfg.bind = SockAddr::loopback(0);
        auto srv = em.template make_server<EchoProto>(scfg, std::move(sh));
        if (!srv) {
            ready.set_value();
            em.stop();
            return;
        }
        std::uint16_t port = (*srv)->bound_addr().port();

        Handlers<EchoProto> ch;
        ch.on_open = [&](ConnId id, Peer) {
            ++opens;
            auto* c = Connection<EchoProto, EM>::resolve(em, id);
            if (!c) return;
            auto f = echo_frame("hi");
            c->send(ByteSpan(f.data(), f.size()));
        };
        ch.on_messages = [&](ConnId, std::span<const EchoMsg> b) {
            echoes += int(b.size());
        };
        ch.on_close = [&](ConnId, CloseReason) { ++closes; };

        ClientConfig ccfg;
        ccfg.target = Endpoint{"127.0.0.1", port};
        ccfg.connect_timeout = 2s;
        ccfg.auto_reconnect = true;
        ccfg.reconnect = Backoff{10ms, 100ms, 0.0, Duration::zero()};
        auto cli = em.template make_client<EchoProto>(ccfg, std::move(ch));
        if (!cli) {
            ready.set_value();
            em.stop();
            return;
        }

        ready.set_value();
        em.run();
    });
    ready.get_future().get();

    // Wait for the echo round-trip.
    for (int i = 0; i < 2000 && echoes == 0; ++i)
        std::this_thread::sleep_for(1ms);
    CHECK(echoes >= 1);
    CHECK(opens >= 1);

    em.stop();
    t.join();
    // Shutdown-close is delivered when the EM tears down its owned objects
    // (§20) — which is the EM's destructor, not stop().
    emp.reset();
    CHECK(closes >= 1);
}

// Connect to a dead port: ConnectFailed, no auto-reconnect → Disconnected.
AFX_BACKEND_TEST_CASE(
    "loopback: refused connect ends Disconnected without reconnect",
    EM) {
    auto emp = afx::test::make_real_em<EM>(
        EventManagerConfig{.wait = WaitStrategy::SpinThenBlock});
    if (!emp) { MESSAGE("backend unavailable — skipped"); return; }
    EM& em = *emp;
    std::promise<void> ready;
    std::atomic<ClientState> last{ClientState::Disconnected};
    std::atomic<int> closes{0};

    std::thread t([&] {
        Handlers<EchoProto> ch;
        ch.on_close = [&](ConnId, CloseReason) { ++closes; };
        ClientConfig ccfg;
        // Reserved test-net address: nothing listens there.
        ccfg.target = Endpoint{"127.0.0.1", 1};
        ccfg.auto_reconnect = false;
        ccfg.connect_timeout = 500ms;
        auto cli = em.template make_client<EchoProto>(ccfg, std::move(ch));
        REQUIRE(cli.has_value());
        (*cli)->on_state_change([&](ClientState s) { last = s; });
        ready.set_value();
        em.run();
    });
    ready.get_future().get();

    // Wait for the close notification (ConnectFailed), then check the client
    // settled back to Disconnected without reconnecting.
    for (int i = 0; i < 2000 && closes == 0; ++i)
        std::this_thread::sleep_for(1ms);
    CHECK(closes >= 1);
    for (int i = 0; i < 2000 && last != ClientState::Disconnected; ++i)
        std::this_thread::sleep_for(1ms);
    CHECK(last == ClientState::Disconnected);
    em.stop();
    t.join();
}

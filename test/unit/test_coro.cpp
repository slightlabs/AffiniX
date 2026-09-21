// M9 coroutine layer tests (M9-07 equivalence + lifecycle). Sim-driven
// where possible (deterministic); loopback for the conn paths, mirroring
// the callback-mode integration tests so behaviour is comparable.

#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <future>
#include <stdexcept>
#include <thread>

#include "../proto.hpp"
#include "../real_env.hpp"
#include "../test_env.hpp"
#include "afx/coro/combine.hpp"
#include "afx/coro/conn.hpp"
#include "afx/coro/scope.hpp"

using namespace afx;
using afx::test::EchoHeader;
using afx::test::EchoMsg;
using afx::test::EchoProto;
using SimEM = afx::test::TestEnv::EM;

namespace {

// ---- helper tasks ---------------------------------------------------------
// First parameter carries the EM → arena-allocated frame (spike 0002).

CoroTask<int> produce(SimEM&, int v) {
    co_return v;
}

CoroTask<void> store(SimEM&, int* out, int v) {
    *out = v;
    co_return;
}

CoroTask<void> parent_sum(SimEM& em, int* out) {
    int a = co_await produce(em, 20);
    int b = co_await produce(em, 22);
    *out = a + b;
}

CoroTask<int> boom(SimEM&) {
    throw std::runtime_error("child failure");
}

CoroTask<void> catcher(SimEM& em, bool* caught) {
    try {
        (void)co_await boom(em);
    } catch (const std::runtime_error&) {
        *caught = true;
    }
}

CoroTask<void> sleeper(SimEM& em, Duration d, int* hits, bool* ok) {
    auto r = co_await em.sleep(d);
    ++*hits;
    if (ok) *ok = bool(r);
}

CoroTask<void> ambient_reader(SimEM&, TraceId* out) {
    *out = detail::ambient_context().trace;
    co_return;
}

CoroTask<void> scope_parent(SimEM& em, int* done) {
    coro::TaskScope scope(em);
    auto worker = [](SimEM&, int* d) -> CoroTask<void> {
        ++*d;
        co_return;
    };
    scope.spawn(worker(em, done));
    scope.spawn(worker(em, done));
    co_await scope.join();
}

CoroTask<void> all_runner(SimEM& em, int* out) {
    auto r = co_await coro::when_all(produce(em, 2), produce(em, 3));
    if (r) *out = std::get<0>(*r) + std::get<1>(*r);
}

CoroTask<void> any_runner(SimEM& em, std::size_t* winner, int* value) {
    // fast wins over slow: slow sleeps an hour, fast returns immediately.
    auto slow = [](SimEM& e) -> CoroTask<int> {
        (void)co_await e.sleep(1h);
        co_return -1;
    };
    auto r = co_await coro::when_any(slow(em), produce(em, 7));
    if (r) {
        *winner = r->first;
        *value = std::get<1>(r->second);
    }
}

CoroTask<void> timeout_runner(SimEM& em, Error* err) {
    auto hang = [](SimEM& e) -> CoroTask<int> {
        (void)co_await e.sleep(1h);
        co_return 0;
    };
    auto r = co_await coro::with_timeout(10ms, hang(em));
    if (!r) *err = r.error();
}

CoroTask<void> timeout_fast(SimEM& em, int* out) {
    auto r = co_await coro::with_timeout(1h, produce(em, 9));
    if (r) *out = *r;
}

CoroTask<void> reply_runner(SimEM&, Mailbox target, int* out) {
    auto r = co_await coro::post_and_reply(target, [] { return 41; });
    if (r) *out = *r;
}

CoroTask<void> scope_cancelled_child(SimEM& em, int* cancelled_hits) {
    coro::TaskScope scope(em);
    scope.spawn(sleeper(em, 1h, cancelled_hits, nullptr));
    scope.request_stop();
    co_await scope.join();
}

}  // namespace

TEST_CASE("coro: tasks are lazy — nothing runs before spawn/await") {
    afx::test::TestEnv env;
    int ran = 0;
    auto t = store(env.em, &ran, 7);  // creates the frame, does not run
    CHECK(ran == 0);
    env.pump();
    CHECK(ran == 0);
    t.start();  // manual start for an unspawned task
    CHECK(ran == 7);
}

TEST_CASE("coro: zero-byte arena falls back to heap frames (observable)") {
    afx::test::TestEnv env(EventManagerConfig{.wait = WaitStrategy::Spin,
                                              .memory = {.arena_bytes = 0}});
    int ran = 0;
    env.em.spawn(store(env.em, &ran, 1));
    CHECK(ran == 1);
    CHECK(env.em.coro_heap_frames() == 1);  // fallback counted, not silent
}

TEST_CASE("coro: spawn runs the task on the EM; frame comes from the arena") {
    afx::test::TestEnv env;
    int ran = 0;
    env.em.spawn(store(env.em, &ran, 42));
    CHECK(ran == 42);  // spawn resumes immediately (lazy task, sync start)
    CHECK(env.em.stats().coro_spawned == 1);
    CHECK(env.em.stats().coro_completed == 1);
    CHECK(env.em.coro_heap_frames() == 0);  // arena frame, no heap fallback
    env.pump();
    CHECK(ran == 42);
}

TEST_CASE("coro: nested co_await returns values up the chain") {
    afx::test::TestEnv env;
    int out = 0;
    env.em.spawn(parent_sum(env.em, &out));
    CHECK(out == 42);
}

TEST_CASE("coro: child exception rethrows at the parent's co_await") {
    afx::test::TestEnv env;
    bool caught = false;
    env.em.spawn(catcher(env.em, &caught));
    CHECK(caught);
}

TEST_CASE("coro: sleep suspends and resumes on virtual time") {
    afx::test::TestEnv env;
    int hits = 0;
    bool ok = false;
    env.em.spawn(sleeper(env.em, 50ms, &hits, &ok));
    CHECK(hits == 0);  // suspended
    env.advance(49ms);
    CHECK(hits == 0);
    env.advance(1ms);
    CHECK(hits == 1);
    CHECK(ok);
}

TEST_CASE("coro: ambient Context follows the task across suspension") {
    afx::test::TestEnv env;
    TraceId seen{};
    Context ctx;
    ctx.trace = TraceId{0xDEAD, 0xBEEF, 7};
    env.em.spawn(ambient_reader(env.em, &seen), ctx);
    CHECK(seen == ctx.trace);
}

TEST_CASE("coro: TaskScope joins children") {
    afx::test::TestEnv env;
    int done = 0;
    env.em.spawn(scope_parent(env.em, &done));
    env.pump();
    CHECK(done == 2);  // join() waited for both children
}

TEST_CASE("coro: TaskScope request_stop cancels a parked child") {
    afx::test::TestEnv env;
    int hits = 0;
    env.em.spawn(scope_cancelled_child(env.em, &hits));
    env.pump();
    // The child's sleep was cancelled mid-flight; the hit landed with a
    // Cancelled result and join() still returned.
    CHECK(hits == 1);
}

TEST_CASE("coro: when_all collects results from both children") {
    afx::test::TestEnv env;
    int out = 0;
    env.em.spawn(all_runner(env.em, &out));
    CHECK(out == 5);
}

TEST_CASE("coro: when_any returns the first finisher and cancels the rest") {
    afx::test::TestEnv env;
    std::size_t winner = 99;
    int value = -1;
    env.em.spawn(any_runner(env.em, &winner, &value));
    env.pump();
    CHECK(winner == 1);
    CHECK(value == 7);
}

TEST_CASE("coro: with_timeout expires a hung child") {
    afx::test::TestEnv env;
    Error err{};
    env.em.spawn(timeout_runner(env.em, &err));
    env.advance(10ms);
    CHECK(err.category == ErrorCategory::Cancelled);
    CHECK(err.code == std::uint16_t(Err::Expired));
}

TEST_CASE("coro: with_timeout passes through a fast result") {
    afx::test::TestEnv env;
    int out = 0;
    env.em.spawn(timeout_fast(env.em, &out));
    CHECK(out == 9);
}

TEST_CASE("coro: post_and_reply round-trips through the same mailbox") {
    afx::test::TestEnv env;
    int out = 0;
    env.em.spawn(reply_runner(env.em, env.em.mailbox(), &out));
    env.pump();  // drain the work item
    env.pump();  // drain the reply hop
    CHECK(out == 41);
}

TEST_CASE("coro: EM teardown cancels a parked root cleanly") {
    int hits = 0;
    {
        afx::test::TestEnv env;
        env.em.spawn(sleeper(env.em, 1h, &hits, nullptr));
        CHECK(env.em.stats().coro_spawned == 1);
        // ~TestEnv → ~BasicEventManager: the parked root is cancelled and
        // destroyed here — no leak, no dangling timer.
    }
    CHECK(hits == 1);  // cancel thunk resumed it once with Cancelled
}

// ---- loopback session equivalence (M9-07) -----------------------------------
// The same echo behaviour through the coroutine layer on every production
// backend — the coroutine session must deliver what the callback session
// delivers.
AFX_BACKEND_TEST_CASE("coro: echo session over loopback", EM) {
    auto emp = afx::test::make_real_em<EM>(
        EventManagerConfig{.wait = WaitStrategy::SpinThenBlock});
    if (!emp) {
        MESSAGE("backend unavailable — skipped");
        return;
    }
    EM& em = *emp;
    std::promise<void> ready;
    std::atomic<int> echoes{0};

    std::thread t([&] {
        ServerConfig scfg;
        scfg.bind = SockAddr::loopback(0);
        auto srv = em.template make_coro_server<EchoProto>(
            scfg, [](ConnRef<EchoProto, EM> c) -> CoroTask<void> {
                while (auto batch = co_await c.recv()) {
                    for (auto& m : *batch) {
                        auto f = afx::test::echo_frame(std::string_view(
                            reinterpret_cast<const char*>(m.body.data()),
                            m.body.size()));
                        auto r = co_await c.send(ByteSpan(f.data(), f.size()));
                        if (r == SendResult::Closed) co_return;
                    }
                }
            });
        if (!srv) {
            ready.set_value();
            em.stop();
            return;
        }
        std::uint16_t port = (*srv)->bound_addr().port();

        Handlers<EchoProto> ch;
        ch.on_open = [&](ConnId id, Peer) {
            if (auto* c = Connection<EchoProto, EM>::resolve(em, id)) {
                auto f = afx::test::echo_frame("hi");
                (void)c->send(ByteSpan(f.data(), f.size()));
            }
        };
        ch.on_messages = [&](ConnId, std::span<const EchoMsg> b) {
            echoes += int(b.size());
        };
        ClientConfig ccfg;
        ccfg.target = Endpoint{"127.0.0.1", port};
        ccfg.connect_timeout = 2s;
        ccfg.auto_reconnect = false;
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
    for (int i = 0; i < 2000 && echoes == 0; ++i)
        std::this_thread::sleep_for(1ms);
    CHECK(echoes >= 1);

    em.stop();
    t.join();
}

// connect awaiter: coroutine client side of the same echo exchange.
AFX_BACKEND_TEST_CASE("coro: connect awaiter + session over loopback", EM) {
    auto emp = afx::test::make_real_em<EM>(
        EventManagerConfig{.wait = WaitStrategy::SpinThenBlock});
    if (!emp) {
        MESSAGE("backend unavailable — skipped");
        return;
    }
    EM& em = *emp;
    std::promise<void> ready;
    std::atomic<int> echoes{0};

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
        if (!srv) {
            ready.set_value();
            em.stop();
            return;
        }
        std::uint16_t port = (*srv)->bound_addr().port();

        // The client's user-visible on_messages still counts echoes; the
        // connect itself is driven by a coroutine that awaits Connected.
        Handlers<EchoProto> ch;
        ch.on_messages = [&](ConnId, std::span<const EchoMsg> b) {
            echoes += int(b.size());
        };
        ClientConfig ccfg;
        ccfg.target = Endpoint{"127.0.0.1", port};
        ccfg.connect_timeout = 2s;
        ccfg.auto_reconnect = false;
        auto cli = em.template make_client<EchoProto>(ccfg, std::move(ch));
        if (!cli) {
            ready.set_value();
            em.stop();
            return;
        }
        TcpClient<EchoProto, EM>* clip = *cli;

        em.spawn([](EM& e, TcpClient<EchoProto, EM>* c,
                    std::atomic<int>* n) -> CoroTask<void> {
            auto r = co_await coro::ConnectOp<EchoProto, EM>(c);
            if (!r) co_return;
            ConnRef<EchoProto, EM> conn = *r;
            auto f = afx::test::echo_frame("hi");
            (void)co_await conn.send(ByteSpan(f.data(), f.size()));
            // The echo lands via the callback on_messages — coroutine and
            // callback paths coexist on the same connection.
            while (n->load() == 0) co_await e.sleep(1ms);
        }(em, clip, &echoes));

        ready.set_value();
        em.run();
    });
    ready.get_future().get();
    for (int i = 0; i < 2000 && echoes == 0; ++i)
        std::this_thread::sleep_for(1ms);
    CHECK(echoes >= 1);

    em.stop();
    t.join();
}

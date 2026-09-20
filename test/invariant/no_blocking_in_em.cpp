// Invariant test (IMPLEMENTATION_PLAN.md M3-12): an EM thread must never
// block when work is pending, and a configured-Spin loop must never block at
// all. Conversely a Block-mode loop must actually sleep and be woken by a
// mailbox post — the wake path the lost-wakeup stress test hammers.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "../test_env.hpp"
#include "afx/core/event_manager.hpp"

using namespace afx;

TEST_CASE("invariant: Spin wait strategy never blocks") {
    EventManager em(EventManagerConfig{.wait = WaitStrategy::Spin});
    for (int i = 0; i < 200; ++i) em.poll_once();
    CHECK(em.stats().blocks == 0);
    CHECK(em.stats().spins > 0);

    // Pending defer/mailbox work keeps it out of any wait regardless.
    em.defer([] {});
    (void)em.post([] {});
    em.poll_once();
    CHECK(em.stats().blocks == 0);
}

TEST_CASE("invariant: Block loop sleeps when idle and wakes on post") {
    EventManager em(EventManagerConfig{.wait = WaitStrategy::Block});
    std::atomic<bool> ran{false};

    std::thread t([&] { em.run(); });
    // Give the loop a moment to reach its first blocking wait.
    std::this_thread::sleep_for(20ms);

    CHECK(em.post([&] { ran = true; }) == PostResult::Ok);

    const auto deadline_tp = std::chrono::steady_clock::now() + 2s;
    while (!ran.load() && std::chrono::steady_clock::now() < deadline_tp)
        std::this_thread::sleep_for(1ms);
    CHECK(ran.load());                       // the wake was not lost

    em.stop();
    t.join();
    CHECK(em.stats().blocks > 0);            // it genuinely slept
    CHECK(em.stats().wakeups > 0);           // and the wake was observed
}

TEST_CASE("invariant: SpinThenBlock escapes the spin budget then blocks") {
    EventManagerConfig cfg;
    cfg.wait = WaitStrategy::SpinThenBlock;
    cfg.spin_budget = 1ms;
    EventManager em(cfg);

    // at() deadlines are precise — they live on the heap, not the wheel —
    // so each blocking wait is bounded by the next deadline (~2 ms). The
    // wheel has nothing to grind toward real-clock time.
    int fired = 0;
    for (int k = 1; k <= 3; ++k)
        em.at(em.clock().now() + 2ms * k, [&](TimerCtx) { ++fired; });
    // Keep one later deadline pending: once the heap empties a blocking
    // poll sleeps ~forever by design, and this test drives poll_once
    // synchronously with no other thread to wake it.
    em.at(em.clock().now() + 30ms, [](TimerCtx) {});

    for (int i = 0; i < 64 && fired < 3; ++i) em.poll_once();
    CHECK(fired >= 3);
    CHECK(em.stats().blocks > 0);     // it genuinely slept to the deadline

    // Work arriving while it would block must still be handled: a posted
    // task runs on the next iteration, never stranded behind the wake.
    int ran = 0;
    (void)em.post([&] { ++ran; });
    em.at(em.clock().now() + 10ms, [](TimerCtx) {});   // bound this wait too
    em.poll_once();
    CHECK(ran == 1);
}

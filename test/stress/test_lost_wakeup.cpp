// Lost-wakeup stress for the arm/block protocol (IMPLEMENTATION_PLAN.md
// M3-05, DESIGN.md §8.1): producers post at randomized intervals straddling
// the consumer's spin budget so it flips between spinning and blocking.
// Every message must be delivered; the loop must never sleep with a
// non-empty queue.
//
// Bounded for PR CI; set AFX_STRESS_SCALE for the longer nightly run.
// Under TSAN this doubles as the data-race check for the protocol.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include "afx/core/event_manager.hpp"
#include "afx/itc/mpsc_ring.hpp"
#include "afx/itc/spsc_ring.hpp"

using namespace afx;

namespace {

int stress_scale() {
    if (const char* s = std::getenv("AFX_STRESS_SCALE"))
        return std::max(1, std::atoi(s));
    return 1;
}

} // namespace

TEST_CASE("stress: every post is delivered across block/spin transitions") {
    const int scale = stress_scale();
    constexpr int kProducers = 4;
    const int kMsgs = 20'000 * scale;

    for (WaitStrategy wait : {WaitStrategy::Block, WaitStrategy::SpinThenBlock}) {
        CAPTURE(int(wait));
        EventManagerConfig cfg;
        cfg.wait = wait;
        cfg.spin_budget = 10us;      // tiny: cross the arm/block seam often
        EventManager em(cfg);

        std::atomic<std::uint64_t> delivered{0};
        std::atomic<std::uint64_t> post_failures{0};
        std::atomic<bool> go{false};

        std::thread loop([&] { em.run(); });

        std::vector<std::thread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&, p] {
                while (!go.load(std::memory_order_acquire)) {}
                std::mt19937 rng(0xBEEF + p);
                for (int i = 0; i < kMsgs; ++i) {
                    // Occasionally pause around the spin budget so the
                    // consumer blocks; mostly burst.
                    if (rng() % 97 == 0)
                        std::this_thread::sleep_for(
                            std::chrono::microseconds(rng() % 400));
                    // Bounded ring: Full is legitimate backpressure — retry.
                    PostResult r;
                    do {
                        r = em.mailbox().post([&] {
                            delivered.fetch_add(1, std::memory_order_relaxed);
                        });
                    } while (r == PostResult::Full);
                    if (r != PostResult::Ok)
                        post_failures.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        go.store(true, std::memory_order_release);

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(30 * scale + 30);
        while (delivered.load() < std::uint64_t(kProducers) * kMsgs &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(2ms);

        for (auto& t : producers) t.join();
        CHECK(post_failures.load() == 0);
        CHECK(delivered.load() == std::uint64_t(kProducers) * kMsgs);

        // Liveness: a final post after the storm must also run — the loop
        // may not be asleep holding a non-empty queue.
        std::atomic<bool> tail{false};
        (void)em.post([&] { tail = true; });
        const auto tail_deadline = std::chrono::steady_clock::now() + 5s;
        while (!tail.load() && std::chrono::steady_clock::now() < tail_deadline)
            std::this_thread::sleep_for(1ms);
        CHECK(tail.load());

        em.stop();
        loop.join();

        CHECK(em.stats().mailbox_pops ==
              std::uint64_t(kProducers) * kMsgs + 1);
    }
}

TEST_CASE("stress: MPSC ring preserves per-producer order, no loss") {
    const int scale = stress_scale();
    constexpr int kProducers = 4;
    constexpr std::size_t kCap = 256;
    const int kMsgs = 50'000 * scale;

    MpscRing<std::uint64_t> ring(kCap);
    std::atomic<bool> go{false};
    std::atomic<std::uint64_t> popped{0};

    // Values encode (producer << 48) | seq so per-producer order is checkable.
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            while (!go.load(std::memory_order_acquire)) {}
            for (std::uint64_t i = 0; i < std::uint64_t(kMsgs); ++i) {
                std::uint64_t v = (std::uint64_t(p) << 48) | i;
                while (!ring.try_push(v))
                    std::this_thread::yield();
            }
        });
    }

    std::vector<std::uint64_t> last(kProducers, ~0ULL);
    std::vector<char> seen_started(kProducers, 0);
    std::uint64_t expected_total = std::uint64_t(kProducers) * kMsgs;
    std::atomic<std::uint64_t> order_violations{0};

    go.store(true, std::memory_order_release);

    std::thread consumer([&] {
        std::uint64_t v;
        std::uint64_t got = 0;
        while (got < expected_total) {
            if (!ring.try_pop(v)) { std::this_thread::yield(); continue; }
            ++got;
            int p = int(v >> 48);
            std::uint64_t seq = v & ((1ULL << 48) - 1);
            // Per-producer sequences must be strictly increasing.
            std::uint64_t want = seen_started[p] ? last[p] + 1 : 0;
            if (seq != want)
                order_violations.fetch_add(1, std::memory_order_relaxed);
            seen_started[p] = 1;
            last[p] = seq;
        }
        popped = got;
    });

    for (auto& t : producers) t.join();
    consumer.join();
    CHECK(order_violations.load() == 0);
    CHECK(popped == expected_total);
    for (int p = 0; p < kProducers; ++p)
        CHECK(last[p] == std::uint64_t(kMsgs) - 1);
}

TEST_CASE("stress: SPSC ring cross-thread ordering and completeness") {
    const int scale = stress_scale();
    constexpr std::size_t kCap = 1024;
    const std::uint64_t kMsgs = std::uint64_t(500'000) * scale;

    SpscRing<std::uint64_t> ring(kCap);
    std::atomic<bool> go{false};

    std::thread prod([&] {
        while (!go.load(std::memory_order_acquire)) {}
        for (std::uint64_t i = 0; i < kMsgs; ++i)
            while (!ring.try_push(i)) std::this_thread::yield();
    });

    go.store(true, std::memory_order_release);

    std::uint64_t expect = 0;
    std::atomic<std::uint64_t> order_violations{0};
    while (expect < kMsgs)
        ring.drain(
            [&](std::uint64_t v) {
                if (v != expect)
                    order_violations.fetch_add(1, std::memory_order_relaxed);
                ++expect;
            },
            4096);
    prod.join();
    CHECK(order_violations.load() == 0);
    CHECK(expect == kMsgs);
}

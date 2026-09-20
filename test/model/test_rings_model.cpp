#include <doctest/doctest.h>

#include <cstdint>
#include <deque>
#include <random>
#include <vector>

#include "../model_scale.hpp"
#include "afx/itc/mpsc_ring.hpp"
#include "afx/itc/spsc_ring.hpp"

using namespace afx;

// Model-based tests (IMPLEMENTATION_PLAN.md §4): hammer the rings with seeded
// random operation sequences and check every observable behaviour against a
// std::deque reference. Small capacity so wrap-around and batch-claim
// boundaries are hit constantly. Concurrency correctness (per-producer
// ordering under real contention) lives in test/stress — this file is about
// the state machine: full/empty boundaries, wrap-around, partial bulk ops.

namespace {

std::mt19937_64 rng_for(const char* name, std::uint64_t seed) {
    MESSAGE("model seed for ", name, ": ", seed);
    return std::mt19937_64(seed);
}

}  // namespace

TEST_CASE("model: SpscRing matches std::deque over random op sequences") {
    constexpr std::size_t kCap = 64;
    constexpr std::uint64_t kSeeds[] = {1, 42, 0xC0FFEE};

    for (std::uint64_t seed : kSeeds) {
        CAPTURE(seed);
        auto rng = rng_for("spsc", seed);
        SpscRing<std::uint64_t> ring(kCap);
        std::deque<std::uint64_t> model;
        std::uint64_t next_val = 0;

        const std::uint64_t steps =
            std::uint64_t(2'000'000 * afx::test::model_scale());
        for (std::uint64_t step = 0; step < steps; ++step) {
            switch (rng() % 4) {
                case 0: {  // single push
                    std::uint64_t v = next_val;
                    bool ok = ring.try_push(v);
                    CHECK(ok == (model.size() < kCap));
                    if (ok) model.push_back(v), ++next_val;
                    break;
                }
                case 1: {  // bulk push of a random count
                    std::size_t want = rng() % (kCap + 8);
                    std::vector<std::uint64_t> batch(want);
                    for (auto& v : batch) v = next_val++;
                    std::size_t n = ring.try_push_bulk(batch);
                    CHECK(n == std::min(want, kCap - model.size()));
                    for (std::size_t i = 0; i < n; ++i)
                        model.push_back(batch[i]);
                    next_val -= want - n;  // unpushed values aren't consumed
                    break;
                }
                case 2: {  // drain up to random max
                    std::size_t max = rng() % (kCap + 8);
                    std::size_t expected = std::min(max, model.size());
                    std::size_t got = ring.drain(
                        [&](std::uint64_t v) {
                            CHECK(v == model.front());
                            model.pop_front();
                        },
                        max);
                    CHECK(got == expected);
                    break;
                }
                case 3: {  // contiguous-claim drain
                    std::size_t max = rng() % (kCap + 8);
                    std::size_t before = model.size();
                    ring.drain_contiguous(
                        [&](std::span<std::uint64_t> s) {
                            CHECK(s.size() <= max);
                            for (std::uint64_t v : s) {
                                CHECK(v == model.front());
                                model.pop_front();
                            }
                        },
                        max);
                    // Progress whenever items were available and max allowed
                    // it.
                    if (before > 0 && max > 0) CHECK(model.size() < before);
                    break;
                }
            }
            CHECK(ring.empty() == model.empty());
            CHECK(ring.size_approx() == model.size());
        }
    }
}

TEST_CASE("model: MpscRing matches std::deque over random op sequences") {
    constexpr std::size_t kCap = 64;
    constexpr std::uint64_t kSeeds[] = {7, 1337, 0xBADC0DE};

    for (std::uint64_t seed : kSeeds) {
        CAPTURE(seed);
        auto rng = rng_for("mpsc", seed);
        MpscRing<std::uint64_t> ring(kCap);
        std::deque<std::uint64_t> model;
        std::uint64_t next_val = 0;

        // Single-threaded interleaving: several logical "producers" each with
        // a monotonic sequence; the model still sees a strict FIFO.
        const std::uint64_t steps =
            std::uint64_t(2'000'000 * afx::test::model_scale());
        for (std::uint64_t step = 0; step < steps; ++step) {
            switch (rng() % 3) {
                case 0: {
                    std::uint64_t v = next_val;
                    bool ok = ring.try_push(v);
                    CHECK(ok == (model.size() < kCap));
                    if (ok) model.push_back(v), ++next_val;
                    break;
                }
                case 1: {
                    std::size_t want = rng() % (kCap + 8);
                    std::vector<std::uint64_t> batch(want);
                    for (auto& v : batch) v = next_val++;
                    std::size_t n = ring.try_push_bulk(batch);
                    CHECK(n == std::min(want, kCap - model.size()));
                    for (std::size_t i = 0; i < n; ++i)
                        model.push_back(batch[i]);
                    next_val -= want - n;
                    break;
                }
                case 2: {
                    std::size_t max = rng() % (kCap + 8);
                    std::size_t expected = std::min(max, model.size());
                    std::size_t got = ring.drain(
                        [&](std::uint64_t v) {
                            CHECK(v == model.front());
                            model.pop_front();
                        },
                        max);
                    CHECK(got == expected);
                    break;
                }
            }
            CHECK(ring.empty() == model.empty());
        }
    }
}

TEST_CASE("model: ring indices survive many wrap-arounds") {
    // Fill/drain alternately so head/tail lap the small buffer many times;
    // monotone values make any slot aliasing visible.
    SpscRing<std::uint64_t> ring(8);
    std::uint64_t push_v = 0, pop_v = 0;
    for (int lap = 0; lap < 200'000; ++lap) {
        for (int i = 0; i < 8; ++i) REQUIRE(ring.try_push(push_v++));
        CHECK(!ring.try_push(push_v));  // genuinely full
        for (int i = 0; i < 8; ++i) {
            std::uint64_t seen = ~std::uint64_t(0);
            ring.drain([&](std::uint64_t v) { seen = v; }, 1);
            CHECK(seen == pop_v++);
        }
        CHECK(ring.empty());
    }
}

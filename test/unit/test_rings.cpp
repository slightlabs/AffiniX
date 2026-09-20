#include <doctest/doctest.h>

#include <algorithm>
#include <deque>
#include <random>
#include <thread>

#include "afx/itc/mpsc_ring.hpp"
#include "afx/itc/spsc_ring.hpp"

using namespace afx;

TEST_CASE("SpscRing basic FIFO") {
    SpscRing<int> r(8);
    for (int i = 0; i < 5; ++i) CHECK(r.try_push(i));
    std::vector<int> out;
    r.drain([&](int v) { out.push_back(v); }, 16);
    CHECK(out == std::vector<int>{0, 1, 2, 3, 4});
}

TEST_CASE("SpscRing is bounded") {
    SpscRing<int> r(4);
    for (int i = 0; i < 4; ++i) CHECK(r.try_push(i));
    CHECK(!r.try_push(4));  // full: no silent growth
    int v;
    CHECK(r.drain([&](int x) { v = x; }, 1) == 1);
    CHECK(v == 0);
    CHECK(r.try_push(4));
}

TEST_CASE("SpscRing contiguous batch drain") {
    SpscRing<int> r(16);
    for (int i = 0; i < 10; ++i) r.try_push(i);
    std::vector<int> out;
    r.drain_contiguous(
        [&](std::span<int> s) { out.insert(out.end(), s.begin(), s.end()); },
        16);
    CHECK(out.size() == 10);
    CHECK(out.front() == 0);
    CHECK(out.back() == 9);
}

TEST_CASE("SpscRing bulk push partial on full") {
    SpscRing<int> r(4);
    std::vector<int> in{1, 2, 3, 4, 5, 6};
    CHECK(r.try_push_bulk(in) == 4);
    CHECK(r.try_push_bulk(in) == 0);
}

TEST_CASE("MpscRing single-threaded FIFO") {
    MpscRing<int> r(8);
    for (int i = 0; i < 6; ++i) CHECK(r.try_push(i));
    int v;
    for (int i = 0; i < 6; ++i) {
        CHECK(r.try_pop(v));
        CHECK(v == i);
    }
    CHECK(!r.try_pop(v));
}

TEST_CASE("MpscRing bounded") {
    MpscRing<int> r(4);
    for (int i = 0; i < 4; ++i) CHECK(r.try_push(i));
    CHECK(!r.try_push(99));
}

// ---- model-based: random ops vs a std::deque reference (test taxonomy §4) --
TEST_CASE("SpscRing model test vs std::deque") {
    std::mt19937_64 rng(0xA551);
    for (int round = 0; round < 200; ++round) {
        SpscRing<int> r(16);
        std::deque<int> model;
        for (int op = 0; op < 2000; ++op) {
            if (rng() % 2) {
                int v = int(rng());
                bool pushed = r.try_push(v);
                CHECK(pushed == (model.size() < 16));
                if (pushed) model.push_back(v);
            } else {
                std::vector<int> got;
                std::size_t want = rng() % 5;
                r.drain([&](int v) { got.push_back(v); }, want);
                std::size_t expect = std::min(want, model.size());
                CHECK(got.size() == expect);
                for (std::size_t i = 0; i < got.size(); ++i) {
                    CHECK(got[i] == model.front());
                    model.pop_front();
                }
            }
        }
    }
}

TEST_CASE("MpscRing model test vs std::deque") {
    std::mt19937_64 rng(0xBEEF);
    for (int round = 0; round < 200; ++round) {
        MpscRing<int> r(16);
        std::deque<int> model;
        for (int op = 0; op < 2000; ++op) {
            if (rng() % 2) {
                int v = int(rng());
                bool pushed = r.try_push(v);
                CHECK(pushed == (model.size() < 16));
                if (pushed) model.push_back(v);
            } else {
                int v;
                bool got = r.try_pop(v);
                CHECK(got == !model.empty());
                if (got) {
                    CHECK(v == model.front());
                    model.pop_front();
                }
            }
        }
    }
}

// ---- threaded smoke: correctness across the producer/consumer boundary ----
TEST_CASE("SpscRing threaded: all items delivered exactly once") {
    constexpr int N = 200'000;
    SpscRing<int> r(1024);
    std::atomic<bool> done{false};
    std::atomic<long long> sum{0};

    std::thread prod([&] {
        for (int i = 1; i <= N; ++i)
            while (!r.try_push(i)) {}
        done = true;
    });
    long long got = 0;
    int expected_next = 1;
    while (!done || !r.empty()) {
        r.drain(
            [&](int v) {
                CHECK(v == expected_next++);
                ++got;
            },
            1024);
    }
    prod.join();
    CHECK(got == N);
}

TEST_CASE("MpscRing threaded: 4 producers, all items delivered") {
    constexpr int N = 50'000;
    MpscRing<int> r(1024);
    std::atomic<int> live{4};
    std::vector<std::atomic<int>> seen(4 * N);
    std::atomic<long long> total{0};

    std::vector<std::thread> prods;
    for (int p = 0; p < 4; ++p)
        prods.emplace_back([&, p] {
            for (int i = 0; i < N; ++i) {
                while (!r.try_push(p * N + i)) {}
            }
            live.fetch_sub(1);
        });

    int got = 0;
    while (live.load() || got < 4 * N) {
        int v;
        while (r.try_pop(v)) {
            seen[v].fetch_add(1);
            ++got;
        }
    }
    for (auto& t : prods) t.join();
    CHECK(got == 4 * N);
    for (auto& s : seen) CHECK(s.load() == 1);
}

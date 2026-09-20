// bench/itc_pingpong (M3-13): mailbox round-trip latency and post cost.
// "Round-trip" = post a task to the EM, it replies onto a raw MpscRing the
// caller drains — one full traverse of the arm/block wake path.

#include <atomic>
#include <chrono>
#include <thread>

#include "afx/itc/mpsc_ring.hpp"
#include "bench_env.hpp"

using namespace afx;

int main() {
    using namespace std::chrono;

    // ---- one-way post -> execute latency ----------------------------------
    {
        EventManager em(EventManagerConfig{.wait = WaitStrategy::Block});
        std::thread t([&] { em.run(); });
        std::this_thread::sleep_for(5ms);  // let it reach the blocked wait

        MpscRing<std::uint64_t> reply(1024);
        std::atomic<std::uint64_t> seq{0};

        auto b = afx::bench::make_bench();
        const auto spin_deadline = seconds(60);  // generous: a missed reply
                                                 // means a lost wake — fail,
                                                 // don't hang CI for 300s
        b.run("mailbox round-trip (post -> run -> reply)", [&] {
            std::uint64_t want = seq.fetch_add(1) + 1;
            if (em.post([&] { (void)reply.try_push(want); }) !=
                PostResult::Ok) {
                std::fprintf(stderr, "itc_pingpong: post dropped\n");
                std::abort();
            }
            std::uint64_t v = 0;
            auto t0 = steady_clock::now();
            while (!reply.try_pop(v) || v != want) {  // spin-drain reply
                if (steady_clock::now() - t0 > spin_deadline) {
                    std::fprintf(stderr, "itc_pingpong: reply never arrived\n");
                    std::abort();
                }
            }
        });
        em.stop();
        t.join();
    }

    // ---- raw post throughput (no wait for delivery) ------------------------
    {
        EventManager em(EventManagerConfig{.wait = WaitStrategy::Spin});
        std::thread t([&] { em.run(); });

        auto b = afx::bench::make_bench();
        std::uint64_t n = 0;
        b.run("mailbox post() call", [&] {
            (void)em.post([&] { ++n; });
            ankerl::nanobench::doNotOptimizeAway(n);
        });
        em.stop();
        t.join();
    }
}

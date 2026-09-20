// bench/channel_throughput (M3-13): channel push/drain throughput for SPSC
// and MPSC kinds, plus the same workload on the bare rings for comparison
// (this doubles as open-question 1's hand-rolled-vs-reference measurement
// hook — swap the ring impl and rerun).

#include <cstdint>
#include <span>
#include <vector>

#include "afx/itc/channel.hpp"
#include "bench_env.hpp"

using namespace afx;

int main() {
    std::uint64_t sink = 0;

    for (ChannelKind kind : {ChannelKind::SPSC, ChannelKind::MPSC}) {
        const char* name = kind == ChannelKind::SPSC ? "spsc" : "mpsc";
        auto [tx, rx] = channel<std::uint64_t>(4096, kind);

        // Single-item push + drain (single-threaded: measures the machinery,
        // not contention).
        {
            char label[64];
            std::snprintf(label, sizeof label, "channel<%s> push+drain x1",
                          name);
            auto b = afx::bench::make_bench();
            b.batch(1).run(label, [&] {
                tx.try_push(1);
                rx.poll();
            });
        }
        // Bulk push + batch drain.
        {
            std::vector<std::uint64_t> batch(256, 7);
            char label[64];
            std::snprintf(label, sizeof label, "channel<%s> push+drain x256",
                          name);
            auto b = afx::bench::make_bench();
            b.batch(256).run(label, [&] {
                tx.try_push_bulk(batch);
                rx.poll();
            });
        }
    }

    // Bare-ring comparison: what the channel machinery adds on top.
    {
        SpscRing<std::uint64_t> ring(4096);
        auto b = afx::bench::make_bench();
        b.batch(1).run("raw SpscRing push+drain x1", [&] {
            ring.try_push(1);
            ring.drain([&](std::uint64_t v) { sink += v; }, 1);
        });
    }

    ankerl::nanobench::doNotOptimizeAway(sink);
}

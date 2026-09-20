// bench/timer_ops (M4-11): arm+cancel cost and fire-storm dispatch cost,
// on a VirtualClock EM so the numbers reflect the machinery, not sleeping.

#include <cstdint>
#include <vector>

#include "bench_env.hpp"
#include "../test/test_env.hpp"

using namespace afx;

int main() {
    // ---- arm + cancel ------------------------------------------------------
    {
        afx::test::TestEnv env;
        std::vector<TimerId> ids;
        ids.reserve(4096);
        auto b = afx::bench::make_bench();
        b.run("timer arm+cancel", [&] {
            TimerId id = env.em.after(1h, [](TimerCtx) {});
            env.em.cancel(id);
            env.pump();
        });
    }

    // ---- fire storm: N one-shots due in the same tick ----------------------
    {
        afx::test::TestEnv env;
        constexpr int kN = 10000;
        auto b = afx::bench::make_bench();
        b.batch(kN).run("timer fire storm (per fire)", [&] {
            for (int i = 0; i < kN; ++i)
                env.em.after(1ms, [](TimerCtx) {});
            env.advance(1ms);            // one expiry-stage pass fires all
        });
    }

    // ---- group cancel ------------------------------------------------------
    {
        afx::test::TestEnv env;
        auto b = afx::bench::make_bench();
        constexpr int kN = 1024;
        b.batch(kN).run("cancel_group (per member)", [&] {
            TimerGroup g = env.em.make_timer_group();
            for (int i = 0; i < kN; ++i)
                env.em.after(1h, [](TimerCtx) {}, g);
            env.em.cancel_group(g);
            env.pump();
        });
    }
}

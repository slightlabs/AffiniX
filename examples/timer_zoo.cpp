// Timer zoo: one-shot, precise at(), fixed-rate and fixed-delay repeats,
// groups, and cancel-in-callback. DESIGN.md §10 examples.

#include <cstdio>

#include "afx/afx.hpp"

using namespace afx;

int main() {
    EventManager em(EventManagerConfig{.wait = WaitStrategy::Block});

    em.after(100ms, [](TimerCtx) { std::puts("after 100ms"); });

    em.every(
        50ms,
        [](TimerCtx c) { std::printf("every 50ms (missed=%u)\n", c.missed); },
        Duration::zero(), RepeatMode::FixedRate);

    auto g = em.make_timer_group();
    em.after(10ms, [](TimerCtx) { std::puts("group t1"); }, g);
    em.after(20ms, [](TimerCtx) { std::puts("group t2"); }, g);
    em.after(
        30ms,
        [g, &em](TimerCtx) {
            std::puts("cancelling group (t4 never fires)");
            em.cancel_group(g);
        },
        g);
    em.after(40ms, [](TimerCtx) { std::puts("group t4"); }, g);

    // Stop after 300 ms.
    em.after(300ms, [&em](TimerCtx) { em.stop(); });
    em.run();
    std::puts("done");
    return 0;
}

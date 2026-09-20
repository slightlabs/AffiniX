#include <doctest/doctest.h>

#include <vector>

#include "../test_env.hpp"
#include "afx/core/event_manager.hpp"

using namespace afx;
using afx::test::TestEnv;

// Regression coverage for two bugs found in the timer/group lifecycle:
//
//  1. A one-shot timer that fired naturally was never unlinked from its
//     TimerGroup's intrusive list. Its HandleTable slot was then recycled for
//     an unrelated timer, and the stale link corrupted the group's list — a
//     later cancel_group() looped forever growing dead_timers_ until the
//     process OOM'd.
//  2. HandleTable slots lived in a std::vector, so growth relocated nodes
//     that wheel/group intrusive lists still pointed at.
//
// With either bug present, the first two cases below hang or corrupt state.

TEST_CASE("timer group: firing a member does not corrupt the group") {
    TestEnv env;
    TimerGroup g = env.em.make_timer_group();

    int fired = 0;
    env.em.after(1ms, [&](TimerCtx) { ++fired; }, g);

    env.advance(1ms);            // fires; slot queued for reclamation
    env.pump();                  // bookkeeping: reclaim() recycles the slot

    CHECK(fired == 1);
    // The group must be empty now; cancel_group must terminate instantly.
    CHECK(env.em.cancel_group(g) == 0);
}

TEST_CASE("timer group: recycled slot does not alias into the old group") {
    TestEnv env;
    TimerGroup g1 = env.em.make_timer_group();
    TimerGroup g2 = env.em.make_timer_group();

    int a_fired = 0, b_fired = 0;
    env.em.after(1ms, [&](TimerCtx) { ++a_fired; }, g1);   // slot S
    env.advance(1ms);
    env.pump();                          // S is reclaimed to the free list

    // Arm enough timers that S is handed out again — in a different group.
    std::vector<TimerId> ids;
    for (int i = 0; i < 8; ++i)
        ids.push_back(env.em.after(1h, [&](TimerCtx) { ++b_fired; }, g2));

    // Cancelling the dead group must not touch the recycled slot's owner.
    CHECK(env.em.cancel_group(g1) == 0);
    CHECK(env.em.cancel_group(g2) == 8);

    // Advance past every deadline: nothing may fire (all cancelled).
    env.advance(2h);
    env.pump();
    CHECK(a_fired == 1);
    CHECK(b_fired == 0);
}

TEST_CASE("timer group: cancel_group survives table growth") {
    TestEnv env;
    TimerGroup g = env.em.make_timer_group();

    // Arm far more timers than the handle table's initial capacity; slot
    // addresses must remain stable or the intrusive group list corrupts.
    constexpr int kN = 2000;
    for (int i = 0; i < kN; ++i)
        env.em.after(Duration(1h + Nanos(i)), [](TimerCtx) {}, g);

    CHECK(env.em.cancel_group(g) == kN);
    CHECK(env.em.cancel_group(g) == 0);    // idempotent on an empty group
}

TEST_CASE("timer: cancel of a stale id is a harmless no-op") {
    TestEnv env;
    TimerId id = env.em.after(1ms, [](TimerCtx) {});
    CHECK(env.em.cancel(id));
    env.pump();                            // reclaim the slot

    // Arm + fire an unrelated timer so the slot is reused and the stale
    // generation is thoroughly dead.
    env.em.after(1ms, [](TimerCtx) {});
    env.advance(2ms);

    CHECK(!env.em.cancel(id));             // stale handle: no-op, no crash
    CHECK(env.em.time_until(id) == std::nullopt);
}

TEST_CASE("timer: self-cancel inside the callback works") {
    TestEnv env;
    TimerId self{};
    int fired = 0;
    self = env.em.every(1ms, [&](TimerCtx) {
        ++fired;
        env.em.cancel(self);
    });
    env.advance(1ms);
    env.advance(5ms);
    CHECK(fired == 1);                     // fired once, then dead
}

TEST_CASE("timer: cancel of a pending member removes it from the group") {
    TestEnv env;
    TimerGroup g = env.em.make_timer_group();
    TimerId keep = env.em.after(1h, [](TimerCtx) {}, g);
    TimerId drop = env.em.after(1h, [](TimerCtx) {}, g);
    CHECK(env.em.cancel(drop));
    // Only the surviving member is left for the group cancel.
    CHECK(env.em.cancel_group(g) == 1);
    (void)keep;
}

TEST_CASE("timer group: reschedule keeps group membership") {
    // Regression: unlink_timer() once removed a rescheduled node from its
    // group's intrusive list too, so a rescheduled member survived
    // cancel_group() and kept firing (caught by the timer model test).
    TestEnv env;
    TimerGroup g = env.em.make_timer_group();

    int fired = 0;
    TimerId id = env.em.every(1ms, [&](TimerCtx) { ++fired; },
                              Duration::zero(), RepeatMode::FixedRate, g);
    for (int i = 0; i < 3; ++i) env.advance(1ms);   // one coalesced fire/pump
    REQUIRE(fired == 3);

    env.em.reschedule(id, 1h);             // reinsert: still a group member
    CHECK(env.em.cancel_group(g) == 1);    // must find and cancel it

    env.advance(2h);
    env.pump();
    CHECK(fired == 3);                     // no further fires
}

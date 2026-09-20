#include <doctest/doctest.h>

#include <map>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

#include "../test_env.hpp"
#include "afx/core/timer_wheel.hpp"

using namespace afx;
using afx::test::TestEnv;

// ---- direct wheel/heap behaviour -------------------------------------------

TEST_CASE("TimerWheel fires at the containing tick, never early") {
    TimerWheel w(1ms);
    std::vector<TimerNode> nodes(4);
    std::vector<int> fired;

    auto arm = [&](int i, TimePoint at) {
        nodes[i].expiry = at;
        nodes[i].expiry_tick = w.tick_of(at);
        w.insert(nodes[i], w.now_tick());
    };
    auto fire = [&](TimerNode& n) {
        fired.push_back(int(&n - nodes.data()));
    };

    arm(0, TimePoint(1500us));   // tick 2 (ceil of 1.5 ms)
    arm(1, TimePoint(2ms));      // tick 2
    arm(2, TimePoint(300ms));    // level-1 slot
    arm(3, TimePoint(1us));      // tick 1

    w.advance(w.floor_tick(TimePoint(1ms)), fire);
    CHECK(fired == std::vector<int>{3});
    fired.clear();

    w.advance(w.floor_tick(TimePoint(2ms)), fire);
    CHECK(std::set<int>(fired.begin(), fired.end()) == std::set<int>{0, 1});
    fired.clear();

    w.advance(w.floor_tick(TimePoint(299ms)), fire);
    CHECK(fired.empty());
    w.advance(w.floor_tick(TimePoint(300ms)), fire);
    CHECK(fired == std::vector<int>{2});
}

TEST_CASE("TimerWheel multi-level cascade and long horizon") {
    TimerWheel w(1ms);
    std::vector<TimerNode> nodes(3);
    std::vector<int> fired;
    auto arm = [&](int i, TimePoint at) {
        nodes[i].expiry = at;
        nodes[i].expiry_tick = w.tick_of(at);
        w.insert(nodes[i], w.now_tick());
    };
    auto fire = [&](TimerNode& n) { fired.push_back(int(&n - nodes.data())); };

    arm(0, TimePoint(70s));     // level 2 territory (256 ms per L1 slot)
    arm(1, TimePoint(20min));   // level 3
    arm(2, TimePoint(256ms));   // first tick of level 1's second slot

    auto advance_to = [&](TimePoint t) {
        std::uint64_t target = w.floor_tick(t);
        while (w.now_tick() < target) w.advance(target, fire);
    };

    advance_to(TimePoint(300ms));
    CHECK(fired == std::vector<int>{2});
    fired.clear();

    advance_to(TimePoint(70s));
    CHECK(fired == std::vector<int>{0});
    fired.clear();

    advance_to(TimePoint(20min));
    CHECK(fired == std::vector<int>{1});
}

TEST_CASE("TimerWheel overdue insert fires on the next advance step") {
    // A node armed with expiry_tick <= now_tick has no meaningful residue in
    // the current rotation; it must not sit in a stale slot for up to 256
    // ticks. Regression coverage for the clamp in TimerWheel::insert.
    TimerWheel w(1ms);
    w.advance(w.floor_tick(TimePoint(200ms)), [](TimerNode&) {});

    TimerNode n;
    n.expiry = TimePoint(50ms);             // already in the past
    n.expiry_tick = w.tick_of(n.expiry);    // tick 50 < now_tick 200
    w.insert(n, w.now_tick());

    int fired = 0;
    w.advance(w.now_tick() + 1, [&](TimerNode& m) { if (&m == &n) ++fired; });
    CHECK(fired == 1);
}

TEST_CASE("TimerWheel cascade onto the current tick fires that step") {
    // A level-1 node whose expiry sits exactly on the boundary tick it
    // cascades at must land in the *current* level-0 slot — the sweep
    // drains it later in the same step. Regression: an overdue-insert
    // clamp once pushed such nodes to the next slot, so a timer at a
    // 256-tick boundary fired one pump late (caught by the model test).
    TimerWheel w(1ms);
    TimerNode n;
    n.expiry = TimePoint(512ms);           // tick 512: level-1 digit boundary
    n.expiry_tick = w.tick_of(n.expiry);
    w.insert(n, 0);                        // level 1, digit-1 slot 2

    int fired = 0;
    auto fire = [&](TimerNode& m) { if (&m == &n) ++fired; };
    w.advance(511, fire);
    CHECK(fired == 0);                     // not early
    w.advance(512, fire);
    CHECK(fired == 1);                     // fires the step it cascades in
}

TEST_CASE("TimerWheel unlink is O(1) and idempotent") {
    TimerWheel w(1ms);
    TimerNode n;
    n.expiry = TimePoint(10ms);
    n.expiry_tick = w.tick_of(n.expiry);
    w.insert(n, 0);
    CHECK(w.size() == 1);
    w.unlink(n);
    w.unlink(n);                 // safe to unlink twice
    CHECK(w.size() == 0);
    int fired = 0;
    w.advance(w.floor_tick(TimePoint(100ms)), [&](TimerNode&) { ++fired; });
    CHECK(fired == 0);
}

TEST_CASE("TimerHeap orders arbitrary deadlines") {
    TimerHeap h;
    std::vector<TimerNode> nodes(5);
    std::vector<TimePoint> times = {TimePoint(5ms), TimePoint(1ms),
                                    TimePoint(9ms), TimePoint(3ms),
                                    TimePoint(7ms)};
    for (int i = 0; i < 5; ++i) {
        nodes[i].expiry = times[i];
        h.push(nodes[i]);
    }
    std::vector<TimePoint> out;
    h.expire(TimePoint(100ms), [&](TimerNode& n) { out.push_back(n.expiry); });
    CHECK(out == std::vector<TimePoint>{TimePoint(1ms), TimePoint(3ms),
                                        TimePoint(5ms), TimePoint(7ms),
                                        TimePoint(9ms)});
}

TEST_CASE("TimerHeap removal mid-heap") {
    TimerHeap h;
    std::vector<TimerNode> nodes(4);
    for (int i = 0; i < 4; ++i) {
        nodes[i].expiry = TimePoint(Nanos((i + 1) * 1000));
        h.push(nodes[i]);
    }
    h.remove(nodes[1]);
    h.remove(nodes[1]);          // idempotent
    std::vector<TimePoint> out;
    h.expire(TimePoint(1s), [&](TimerNode& n) { out.push_back(n.expiry); });
    CHECK(out.size() == 3);
}

// Model-based: random arm/cancel/advance against a std::multimap reference.
TEST_CASE("TimerWheel model test vs multimap") {
    std::mt19937_64 rng(12345);
    TimerWheel w(1ms);
    std::vector<std::unique_ptr<TimerNode>> nodes;
    std::unordered_map<TimerNode*, int> ids;
    std::map<std::pair<std::uint64_t, int>, TimerNode*> model;  // (tick,id)->node
    std::uint64_t now = 0;

    for (int round = 0; round < 300; ++round) {
        int op = int(rng() % 100);
        if (op < 55) {
            // arm
            auto n = std::make_unique<TimerNode>();
            std::uint64_t delay = rng() % (300 * 1000);   // up to 300 s
            n->expiry = TimePoint(Nanos(now * 1'000'000) + Nanos(delay * 1'000'000));
            n->expiry_tick = w.tick_of(n->expiry);
            int id = int(nodes.size());
            if (!w.fits(n->expiry_tick, w.now_tick())) continue;
            w.insert(*n, w.now_tick());
            ids[n.get()] = id;
            nodes.push_back(std::move(n));
            model[{nodes.back()->expiry_tick, id}] = nodes.back().get();
        } else if (op < 75) {
            // cancel a random live node
            if (model.empty()) continue;
            auto it = model.begin();
            std::advance(it, rng() % model.size());
            TimerNode* n = it->second;
            w.unlink(*n);
            for (auto mit = model.begin(); mit != model.end();) {
                if (mit->second == n) mit = model.erase(mit); else ++mit;
            }
        } else {
            // advance time 1..50 ticks, firing
            std::uint64_t step = 1 + rng() % 50;
            std::multiset<std::pair<std::uint64_t, int>> fired;
            w.advance(now + step, [&](TimerNode& n) {
                fired.emplace(n.expiry_tick, ids[&n]);
            });
            now += step;
            // model: everything with tick <= now fires
            std::multiset<std::pair<std::uint64_t, int>> expected;
            for (auto it = model.begin();
                 it != model.end() && it->first.first <= now;) {
                expected.insert(it->first);
                it = model.erase(it);
            }
            CHECK(fired == expected);
        }
    }
}

// ---- EM-level timer semantics (virtual time) --------------------------------

TEST_CASE("EM timers: after fires once, at is precise") {
    TestEnv env;
    int after_n = 0, at_n = 0;
    env.em.after(10ms, [&](TimerCtx) { ++after_n; });
    env.em.at(TimePoint(1500us), [&](TimerCtx) { ++at_n; });

    env.advance(1ms);
    CHECK(at_n == 0);            // heap timer: waits for its exact deadline
    env.advance(500us);
    env.pump();
    CHECK(at_n == 1);
    CHECK(after_n == 0);
    env.advance(9ms);
    CHECK(after_n == 1);
    env.advance(100ms);
    CHECK(after_n == 1);         // fired once only
}

TEST_CASE("EM timers: every() FixedRate fires per period without drift") {
    TestEnv env;
    int fired = 0;
    std::optional<TimePoint> last_sched;
    env.em.every(1ms, [&](TimerCtx c) {
        ++fired;
        if (last_sched) CHECK(*last_sched + 1ms == c.scheduled);
        last_sched = c.scheduled;
    });
    for (int i = 0; i < 10; ++i) env.advance(1ms);
    CHECK(fired == 10);
}

TEST_CASE("EM timers: FixedRate coalesces overrun and reports missed") {
    TestEnv env;
    int fired = 0;
    std::uint32_t missed = 0;
    env.em.every(1ms, [&](TimerCtx c) { ++fired; missed += c.missed; });
    // Jump 5 periods at once: one callback; slots 1..5 all elapsed,
    // so missed = 4 overruns beyond the delivered one.
    env.em.clock().advance(5ms);
    env.pump();
    CHECK(fired == 1);
    CHECK(missed == 4);
    // Caught up: next period fires normally.
    env.advance(1ms);
    CHECK(fired == 2);
}

TEST_CASE("EM timers: FixedDelay measures from completion") {
    TestEnv env;
    std::vector<TimePoint> fired_at;
    env.em.every(1ms, [&](TimerCtx) { fired_at.push_back(env.em.now()); },
                 Duration::zero(), RepeatMode::FixedDelay);
    env.em.clock().advance(3ms);
    env.pump();
    env.em.clock().advance(3ms);
    env.pump();
    REQUIRE(fired_at.size() >= 1);
    // With FixedDelay, period runs from now, so jumps don't coalesce.
    CHECK(fired_at.size() == 2);
}

TEST_CASE("EM timers: cancel is safe on stale ids") {
    TestEnv env;
    int fired = 0;
    auto id = env.em.after(5ms, [&](TimerCtx) { ++fired; });
    CHECK(env.em.cancel(id));
    CHECK(!env.em.cancel(id));          // already cancelled: no-op
    env.advance(10ms);
    CHECK(fired == 0);
    // The slot may be reused; the stale id must still be a no-op.
    env.em.after(1ms, [](TimerCtx) {});
    env.advance(1ms);
    CHECK(!env.em.cancel(id));
}

TEST_CASE("EM timers: cancel-from-callback including self") {
    TestEnv env;
    int fired = 0;
    TimerId self{};
    env.em.after(1ms, [&](TimerCtx c) {
        ++fired;
        self = c.id;
        env.em.cancel(c.id);            // legal: self-cancel in callback
    });
    env.advance(2ms);
    CHECK(fired == 1);
    env.advance(10ms);                  // must not fire again / crash
    CHECK(fired == 1);
}

TEST_CASE("EM timers: repeating self-cancel stops future fires") {
    TestEnv env;
    int fired = 0;
    env.em.every(1ms, [&](TimerCtx c) {
        if (++fired == 3) env.em.cancel(c.id);
    });
    for (int i = 0; i < 10; ++i) env.advance(1ms);
    CHECK(fired == 3);
}

TEST_CASE("EM timers: TimerGroup cancels all members at once") {
    TestEnv env;
    int fired = 0;
    auto g = env.em.make_timer_group();
    env.em.after(1ms,  [&](TimerCtx) { ++fired; }, g);
    env.em.after(2ms,  [&](TimerCtx) { ++fired; }, g);
    env.em.every(3ms,  [&](TimerCtx) { ++fired; }, Duration::zero(),
                 RepeatMode::FixedRate, g);
    env.em.after(1ms,  [&](TimerCtx) { ++fired; });   // not in group
    CHECK(env.em.cancel_group(g) == 3);
    env.advance(10ms);
    CHECK(fired == 1);                 // only the ungrouped timer fired
}

TEST_CASE("EM timers: time_until and reschedule") {
    TestEnv env;
    int fired = 0;
    auto id = env.em.after(10ms, [&](TimerCtx) { ++fired; });
    CHECK(env.em.time_until(id) == std::optional<Duration>(10ms));
    CHECK(env.em.reschedule(id, 2ms));
    env.advance(3ms);
    CHECK(fired == 1);
}

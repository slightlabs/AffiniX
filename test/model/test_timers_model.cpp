#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../model_scale.hpp"
#include "../test_env.hpp"
#include "afx/core/event_manager.hpp"

using namespace afx;
using afx::test::TestEnv;

// Model-based test for the timer subsystem (IMPLEMENTATION_PLAN.md §4,
// M4-10): a naive reference model — "a timer fires iff its effective fire
// time has been reached" — is checked against the real wheel+heap+group
// machinery under VirtualClock over long random operation sequences.
//
// The subtle part is *effective* fire time, which the model must replicate:
//   - wheel timers fire at the tick containing their expiry (ceil), so a
//     timer is never early but can be up to one tick late;
//   - timers beyond the 256^4-tick horizon (~49.7 days at 1 ms) and at()
//     "precise" timers live in the heap and fire at their exact expiry;
//   - FixedRate repeats reschedule to the first slot strictly after now and
//     coalesce missed firings into TimerCtx::missed;
//   - FixedDelay repeats reschedule to now + period.

namespace {

constexpr Nanos kTick = 1ms;                 // TestEnv default timer_tick
constexpr std::uint64_t kTickNs = std::uint64_t(kTick.count());
constexpr std::uint64_t kHorizonTicks = std::uint64_t(1) << 32;  // 256^4

std::uint64_t tick_up(TimePoint t) {
    return std::uint64_t(
        (t.time_since_epoch().count() + std::int64_t(kTickNs) - 1) /
        std::int64_t(kTickNs));
}
std::uint64_t tick_down(TimePoint t) {
    return std::uint64_t(t.time_since_epoch().count() / std::int64_t(kTickNs));
}
bool wheel_fits(std::uint64_t expiry_tick, std::uint64_t now_tick) {
    return expiry_tick <= now_tick ||
           ((expiry_tick - now_tick) >> 32) == 0;
}

struct ModelTimer {
    TimerId    tid{};
    int        tag = -1;
    TimePoint  expiry{};      // next scheduled fire (requested, unrounded)
    TimePoint  effective{};   // when the loop will actually fire it
    Duration   period{};
    RepeatMode mode = RepeatMode::FixedRate;
    bool       precise = false;
    bool       heap = false;  // resident in the heap (precise or >horizon)
    bool       alive = true;
    int        group = -1;    // model group index, -1 = none
};

struct Model {
    TestEnv* env;
    std::unordered_map<int, ModelTimer> timers;
    std::vector<TimerGroup>             groups;   // index = model group id
    std::vector<int>                    live_tags;
    std::vector<int>                    dead_tags;
    int next_tag = 0;

    std::uint64_t now_tick() const { return tick_down(env->em.now()); }
    TimePoint now() const { return env->em.now(); }

    // Where a timer with this expiry/precision lands if inserted now, and
    // the expiry at which the EM will actually fire it.
    bool goes_to_heap(TimePoint expiry, bool precise,
                      std::uint64_t at_tick) const {
        return precise || !wheel_fits(tick_up(expiry), at_tick);
    }
    TimePoint effective_of(TimePoint expiry, bool precise) const {
        if (goes_to_heap(expiry, precise, now_tick())) return expiry;
        return TimePoint(Nanos(std::int64_t(tick_up(expiry) * kTickNs)));
    }

    int arm(TimePoint expiry, Duration period, RepeatMode mode,
            bool precise, int group, TimerFn&& fn) {
        ModelTimer mt;
        mt.tag = next_tag++;
        mt.expiry = expiry;
        mt.period = period;
        mt.mode = mode;
        mt.precise = precise;
        mt.group = group;
        mt.heap = goes_to_heap(expiry, precise, now_tick());
        TimerGroup tg = group >= 0 && group < int(groups.size())
                          ? groups[group] : TimerGroup{};
        if (period.count() > 0)
            mt.tid = env->em.every(period, std::move(fn), expiry - now(),
                                   mode, tg);
        else if (precise)
            mt.tid = env->em.at(expiry, std::move(fn), tg);
        else
            mt.tid = env->em.after(expiry - now(), std::move(fn), tg);
        mt.effective = effective_of(expiry, precise);
        timers.emplace(mt.tag, mt);
        live_tags.push_back(mt.tag);
        char b[192];
        std::snprintf(b, sizeof b,
                      "arm exp=%lld eff=%lld period=%lld precise=%d heap=%d g=%d",
                      (long long)expiry.time_since_epoch().count(),
                      (long long)mt.effective.time_since_epoch().count(),
                      (long long)period.count(), (int)precise, (int)mt.heap,
                      group);
        note(mt.tag, b);
        return mt.tag;
    }

    // Expected firings for a pump ending at `now`: every live timer whose
    // effective time has been reached fires exactly once this iteration
    // (repeats coalesce). Mutates the model the same way fire_timer does.
    void fire_due(TimePoint now, std::vector<int>& expected) {
        for (auto& [tag, mt] : timers) {
            if (!mt.alive || mt.effective > now) continue;
            expected.push_back(tag);
            if (mt.period.count() == 0) {
                mt.alive = false;
                continue;
            }
            if (mt.mode == RepeatMode::FixedRate) {
                TimePoint next = mt.expiry + mt.period;
                while (next <= now) next += mt.period;
                mt.expiry = next;
            } else {   // FixedDelay
                mt.expiry = now + mt.period;
            }
            // Reinsertion is checked against the wheel's position at fire
            // time: for a wheel timer that's the tick it fired on; for a
            // heap timer it's floor(now), because heap_.expire() runs after
            // the wheel has already advanced to the pump's tick.
            std::uint64_t fire_tick = mt.heap ? now_tick()
                                              : tick_down(mt.effective);
            mt.heap = goes_to_heap(mt.expiry, mt.precise, fire_tick);
            mt.effective = mt.heap
                ? mt.expiry
                : TimePoint(Nanos(std::int64_t(tick_up(mt.expiry) * kTickNs)));
            char b[192];
            std::snprintf(b, sizeof b,
                          "fire@now=%lld -> exp=%lld eff=%lld heap=%d",
                          (long long)now.time_since_epoch().count(),
                          (long long)mt.expiry.time_since_epoch().count(),
                          (long long)mt.effective.time_since_epoch().count(),
                          (int)mt.heap);
            note(mt.tag, b);
        }
    }

    // Lifecycle notes for post-mortem dumps on mismatch — bounded: a
    // divergence is always explained by the recent tail, and unbounded
    // notes would dominate RSS over long sweeps.
    static constexpr std::size_t kHistoryCap = 48;
    std::unordered_map<int, std::deque<std::string>> history;
    void note(int tag, const std::string& s) {
        auto& h = history[tag];
        if (h.size() == kHistoryCap) h.pop_front();
        h.push_back(s);
    }

    void cancel_tag(int tag) {
        auto it = timers.find(tag);
        if (it == timers.end()) return;
        it->second.alive = false;
        note(tag, "cancel");
    }
    void cancel_group(int g) {
        for (auto& [tag, mt] : timers)
            if (mt.alive && mt.group == g) {
                mt.alive = false;
                note(mt.tag, "group-cancel g=" + std::to_string(g));
            }
    }
    void sweep_dead() {
        live_tags.erase(std::remove_if(live_tags.begin(), live_tags.end(),
                        [&](int t) { return !timers[t].alive; }),
                        live_tags.end());
    }
};

} // namespace

// Seeds: a fixed regression set plus AFX_MODEL_SEEDS (comma list) for
// nightly sweeps — a failure always reports its seed for reproduction.
std::vector<std::uint64_t> model_seeds() {
    std::vector<std::uint64_t> v{11, 2024, 0xFEEDF};
    if (const char* s = std::getenv("AFX_MODEL_SEEDS")) {
        std::string cur;
        for (const char* p = s; ; ++p) {
            if (*p == ',' || *p == 0) {
                if (!cur.empty()) v.push_back(std::strtoull(cur.c_str(), nullptr, 0));
                cur.clear();
                if (*p == 0) break;
            } else cur += *p;
        }
    }
    return v;
}

TEST_CASE("model: timers match a naive reference under VirtualClock") {
    for (std::uint64_t seed : model_seeds()) {
        CAPTURE(seed);
        std::mt19937_64 rng(seed);
        TestEnv env;
        Model m{&env};

        // Record every actual firing: tag plus the lateness invariant.
        std::vector<int> fired;
        std::unordered_map<int, TimerCtx> fired_ctx;

        auto make_fn = [&](int tag) {
            return TimerFn([&, tag](TimerCtx tc) {
                fired.push_back(tag);
                fired_ctx[tag] = tc;   // keep the latest firing's context
            });
        };

        // A few groups to exercise group cancellation.
        for (int i = 0; i < 4; ++i)
            m.groups.push_back(env.em.make_timer_group());

        auto rand_delay = [&]() -> Duration {
            switch (rng() % 10) {
            case 0: return Nanos(rng() % 900);            // sub-tick
            case 1: return Nanos(kTickNs) * (256 * 256 + rng() % 512); // L2
            case 2: return Duration(24h) + Nanos(rng() % 1000); // >horizon->heap
            default: return Nanos(kTickNs) * (1 + rng() % 3000);
            }
        };

        const int kSteps = int(20000 * afx::test::model_scale());
        for (int step = 0; step < kSteps; ++step) {
            int op = int(rng() % 100);
            if (op < 35) {                       // arm one-shot
                int g = rng() % 6 == 0 ? int(rng() % m.groups.size()) : -1;
                m.arm(m.now() + rand_delay(), Duration{},
                      RepeatMode::FixedRate, false, g, make_fn(m.next_tag));
            } else if (op < 45) {                // arm precise at()
                m.arm(m.now() + rand_delay() - Duration(1ms),
                      Duration{}, RepeatMode::FixedRate, true, -1,
                      make_fn(m.next_tag));
            } else if (op < 60) {                // arm repeating
                Duration p = Nanos(kTickNs) * (1 + rng() % 500);
                auto mode = rng() % 2 ? RepeatMode::FixedRate
                                      : RepeatMode::FixedDelay;
                int g = rng() % 4 == 0 ? int(rng() % m.groups.size()) : -1;
                m.arm(m.now() + p, p, mode, false, g, make_fn(m.next_tag));
            } else if (op < 72) {                // cancel
                if (rng() % 3 && !m.live_tags.empty()) {
                    int tag = m.live_tags[rng() % m.live_tags.size()];
                    // A live model timer must still be cancellable in the EM.
                    CHECK(env.em.cancel(m.timers[tag].tid));
                    m.cancel_tag(tag);
                    m.dead_tags.push_back(tag);   // now a stale id
                } else if (!m.dead_tags.empty()) {
                    int tag = m.dead_tags[rng() % m.dead_tags.size()];
                    CHECK(!env.em.cancel(m.timers[tag].tid));  // stale: no-op
                }
            } else if (op < 78) {                // cancel a group
                int g = int(rng() % m.groups.size());
                env.em.cancel_group(m.groups[g]);
                m.cancel_group(g);
            } else if (op < 88) {                // reschedule a live timer
                if (!m.live_tags.empty()) {
                    int tag = m.live_tags[rng() % m.live_tags.size()];
                    auto& mt = m.timers[tag];
                    mt.expiry = m.now() + rand_delay();
                    env.em.reschedule(mt.tid, mt.expiry - m.now());
                    mt.heap = m.goes_to_heap(mt.expiry, mt.precise,
                                             m.now_tick());
                    mt.effective = mt.heap
                        ? mt.expiry
                        : TimePoint(Nanos(
                              std::int64_t(tick_up(mt.expiry) * kTickNs)));
                    char b[192];
                    std::snprintf(b, sizeof b,
                                  "resched@now=%lld -> exp=%lld eff=%lld heap=%d",
                                  (long long)m.now().time_since_epoch().count(),
                                  (long long)mt.expiry.time_since_epoch().count(),
                                  (long long)mt.effective.time_since_epoch().count(),
                                  (int)mt.heap);
                    m.note(tag, b);
                }
            } else {                             // advance time
                fired.clear();
                fired_ctx.clear();
                Duration jump;
                switch (rng() % 10) {
                case 0: jump = Nanos(kTickNs) * (256 + rng() % 3840); break;
                case 1: jump = Nanos(kTickNs) * (65536 + rng() % 65536); break;
                default: jump = Nanos(rng() % (kTickNs * 64)); break;
                }
                // Chunk so the wheel's per-advance tick bound never caps a
                // pump; accumulate expected + actual over the whole jump.
                std::vector<int> expected;
                Duration left = jump;
                while (left.count() > 0) {
                    Duration chunk = std::min(left, Nanos(kTickNs) * 4000);
                    env.advance(chunk);
                    m.fire_due(env.em.now(), expected);
                    left -= chunk;
                }
                std::sort(expected.begin(), expected.end());
                std::sort(fired.begin(), fired.end());
                CAPTURE(step);
                CAPTURE(jump.count());
                if (expected != fired) {
                    std::vector<int> only_exp, only_fired;
                    std::set_difference(expected.begin(), expected.end(),
                                        fired.begin(), fired.end(),
                                        std::back_inserter(only_exp));
                    std::set_difference(fired.begin(), fired.end(),
                                        expected.begin(), expected.end(),
                                        std::back_inserter(only_fired));
                    for (int t : only_exp) {
                        const auto& mt = m.timers[t];
                        MESSAGE("expected-not-fired tag=", t,
                                " expiry=", mt.expiry.time_since_epoch().count(),
                                " eff=", mt.effective.time_since_epoch().count(),
                                " period=", mt.period.count(),
                                " precise=", mt.precise, " heap=", mt.heap,
                                " now=", env.em.now().time_since_epoch().count());
                        for (auto& h : m.history[t]) MESSAGE("  ", h);
                    }
                    for (int t : only_fired) {
                        const auto& mt = m.timers[t];
                        std::string ctx_str = "none";
                        auto it = fired_ctx.find(t);
                        if (it != fired_ctx.end()) {
                            char b[128];
                            std::snprintf(b, sizeof b,
                                          "scheduled=%lld now=%lld missed=%u",
                                          (long long)it->second.scheduled
                                              .time_since_epoch().count(),
                                          (long long)it->second.now
                                              .time_since_epoch().count(),
                                          it->second.missed);
                            ctx_str = b;
                        }
                        MESSAGE("fired-not-expected tag=", t,
                                " expiry=", mt.expiry.time_since_epoch().count(),
                                " eff=", mt.effective.time_since_epoch().count(),
                                " period=", mt.period.count(),
                                " precise=", mt.precise, " heap=", mt.heap,
                                " alive=", mt.alive, " ctx=", ctx_str,
                                " now=", env.em.now().time_since_epoch().count());
                        for (auto& h : m.history[t]) MESSAGE("  ", h);
                    }
                }
                REQUIRE(expected == fired);
                for (int tag : fired) {
                    const TimerCtx& tc = fired_ctx[tag];
                    const ModelTimer& mt = m.timers[tag];
                    CHECK(tc.now >= tc.scheduled);   // never early
                    // One-shots keep their armed expiry; repeats reschedule.
                    CHECK((tc.scheduled == mt.expiry ||
                          mt.period.count() > 0));
                    (void)mt;
                }
            }
            m.sweep_dead();
            // Fired one-shots become stale ids; repeats stay live. Cancelled
            // tags were already pushed at cancel time.
            for (int tag : fired)
                if (m.timers[tag].period.count() == 0)
                    m.dead_tags.push_back(tag);
            if (m.dead_tags.size() > 512) {
                m.dead_tags.erase(m.dead_tags.begin(),
                                  m.dead_tags.begin() + 256);
            }
        }
    }
}

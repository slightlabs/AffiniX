#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "../test_env.hpp"
#include "afx/core/event_manager.hpp"

using namespace afx;
using afx::test::TestEnv;

TEST_CASE("EM stage order: mailbox before timers before defer") {
    TestEnv env;
    std::vector<int> order;
    // Arm a timer for "now" and post mailbox work and defer work up-front.
    env.em.defer([&] { order.push_back(3); });
    env.em.at(TimePoint(0ms), [&](TimerCtx) { order.push_back(2); });
    env.em.mailbox().post([&] { order.push_back(1); });
    env.em.poll_once();
    CHECK(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("EM defer runs at end of the same iteration") {
    TestEnv env;
    std::vector<int> order;
    env.em.defer([&] {
        order.push_back(1);
        env.em.defer([&] { order.push_back(3); });  // deferred goes next round
    });
    env.em.defer([&] { order.push_back(2); });
    env.em.poll_once();
    CHECK(order == std::vector<int>{1, 2});
    env.em.poll_once();
    CHECK(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("EM post is drained by the owning thread") {
    TestEnv env;
    int n = 0;
    for (int i = 0; i < 10; ++i)
        CHECK(env.em.mailbox().post([&] { ++n; }) == PostResult::Ok);
    CHECK(n == 0);  // nothing ran yet
    env.em.poll_once();
    CHECK(n == 10);
}

TEST_CASE("EM ambient context propagates through post()") {
    TestEnv env;
    TraceId seen;
    Context c;
    c.trace = TraceId{0xdead, 0xbeef, 1};
    env.em.defer([&] {
        auto scope = env.em.with_context(c);
        env.em.mailbox().post([&] { seen = detail::ambient_context().trace; });
    });
    env.em.poll_once();
    env.em.poll_once();
    CHECK(seen == c.trace);
}

TEST_CASE("EM sheds work posted with an already-expired deadline") {
    TestEnv env;
    int ran = 0;
    Context c;
    c.deadline = Deadline::at(TimePoint(-1ms));  // expired before posting
    CHECK(env.em.mailbox().post_with_ctx([&] { ++ran; }, c) == PostResult::Ok);
    env.em.poll_once();
    CHECK(ran == 0);
    CHECK(env.em.stats().deadline_expired_before_start == 1);
    CHECK(env.em.stats().mailbox_pops == 1);
}

TEST_CASE("EM context does not leak between work items") {
    TestEnv env;
    bool leaked = false;
    Context c;
    c.deadline = Deadline::at(TimePoint(1h));
    env.em.defer([&] { auto s = env.em.with_context(c); });
    env.em.defer([&] { leaked = detail::ambient_context().deadline.is_set(); });
    env.em.poll_once();
    CHECK(!leaked);
}

TEST_CASE("EM stop() terminates run()") {
    TestEnv env;
    env.em.after(1ms, [&](TimerCtx) { env.em.stop(); });
    env.em.mailbox().post([] {});  // ensure a wake is pending
    // run() under virtual clock would spin; drive iterations manually instead.
    for (int i = 0; i < 4; ++i) {
        env.em.clock().advance(1ms);
        env.em.poll_once();
    }
    CHECK(!env.em.is_running());
}

TEST_CASE("EM stats accumulate across stages") {
    TestEnv env;
    env.em.after(1ms, [](TimerCtx) {});
    env.em.mailbox().post([] {});
    env.em.poll_once();
    env.em.clock().advance(1ms);
    env.em.poll_once();
    CHECK(env.em.stats().iterations == 2);
    CHECK(env.em.stats().mailbox_pops >= 1);
    CHECK(env.em.stats().timers_fired == 1);
    CHECK(env.em.stats().timers_armed == 1);
}

TEST_CASE("EM iteration callback reports what each iteration did") {
    TestEnv env;
    IterationInfo last{};
    env.em.on_iteration([&](const IterationInfo& i) { last = i; });
    env.em.mailbox().post([] {});
    env.em.poll_once();
    CHECK(last.did_mailbox);
    env.em.poll_once();
    CHECK(!last.did_mailbox);
    CHECK(!last.did_io);
}

TEST_CASE("EM mailbox on a dead EM reports Closed") {
    Mailbox mb;
    {
        TestEnv env;
        mb = env.em.mailbox();
        CHECK(mb.post([] {}) == PostResult::Ok);
    }  // EM destroyed; mb still valid
    CHECK(mb.post([] {}) == PostResult::Closed);
}

TEST_CASE("EM watch/unwatch via sim backend readiness") {
    TestEnv env;
    int fd = env.sim().add_fd();
    int mask = 0;
    auto io = env.em.watch(fd, Interest::Readable,
                           [&](IoId, std::int32_t m) { mask = m; });
    REQUIRE(io.has_value());
    env.sim().deliver_watch(fd, Interest::Readable);
    env.em.poll_once();
    CHECK((mask & CompletionFlag::ReadyRead) != 0);
    env.em.unwatch(*io);
    env.sim().deliver_watch(fd, Interest::Readable);
    mask = 0;
    env.em.poll_once();
    CHECK(mask == 0);  // detached: no more events
}

TEST_CASE("EM spin-until: idle loop stops reporting work") {
    TestEnv env;
    std::size_t idle0 = env.em.stats().idle_iterations;
    env.em.poll_once();
    CHECK(env.em.stats().idle_iterations == idle0 + 1);
}

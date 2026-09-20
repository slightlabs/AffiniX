#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "../test_env.hpp"
#include "afx/itc/mailbox.hpp"

using namespace afx;
using afx::test::TestEnv;

TEST_CASE("Mailbox post/post_batch deliver in order") {
    TestEnv env;
    Mailbox mb = env.em.mailbox();
    std::vector<int> got;
    for (int i = 0; i < 5; ++i) mb.post([&, i] { got.push_back(i); });
    std::vector<Task> batch;
    for (int i = 5; i < 8; ++i)
        batch.emplace_back([&, i] { got.push_back(i); });
    CHECK(mb.post_batch(batch) == 3);
    env.em.poll_once();
    CHECK(got == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7});
}

TEST_CASE("Mailbox bounded: Fail policy reports Full") {
    TestEnv env(
        EventManagerConfig{.wait = WaitStrategy::Spin, .mailbox_capacity = 4});
    Mailbox mb = env.em.mailbox();
    for (int i = 0; i < 4; ++i) CHECK(mb.post([] {}) == PostResult::Ok);
    CHECK(mb.post([] {}) == PostResult::Full);
    env.em.poll_once();
    CHECK(mb.post([] {}) == PostResult::Ok);  // space freed by drain
}

TEST_CASE("post_and_reply round-trips a result") {
    TestEnv env;
    Mailbox mb = env.em.mailbox();
    int reply = 0;
    CHECK(mb.post_and_reply([] { return 40 + 2; }, mb,
                            [&](int v) { reply = v; }) == PostResult::Ok);
    env.em.poll_once();  // work runs; reply lands in the same drain
    CHECK(reply == 42);
}

TEST_CASE("post_and_reply preserves the caller's context") {
    TestEnv env;
    Mailbox mb = env.em.mailbox();
    TraceId seen;
    Context c;
    c.trace = TraceId{1, 2, 3};
    env.em.defer([&] {
        auto scope = env.em.with_context(c);
        mb.post_and_reply([] { return 7; }, mb,
                          [&](int) { seen = detail::ambient_context().trace; });
    });
    env.em.poll_once();
    env.em.poll_once();
    env.em.poll_once();
    CHECK(seen == c.trace);
}

// Lost-wakeup protocol (§8.1): a producer posting while the consumer is
// Blocked must still wake it. Drive a real epoll EM on a thread.
TEST_CASE("Mailbox arm/block: post wakes a blocked EM") {
    EventManager em(EventManagerConfig{.wait = WaitStrategy::Block});
    std::atomic<int> ran{0};
    std::atomic<bool> started{false};
    std::thread t([&] {
        started = true;
        em.run();
    });
    while (!em.is_running()) {}
    // Give the loop a moment to reach the blocked wait, then post.
    std::this_thread::sleep_for(5ms);
    for (int i = 0; i < 32; ++i) {
        em.mailbox().post([&] { ++ran; });
    }
    for (int i = 0; i < 1000 && ran < 32; ++i) std::this_thread::sleep_for(1ms);
    em.stop();
    t.join();
    CHECK(ran == 32);
}

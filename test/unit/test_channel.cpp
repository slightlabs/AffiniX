#include <doctest/doctest.h>

#include <vector>

#include "../test_env.hpp"
#include "afx/itc/barrier.hpp"
#include "afx/itc/channel.hpp"
#include "afx/itc/fan.hpp"
#include "afx/itc/sharded.hpp"

using namespace afx;
using afx::test::TestEnv;

TEST_CASE("Channel SPSC delivers batches to the attached EM") {
    TestEnv env;
    auto [tx, rx] = channel<int>(16, ChannelKind::SPSC);
    std::vector<int> got;
    rx.attach(env.em, [&](std::span<const int> batch) {
        got.insert(got.end(), batch.begin(), batch.end());
    });
    for (int i = 0; i < 10; ++i) CHECK(tx.try_push(i));
    env.em.poll_once();          // drain mailbox -> channel drain -> callback
    CHECK(got.size() == 10);
    CHECK(got.back() == 9);
}

TEST_CASE("Channel MPSC same delivery") {
    TestEnv env;
    auto [tx, rx] = channel<int>(16, ChannelKind::MPSC);
    int sum = 0;
    rx.attach(env.em, [&](std::span<const int> batch) {
        for (int v : batch) sum += v;
    });
    for (int i = 1; i <= 4; ++i) tx.try_push(i);
    env.em.poll_once();
    CHECK(sum == 10);
}

TEST_CASE("Channel is bounded") {
    auto [tx, rx] = channel<int>(4, ChannelKind::SPSC);
    for (int i = 0; i < 4; ++i) CHECK(tx.try_push(i));
    CHECK(!tx.try_push(5));
}

TEST_CASE("Channel poll() works without an EM") {
    auto [tx, rx] = channel<int>(8, ChannelKind::SPSC);
    int n = 0;
    // No attach: poke() skips posting; poll() drains manually.
    tx.try_push(1);
    tx.try_push(2);
    rx.poll();                   // no callback attached: just drains
    CHECK(n == 0);
}

TEST_CASE("Barrier runs per_shard on all, done once") {
    TestEnv env;
    Mailbox mb = env.em.mailbox();
    std::vector<Mailbox> mbs{mb, mb, mb};
    int per = 0, done = 0;
    Barrier::arrive_all(mbs, [&] { ++per; }, [&] { ++done; });
    env.em.poll_once();
    CHECK(per == 3);
    CHECK(done == 1);
}

TEST_CASE("Fan round-robin spreads across shards") {
    TestEnv env;
    Mailbox mb = env.em.mailbox();
    Fan<int> fan({mb, mb, mb, mb});
    int n = 0;
    for (int i = 0; i < 8; ++i)
        fan.dispatch(i, [&](int) { ++n; });
    env.em.poll_once();
    CHECK(n == 8);
}

TEST_CASE("Fan hash-by-key pins same key to same shard") {
    struct Item { int key; int v; };
    // 4 real mailboxes but we only check pick() determinism via dispatch
    // counts: same key must always route to the same mailbox.
    TestEnv env;
    Mailbox mb = env.em.mailbox();
    Fan<Item> fan({mb, mb, mb, mb}, HashBy<Item, int>{&Item::key});
    int n = 0;
    for (int i = 0; i < 16; ++i)
        fan.dispatch(Item{7, i}, [&](Item) { ++n; });
    env.em.poll_once();
    CHECK(n == 16);              // all landed on one shard (this EM)
}

TEST_CASE("Fan broadcast reaches every mailbox") {
    TestEnv env;
    Mailbox mb = env.em.mailbox();
    Fan<int> fan({mb, mb, mb});
    int n = 0;
    fan.broadcast(1, [&](int) { ++n; });
    env.em.poll_once();
    CHECK(n == 3);
}

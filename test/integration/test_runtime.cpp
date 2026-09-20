#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "afx/runtime.hpp"

using namespace afx;

static bool wait_until(std::function<bool()> pred, int ms = 5000) {
    for (int i = 0; i < ms; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

TEST_CASE("Runtime spawns shards and they publish mailboxes") {
    Runtime rt(Topology::detect());
    ThreadConfig cfg;
    cfg.placement = Placement::None;
    cfg.em.wait = WaitStrategy::Block;

    auto g = rt.spawn_group("w", 2, cfg);
    std::atomic<int> setup_ran{0};
    g.each([&](EventManager&) { ++setup_ran; });
    rt.start();

    auto mbs = g.mailboxes();  // blocks until ready
    CHECK(mbs.size() == 2);
    CHECK(wait_until([&] { return setup_ran == 2; }));

    rt.shutdown(5s);
    rt.join();
    CHECK(rt.stopping());
}

TEST_CASE("Runtime: cross-shard post runs on the owning thread") {
    Runtime rt(Topology::detect());
    ThreadConfig cfg;
    cfg.placement = Placement::None;
    cfg.em.wait = WaitStrategy::Block;

    auto g = rt.spawn_group("w", 2, cfg);
    std::atomic<int> hits{0};
    std::thread::id tid0;
    g.each([&](EventManager& em) {
        if (hits == 0) tid0 = std::this_thread::get_id();
    });
    rt.start();
    auto mbs = g.mailboxes();
    REQUIRE(mbs.size() == 2);

    // Post to each mailbox; tasks must execute on their shard's thread.
    std::atomic<int> on_owner{0};
    std::vector<std::thread::id> tids(mbs.size());
    for (std::size_t i = 0; i < mbs.size(); ++i)
        mbs[i].post([&, i] {
            tids[i] = std::this_thread::get_id();
            ++on_owner;
        });
    CHECK(wait_until([&] { return on_owner == int(mbs.size()); }));
    CHECK(tids[0] != tids[1]);  // distinct shard threads
    CHECK(tids[0] != std::this_thread::get_id());

    rt.shutdown(5s);
    rt.join();
}

TEST_CASE("Runtime shutdown is idempotent and joins cleanly") {
    Runtime rt(Topology::detect());
    ThreadConfig cfg;
    cfg.placement = Placement::None;
    auto g = rt.spawn_group("w", 1, cfg);
    rt.start();
    g.mailboxes();
    rt.shutdown(1s);
    rt.shutdown(1s);  // second call is a no-op
    rt.join();
}

TEST_CASE("Topology detects at least one core on this machine") {
    auto topo = Topology::detect();
    CHECK(!topo.empty());
    CHECK(topo.size() >= 1);
    auto phys = topo.physical_cores();
    CHECK(!phys.empty());
    CHECK(phys.size() <= topo.size());
}

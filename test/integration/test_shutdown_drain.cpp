// §20 drain sequence tests (M5-09). The EM drives owned objects through
// listeners-close -> notify -> drain -> shutdown_write -> stop, and the
// runtime delivers the kick through the mailbox.

#include <doctest/doctest.h>

#include <atomic>
#include <string>
#include <vector>

#include "../test_env.hpp"
#include "afx/core/event_manager.hpp"

using namespace afx;

namespace {

// A stand-in "owned object" (a server, say) that records hook order and
// controls when it reports drained.
struct FakeOwned {
    std::vector<const char*> log;
    bool drained = false;

    static void begin(void* p) { static_cast<FakeOwned*>(p)->log.push_back("begin"); }
    static void notify(void* p) { static_cast<FakeOwned*>(p)->log.push_back("notify"); }
    static bool drained_f(void* p) { return static_cast<FakeOwned*>(p)->drained; }
    static void half(void* p) { static_cast<FakeOwned*>(p)->log.push_back("shutdown_write"); }
};

}  // namespace

TEST_CASE("drain: hooks run in §20 order, stopping when writes complete") {
    afx::test::TestEnv env;
    FakeOwned o;

    typename afx::test::TestEnv::EM::ShutdownHooks hooks{
        &FakeOwned::begin, &FakeOwned::notify, &FakeOwned::drained_f,
        &FakeOwned::half};
    env.em.own(&o, [](void*) {}, hooks);

    env.em.begin_shutdown(env.em.now() + 5s);
    // begin + notify ran synchronously; drain waits (o.drained == false).
    REQUIRE(o.log.size() == 2);
    CHECK(o.log[0] == std::string("begin"));
    CHECK(o.log[1] == std::string("notify"));
    CHECK(env.em.shutting_down());

    // The drain recheck runs on a 1 ms timer under VirtualClock.
    env.advance(2ms);
    CHECK(o.log.size() == 2);  // still not drained

    o.drained = true;
    env.advance(2ms);
    REQUIRE(o.log.size() == 3);
    CHECK(o.log[2] == std::string("shutdown_write"));
}

TEST_CASE("drain: an expired deadline forces half-close of undrained work") {
    afx::test::TestEnv env;
    FakeOwned o;
    o.drained = false;  // never completes — abandoned work path

    typename afx::test::TestEnv::EM::ShutdownHooks hooks{
        &FakeOwned::begin, &FakeOwned::notify, &FakeOwned::drained_f,
        &FakeOwned::half};
    env.em.own(&o, [](void*) {}, hooks);

    // Deadline already in the past: the drain wait is skipped entirely.
    env.em.begin_shutdown(env.em.now() - 1s);
    REQUIRE(o.log.size() == 3);
    CHECK(o.log[2] == std::string("shutdown_write"));
}

TEST_CASE("drain: repeated begin_shutdown is a no-op") {
    afx::test::TestEnv env;
    FakeOwned o;

    typename afx::test::TestEnv::EM::ShutdownHooks hooks{
        &FakeOwned::begin, &FakeOwned::notify, &FakeOwned::drained_f,
        &FakeOwned::half};
    env.em.own(&o, [](void*) {}, hooks);

    env.em.begin_shutdown(env.em.now() + 5s);
    env.em.begin_shutdown(env.em.now() + 5s);
    CHECK(o.log.size() == 2);  // begin/notify once, not twice
}

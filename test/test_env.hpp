#pragma once

// TestEnv — VirtualClock + SimBackend driving an EM deterministically
// (DESIGN.md §22.2). No sleeping, no flakiness.

#include "afx/backend/sim.hpp"
#include "afx/core/event_manager.hpp"
#include "afx/net/tcp_server.hpp"
#include "afx/net/tcp_client.hpp"

namespace afx::test {

struct TestEnv {
    using EM = BasicEventManager<VirtualClock, SimBackend>;

    EM em;

    TestEnv() : em(EventManagerConfig{.wait = WaitStrategy::Spin}) {}
    explicit TestEnv(EventManagerConfig cfg)
        : em(std::move(cfg), VirtualClock{}, SimBackend{}) {}

    void pump(std::size_t n = 1) { while (n--) em.poll_once(); }

    void advance(Duration d) {
        em.clock().advance(d);
        pump();
    }
    void advance_to(TimePoint t) {
        em.clock().set(t);
        pump();
    }

    SimBackend& sim() { return em.backend(); }
};

} // namespace afx::test

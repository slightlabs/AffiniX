#pragma once

// RealEnv — real-clock EM driving helpers for backend-parity tests (M8-11).
// The same test body runs against every production backend; construction
// differences live here so test bodies carry no backend-specific branches.

#include <doctest/doctest.h>

#include <memory>

#include "afx/core/event_manager.hpp"
#ifdef AFX_WITH_URING
#include "afx/backend/uring.hpp"
#endif

namespace afx::test {

// The platform readiness EM for parity runs: epoll on Linux, kqueue on
// macOS/BSD (M11-07). The canonical ::afx::EventManager is auto-selecting,
// so parity must pin the concrete backends.
using PollEM = BasicEventManager<SteadyClock, DefaultPollBackend>;
using EpollEM = PollEM;  // legacy name inside existing tests
#ifdef AFX_WITH_URING
using UringEM = BasicEventManager<SteadyClock, UringBackend>;
#endif

// Registers one doctest TEST_CASE_TEMPLATE per production backend. On Linux
// that's epoll + io_uring; on macOS/BSD it's kqueue alone.
#ifdef AFX_WITH_URING
#define AFX_BACKEND_TEST_CASE(name, T) \
    TEST_CASE_TEMPLATE(name, T, ::afx::test::PollEM, ::afx::test::UringEM)
#else
#define AFX_BACKEND_TEST_CASE(name, T) \
    TEST_CASE_TEMPLATE(name, T, ::afx::test::PollEM)
#endif

// Construct an EM of type T around `cfg` on the heap (EMs are not movable).
// UringBackend needs two-phase construction (create() may legitimately fail
// where io_uring is unavailable); other backends are default-constructible.
// Returns nullptr when the requested backend cannot be built.
template <class EM>
std::unique_ptr<EM> make_real_em(EventManagerConfig cfg) {
#ifdef AFX_WITH_URING
    if constexpr (std::is_same_v<EM, UringEM>) {
        auto b = UringBackend::create();
        if (!b) return nullptr;
        return std::make_unique<EM>(std::move(cfg), SteadyClock{},
                                    std::move(*b));
    }
#endif
    return std::make_unique<EM>(std::move(cfg));
}

}  // namespace afx::test

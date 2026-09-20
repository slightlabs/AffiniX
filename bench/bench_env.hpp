#pragma once

// Shared bench harness bits: a smoke mode for CI (AFX_BENCH_SMOKE=1 runs a
// token number of epochs so the binary is a regression-compile + run check,
// not a measurement), and helpers to build EMs.

#include <cstdlib>

#include <nanobench.h>

#include "afx/core/event_manager.hpp"

namespace afx::bench {

inline bool smoke() {
    static const bool v = std::getenv("AFX_BENCH_SMOKE") != nullptr;
    return v;
}

// A Bench pre-configured for smoke mode: in CI we only prove the code runs.
inline ankerl::nanobench::Bench make_bench() {
    ankerl::nanobench::Bench b;
    if (smoke()) {
        b.warmup(1).epochs(2).epochIterations(8);
    }
    return b;
}

}  // namespace afx::bench

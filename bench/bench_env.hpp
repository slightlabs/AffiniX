#pragma once

// Shared bench harness bits: a smoke mode for CI (AFX_BENCH_SMOKE=1 runs a
// token number of epochs so the binary is a regression-compile + run check,
// not a measurement), and helpers to build EMs.

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

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

// Baseline capture (§6): AFX_BENCH_JSON=<path> makes each binary write one
// JSON document for bench/baselines/. Benches create several Bench objects,
// so results are collected per-run and flushed once at the end of main.
inline std::vector<ankerl::nanobench::Result>& collected() {
    static std::vector<ankerl::nanobench::Result> v;
    return v;
}
inline void collect(ankerl::nanobench::Bench& b) {
    for (auto& r : b.results()) collected().push_back(std::move(r));
}
inline void flush_json() {
    const char* out = std::getenv("AFX_BENCH_JSON");
    if (!out) return;
    std::ofstream f(out);
    if (f)
        ankerl::nanobench::render(ankerl::nanobench::templates::json(),
                                  collected(), f);
    else
        std::cerr << "bench: cannot open " << out << " for writing\n";
}

}  // namespace afx::bench

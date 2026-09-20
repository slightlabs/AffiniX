#include "afx/sys/clock.hpp"

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <x86intrin.h>
#endif

#include <fstream>
#include <string>

namespace afx {

std::uint64_t rdtsc() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(
        SteadyClock::now().time_since_epoch().count());
#endif
}

bool tsc_invariant() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    bool constant = false, nonstop = false;
    while (std::getline(f, line)) {
        if (line.find("constant_tsc") != std::string::npos) constant = true;
        if (line.find("nonstop_tsc") != std::string::npos) nonstop = true;
        if (constant && nonstop) return true;
    }
    return false;
#else
    return false;
#endif
}

std::uint64_t tsc_hz() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    if (!tsc_invariant()) return 0;
    auto t0 = SteadyClock::now();
    auto c0 = rdtsc();
    // Short calibration window; callers wanting precision re-calibrate.
    while (SteadyClock::now() - t0 < std::chrono::milliseconds(2)) {}
    auto elapsed = (SteadyClock::now() - t0).count();
    auto c1 = rdtsc();
    if (elapsed <= 0) return 0;
    return static_cast<std::uint64_t>(
        (static_cast<long double>(c1 - c0) * 1e9L) / elapsed);
#else
    return 0;
#endif
}

}  // namespace afx

#pragma once

// Affinity — DESIGN.md §19. CoreSet is a declarative set of logical CPUs;
// pin/unpin and scheduler policy applied to the current thread. Failures are
// reported as errors, never silently ignored.

#include <cstdint>
#include <variant>
#include <vector>

#include "afx/sys/result.hpp"

namespace afx {

class CoreSet {
public:
    CoreSet() = default;

    static CoreSet range(int first, int last) {   // inclusive
        CoreSet s;
        for (int i = first; i <= last; ++i) s.add(i);
        return s;
    }
    static CoreSet of(std::initializer_list<int> ids) {
        CoreSet s;
        for (int i : ids) s.add(i);
        return s;
    }
    static CoreSet all() { CoreSet s; s.all_ = true; return s; }

    void add(int cpu) {
        if (cpu < 0) return;
        if (cpu >= int(bits_.size() * 64))
            bits_.resize(cpu / 64 + 1, 0);
        bits_[cpu / 64] |= (std::uint64_t(1) << (cpu % 64));
    }
    bool contains(int cpu) const {
        return all_ || (cpu >= 0 && cpu < int(bits_.size() * 64) &&
                        (bits_[cpu / 64] >> (cpu % 64)) & 1);
    }
    bool unrestricted() const { return all_; }
    std::vector<int> cpus() const {
        std::vector<int> out;
        for (int i = 0; i < int(bits_.size() * 64); ++i)
            if (contains(i)) out.push_back(i);
        return out;
    }
    std::size_t size() const { return unrestricted() ? ~std::size_t(0) : cpus().size(); }
    bool empty() const { return !all_ && bits_.empty(); }

private:
    std::vector<std::uint64_t> bits_;
    bool all_ = false;
};

// Scheduler policy (§19): Other is the default; Fifo/Rr need privileges and
// failures are reported, not swallowed.
struct SchedPolicy {
    struct Other {};
    struct Fifo { int prio; };
    struct Rr   { int prio; };
    std::variant<Other, Fifo, Rr> v = Other{};
};

Result<void> pin_this_thread(const CoreSet& set);
Result<void> apply_sched_this_thread(const SchedPolicy& p);
// Cores this thread is currently allowed to run on (the cpuset).
Result<CoreSet> allowed_cpus();

} // namespace afx

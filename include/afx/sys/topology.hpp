#pragma once

// Topology — DESIGN.md §19. Parsed from sysfs; the sysfs root is injectable
// so CI can exercise dual-socket, SMT-off, restricted-cpuset and hybrid
// layouts from committed fixtures.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace afx {

struct Core {
    int              id = 0;            // logical cpu index
    int              package = 0;       // socket
    int              physical = 0;      // physical core id within package
    int              numa = -1;
    std::vector<int> smt_siblings;      // includes self
};

class Topology {
public:
    // sysfs_root defaults to /sys; tests inject a fixture tree.
    static Topology detect(std::string_view sysfs_root = "/sys");

    std::span<const Core> cores() const noexcept { return cores_; }

    const Core* core(int logical_id) const noexcept {
        for (auto& c : cores_) if (c.id == logical_id) return &c;
        return nullptr;
    }

    int numa_node_of(int core_id) const noexcept {
        if (const Core* c = core(core_id)) return c->numa;
        return -1;
    }

    // One entry per (package, physical) — the set of physical cores.
    std::vector<int> physical_cores() const;

    // /sys/class/net/<if>/device/numa_node
    std::optional<int> numa_node_of_nic(std::string_view ifname) const;

    std::size_t size() const noexcept { return cores_.size(); }
    bool empty() const noexcept { return cores_.empty(); }

private:
    explicit Topology(std::string sysfs_root) : sysfs_root_(std::move(sysfs_root)) {}

    std::string sysfs_root_;
    std::vector<Core> cores_;
    std::map<int, std::vector<int>> numa_to_cores_;
};

} // namespace afx

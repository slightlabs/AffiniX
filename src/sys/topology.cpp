#include "afx/sys/topology.hpp"

#include <unistd.h>
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace afx {

namespace fs = std::filesystem;

namespace {

int read_int(const fs::path& p, int fallback) {
    std::ifstream f(p);
    int v = fallback;
    f >> v;
    return v;
}

std::vector<int> parse_cpu_list(const std::string& s) {
    // "0-3,8,10-12" style list.
    std::vector<int> out;
    std::istringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        int a = 0, b = 0;
        if (auto dash = tok.find('-'); dash != std::string::npos) {
            a = std::atoi(tok.c_str());
            b = std::atoi(tok.c_str() + dash + 1);
        } else {
            a = b = std::atoi(tok.c_str());
        }
        for (int i = a; i <= b; ++i) out.push_back(i);
    }
    return out;
}

}  // namespace

Topology Topology::detect(std::string_view sysfs_root) {
    Topology t{std::string(sysfs_root)};
    fs::path cpu_root = t.sysfs_root_ + "/devices/system/cpu";

    std::error_code ec;
    if (!fs::exists(cpu_root, ec)) {
        // No sysfs (macOS/BSD, or an explicit nonexistent fixture root):
        // synthesize one flat Core per online CPU — no NUMA/SMT info exists
        // to parse, and tests only need count consistency.
        long n = ::sysconf(_SC_NPROCESSORS_ONLN);
        for (long i = 0; i < (n > 0 ? n : 1); ++i) {
            Core c;
            c.id = int(i);
            c.package = 0;
            c.physical = int(i);
            c.smt_siblings = {int(i)};
            c.numa = -1;
            t.cores_.push_back(c);
        }
        return t;
    }
    for (auto& e : fs::directory_iterator(cpu_root, ec)) {
        std::string name = e.path().filename().string();
        if (name.rfind("cpu", 0) != 0) continue;
        int id = -1;
        auto r =
            std::from_chars(name.data() + 3, name.data() + name.size(), id);
        if (r.ec != std::errc() || id < 0) continue;

        Core c;
        c.id = id;
        fs::path topo = e.path() / "topology";
        c.package = read_int(topo / "physical_package_id", 0);
        c.physical = read_int(topo / "core_id", id);
        if (auto f = std::ifstream(topo / "thread_siblings_list"); f) {
            std::string line;
            std::getline(f, line);
            c.smt_siblings = parse_cpu_list(line);
        }
        if (c.smt_siblings.empty()) c.smt_siblings = {id};

        // NUMA: cpuN is a symlink into nodeN directories on real sysfs; on
        // minimal fixtures, fall back to node dirs listing cpuN.
        c.numa = -1;
        fs::path node_root = t.sysfs_root_ + "/devices/system/node";
        for (auto& ne : fs::directory_iterator(node_root, ec)) {
            std::string nn = ne.path().filename().string();
            if (nn.rfind("node", 0) != 0) continue;
            int nid = -1;
            auto rr =
                std::from_chars(nn.data() + 4, nn.data() + nn.size(), nid);
            if (rr.ec != std::errc() || nid < 0) continue;
            for (auto& ce : fs::directory_iterator(ne.path(), ec)) {
                if (ce.path().filename().string() == name) {
                    c.numa = nid;
                    t.numa_to_cores_[nid].push_back(id);
                    break;
                }
            }
            if (c.numa >= 0) break;
        }
        t.cores_.push_back(std::move(c));
    }
    std::sort(t.cores_.begin(), t.cores_.end(),
              [](const Core& a, const Core& b) { return a.id < b.id; });
    return t;
}

std::vector<int> Topology::physical_cores() const {
    std::vector<int> out;
    std::set<std::pair<int, int>> seen;
    for (auto& c : cores_)
        if (seen.emplace(c.package, c.physical).second) out.push_back(c.id);
    return out;
}

std::optional<int> Topology::numa_node_of_nic(std::string_view ifname) const {
    fs::path p =
        sysfs_root_ + "/class/net/" + std::string(ifname) + "/device/numa_node";
    std::ifstream f(p);
    if (!f) return std::nullopt;
    int v;
    f >> v;
    return v >= 0 ? std::optional<int>(v) : std::nullopt;
}

}  // namespace afx

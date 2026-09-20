#pragma once

// NUMA — DESIGN.md §19, M1-11. Raw syscalls (getcpu, mbind, get_mempolicy);
// no dependency on libnuma. Node-local arena allocation per EM lives here so
// MemoryConfig::{hugepages,numa,prefault} has a real implementation behind it.

#include <cstddef>
#include <cstdint>

#include "afx/sys/result.hpp"

namespace afx::numa {

// True when the kernel supports NUMA memory policy at all (get_mempolicy
// does not return ENOSYS). Everything degrades gracefully when false.
bool available() noexcept;

// Number of NUMA nodes the kernel reports (maxnode of get_mempolicy), or 1.
int node_count() noexcept;

// NUMA node of the CPU this thread is currently running on (-1: unknown).
int current_node() noexcept;

struct AllocOpts {
    int node = -1;          // >= 0: MPOL_BIND to this node
    bool interleave = false;  // MPOL_INTERLEAVE over all nodes (wins over node)
    bool hugepages = false;   // try MAP_HUGETLB, then MADV_HUGEPAGE, then plain
    bool prefault = false;    // touch every page after binding (init, not peak)
};

// An owned anonymous mapping. Heap-backed when the mapping path fails and the
// caller asked for a plain fallback (Region::mapped() distinguishes them).
class Region {
  public:
    Region() = default;
    ~Region();

    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;
    Region(Region&& o) noexcept { *this = std::move(o); }
    Region& operator=(Region&& o) noexcept;

    std::byte* data() noexcept { return static_cast<std::byte*>(p_); }
    const std::byte* data() const noexcept {
        return static_cast<const std::byte*>(p_);
    }
    std::size_t size() const noexcept { return n_; }
    bool mapped() const noexcept { return mapped_; }
    bool hugepages() const noexcept { return hugepages_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }

  private:
    friend Result<Region> alloc(std::size_t, const AllocOpts&);
    void* p_ = nullptr;
    std::size_t n_ = 0;
    bool mapped_ = false;    // munmap vs free
    bool hugepages_ = false;  // actually backed by huge pages
};

// Allocate `bytes` with the requested NUMA policy. The region is bound BEFORE
// it is populated: with MPOL_BIND a MAP_POPULATE at mmap time would charge
// pages to the wrong node, so prefault is an explicit touch pass afterwards.
// Falls back to a plain mapping (never an error) when the kernel lacks NUMA;
// only OOM-level failures surface as an Error.
Result<Region> alloc(std::size_t bytes, const AllocOpts& o);

// Rebind an existing range to a node (MPOL_BIND). M5-05 uses this for the
// per-EM arena.
Result<void> bind_to_node(void* p, std::size_t n, int node) noexcept;

}  // namespace afx::numa

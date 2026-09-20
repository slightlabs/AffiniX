#pragma once

// Arena — DESIGN.md §12.2. A per-EM bump region allocated once at EM
// construction with the MemoryConfig NUMA policy applied (node-local mbind,
// optional hugepages, prefault). Bump allocation only; reclaiming happens at
// granularity above (Pool free lists, whole-arena reset).

#include <cstddef>
#include <cstdint>

#include "afx/sys/numa.hpp"
#include "afx/sys/result.hpp"

namespace afx {

class Arena {
  public:
    Arena() = default;
    // `bytes == 0` creates an empty arena whose alloc() always falls back to
    // the caller (returns nullptr) — used when memory.arena_bytes disables it.
    explicit Arena(std::size_t bytes, const numa::AllocOpts& o = {}) {
        if (bytes == 0) return;
        if (auto r = numa::alloc(bytes, o)) region_ = std::move(*r);
        // A failed mapping leaves an empty arena; pools fall back to heap.
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) noexcept = default;
    Arena& operator=(Arena&&) noexcept = default;

    // Aligned bump allocation. nullptr when the region is exhausted — callers
    // (Pool) count the fallback instead of growing silently.
    void* alloc(std::size_t n, std::size_t align = 16) noexcept {
        std::uintptr_t cur = reinterpret_cast<std::uintptr_t>(data()) + off_;
        std::uintptr_t al = (cur + align - 1) / align * align;
        std::size_t next = al - reinterpret_cast<std::uintptr_t>(data()) + n;
        if (next > size()) return nullptr;
        off_ = next;
        return reinterpret_cast<void*>(al);
    }

    std::byte* data() noexcept { return region_.data(); }
    const std::byte* data() const noexcept { return region_.data(); }
    std::size_t size() const noexcept { return region_.size(); }
    std::size_t used() const noexcept { return off_; }
    std::size_t remaining() const noexcept { return size() - off_; }
    bool owns(const void* p) const noexcept {
        auto* b = reinterpret_cast<const std::byte*>(p);
        return b >= data() && b < data() + size();
    }
    bool hugepages() const noexcept { return region_.hugepages(); }
    explicit operator bool() const noexcept { return region_.operator bool(); }

  private:
    numa::Region region_;
    std::size_t off_ = 0;
};

}  // namespace afx

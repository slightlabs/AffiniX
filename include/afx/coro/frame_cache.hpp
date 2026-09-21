#pragma once

// coro/frame_cache.hpp — size-bucketed frame cache over the EM arena
// (spike 0002, M9-01). Coroutine frames are fixed-size per promise type and
// recycled aggressively; a bump arena alone would leak one frame per
// session. The cache carves 4 KiB chunks from the arena, slices them into
// class-sized blocks, and free-lists them — arena footprint tracks the peak
// live-frame working set, not cumulative churn.
//
// Arena exhaustion (or an oversized frame) falls back to ::operator new —
// visible via heap_fallbacks(), never a silent failure.
//
// EM-thread only: the owning EM is the only thread allowed to alloc/free.

#include <cstddef>
#include <cstdint>
#include <new>

#include "afx/core/arena.hpp"

namespace afx::coro {

class FrameCache {
    // Block header sits just before the returned pointer; free-list links
    // live in the dead user area so the header survives recycling. 16 bytes
    // keeps user pointers 16-aligned for over-aligned frame members.
    struct BlockHeader {
        std::uint16_t cls;  // bucket index, or kHeap
        std::uint16_t pad;
        std::uint32_t magic;  // catches cross-cache/corrupt frees
        std::uint64_t pad2;
    };
    static constexpr std::uint32_t kMagic = 0x46524D43;  // "FRMC"
    static constexpr std::uint16_t kHeap = 0xFFFF;

    // Free-list link stored at the start of the (dead) user area.
    struct FreeBlock {
        FreeBlock* next;
    };

  public:
    static constexpr std::size_t kGranularity = 64;
    static constexpr std::size_t kBuckets = 16;       // classes 64..1024 B
    static constexpr std::size_t kChunkBytes = 4096;  // per arena carve

    FrameCache() = default;
    explicit FrameCache(Arena* arena) : arena_(arena) {}

    void bind(Arena* arena) noexcept { arena_ = arena; }

    void* alloc(std::size_t n) noexcept {
        const std::size_t need = n + sizeof(BlockHeader);
        const std::size_t cls = (need + kGranularity - 1) / kGranularity;
        if (cls > kBuckets) return heap_block(need);
        if (FreeBlock* u = free_[cls - 1]) {
            free_[cls - 1] = u->next;
            return u;
        }
        return carve(cls);
    }

    // `p` is a pointer returned by alloc(); heap blocks are marked in the
    // header so both kinds route correctly.
    void free(void* p) noexcept {
        if (!p) return;
        auto* hdr = static_cast<BlockHeader*>(p) - 1;
        if (hdr->magic != kMagic) {  // corrupted: do not touch the arena
            ++corrupt_;
            return;
        }
        if (hdr->cls == kHeap) {
            ::operator delete(hdr);
            return;
        }
        auto* u = static_cast<FreeBlock*>(p);
        u->next = free_[hdr->cls];
        free_[hdr->cls] = u;
    }

    std::size_t heap_fallbacks() const noexcept { return heap_fallbacks_; }
    std::size_t corrupt_frees() const noexcept { return corrupt_; }

  private:
    void* heap_block(std::size_t need) noexcept {
        ++heap_fallbacks_;
        auto* hdr = static_cast<BlockHeader*>(::operator new(need));
        hdr->cls = kHeap;
        hdr->pad = 0;
        hdr->magic = kMagic;
        return hdr + 1;
    }

    void* carve(std::size_t cls) noexcept {
        const std::size_t block = cls * kGranularity;
        void* mem = arena_ ? arena_->alloc(kChunkBytes, 16) : nullptr;
        if (!mem) return heap_block(block);
        auto* base = static_cast<std::byte*>(mem);
        const std::size_t count = kChunkBytes / block;
        for (std::size_t i = 0; i < count; ++i) {
            auto* hdr = reinterpret_cast<BlockHeader*>(base + i * block);
            hdr->cls = std::uint16_t(cls - 1);
            hdr->pad = 0;
            hdr->magic = kMagic;
            auto* u = reinterpret_cast<FreeBlock*>(hdr + 1);
            u->next = free_[cls - 1];
            free_[cls - 1] = u;
        }
        return alloc_block_user(cls);
    }

    // Pop one block of `cls` — carve() preloaded the list.
    void* alloc_block_user(std::size_t cls) noexcept {
        FreeBlock* u = free_[cls - 1];
        free_[cls - 1] = u->next;
        return u;
    }

    Arena* arena_ = nullptr;
    FreeBlock* free_[kBuckets] = {};
    std::size_t heap_fallbacks_ = 0;
    std::size_t corrupt_ = 0;
};

}  // namespace afx::coro

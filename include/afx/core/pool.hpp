#pragma once

// Pool — DESIGN.md §12.2, M6-02. A slab allocator over fixed chunks carved
// from an Arena (or operator new when the arena is exhausted — counted, never
// silent). Slots come back through a free list; destruction is explicit via
// destroy() so callers keep the deferred-reclamation discipline of §13.

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace afx {

template <class T>
class Pool {
    union Slot {
        T obj;
        Slot* next;
    };
    struct Chunk {
        Chunk* next;
        // Slot storage follows the header.
        alignas(T) unsigned char storage[sizeof(Slot)];
    };

  public:
    // arena may be null or exhausted: chunks then come from operator new and
    // heap_chunks_ records it (visible accounting, §2: no hidden costs).
    explicit Pool(Arena* arena = nullptr, std::size_t objs_per_chunk = 64)
        : arena_(arena), chunk_objs_(objs_per_chunk > 0 ? objs_per_chunk : 1) {}

    ~Pool() {
        // Live objects are the owner's business (deferred reclamation is the
        // caller's contract, §13); the pool only frees chunk memory.
        Chunk* c = heap_chunks_;
        while (c) {
            Chunk* n = c->next;
            delete[] reinterpret_cast<unsigned char*>(c);
            c = n;
        }
    }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    template <class... A>
    T* construct(A&&... a) {
        if (!free_) grow();
        if (!free_) return nullptr;  // OOM is the only failure left
        Slot* s = free_;
        free_ = s->next;
        T* p = &s->obj;
        new (p) T(std::forward<A>(a)...);
        ++live_;
        return p;
    }

    void destroy(T* p) noexcept {
        if (!p) return;
        p->~T();
        Slot* s = reinterpret_cast<Slot*>(p);
        s->next = free_;
        free_ = s;
        --live_;
    }

    std::size_t allocated() const noexcept { return live_; }
    std::size_t capacity() const noexcept {
        return arena_chunks_ * chunk_objs_ + heap_chunk_count_ * chunk_objs_;
    }
    std::size_t heap_chunks() const noexcept { return heap_chunk_count_; }

  private:
    void grow() {
        std::size_t hdr = sizeof(Chunk) - sizeof(((Chunk*)nullptr)->storage);
        std::size_t bytes = hdr + chunk_objs_ * sizeof(Slot);
        unsigned char* mem = nullptr;
        bool heap = false;
        if (arena_)
            mem = static_cast<unsigned char*>(arena_->alloc(bytes, alignof(T)));
        if (!mem) {
            mem = new unsigned char[bytes];
            heap = true;
        }
        auto* c = reinterpret_cast<Chunk*>(mem);
        if (heap) {
            c->next = heap_chunks_;
            heap_chunks_ = c;
            ++heap_chunk_count_;
        } else {
            ++arena_chunks_;  // arena chunks need no bookkeeping list
        }
        auto* s = reinterpret_cast<Slot*>(c->storage);
        for (std::size_t i = 0; i < chunk_objs_; ++i) {
            s[i].next = free_;
            free_ = &s[i];
        }
    }

    Arena* arena_;
    std::size_t chunk_objs_;
    Slot* free_ = nullptr;
    Chunk* heap_chunks_ = nullptr;
    std::size_t live_ = 0;
    std::size_t arena_chunks_ = 0;
    std::size_t heap_chunk_count_ = 0;
};

}  // namespace afx

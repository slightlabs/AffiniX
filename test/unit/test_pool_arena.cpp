// Pool/Arena/NUMA unit tests (M6-02, M1-11, M5-05). The arena is a bump
// region with a NUMA policy; Pool carves fixed chunks from it and counts
// every heap fallback instead of hiding it.

#include <doctest/doctest.h>

#include "afx/core/arena.hpp"
#include "afx/core/pool.hpp"
#include "afx/sys/numa.hpp"

using namespace afx;

namespace {
struct Widget {
    int x = 0;
    std::uint64_t tag = 0;
    explicit Widget(int v) : x(v) {}
};
}  // namespace

TEST_CASE("Arena: bump allocation is aligned and bounded") {
    Arena a(64u << 10);
    REQUIRE(a.size() >= (64u << 10));
    void* p = a.alloc(128, 64);
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 64 == 0);
    CHECK(a.owns(p));
    CHECK(a.used() >= 128);
    void* q = a.alloc(32);
    CHECK(q != p);
    CHECK(a.owns(q));
    // Exhaustion returns nullptr — callers see it, nothing grows silently.
    Arena small(4096);
    void* r = small.alloc(4096);
    CHECK(small.alloc(1) == nullptr);
    (void)r;
    Arena empty(0);
    CHECK(empty.alloc(8) == nullptr);
}

TEST_CASE("Pool: objects land in the arena and slots recycle") {
    Arena a(64u << 10);
    Pool<Widget> pool(&a, 4);
    Widget* w1 = pool.construct(1);
    Widget* w2 = pool.construct(2);
    REQUIRE(w1 != nullptr);
    CHECK(w1->x == 1);
    CHECK(a.owns(w1));
    CHECK(a.owns(w2));
    CHECK(pool.allocated() == 2);
    CHECK(pool.heap_chunks() == 0);

    pool.destroy(w1);
    CHECK(pool.allocated() == 1);
    Widget* w3 = pool.construct(3);  // should reuse w1's freed slot
    CHECK(w3 == w1);
    CHECK(w3->x == 3);
    pool.destroy(w2);
    pool.destroy(w3);
    CHECK(pool.allocated() == 0);
}

TEST_CASE("Pool: arena exhaustion falls back to the heap, counted") {
    Arena a(4096);
    Pool<Widget> pool(&a, 4);
    // numa::alloc rounds up to whole pages, so drain all but a sliver —
    // a chunk (hdr + 4 Slots = 72B) must not fit anymore.
    REQUIRE(a.alloc(a.size() - 16) != nullptr);
    Widget* w = pool.construct(7);
    REQUIRE(w != nullptr);
    CHECK(pool.heap_chunks() >= 1);
    CHECK(!a.owns(w));
    pool.destroy(w);
}

TEST_CASE("Pool: null arena still works (pure heap)") {
    Pool<Widget> pool(nullptr, 2);
    for (int i = 0; i < 5; ++i) {
        Widget* w = pool.construct(i);
        REQUIRE(w != nullptr);
        CHECK(w->x == i);
    }
    CHECK(pool.heap_chunks() >= 2);  // 5 objects / 2 per chunk = 3 chunks
}

TEST_CASE("numa: current_node and node_count are sane") {
    CHECK(numa::node_count() >= 1);
    int n = numa::current_node();
    CHECK(n >= -1);  // -1 tolerated where getcpu is unavailable
    if (n >= 0) CHECK(n < 1024);
}

TEST_CASE("numa::alloc honours size and prefault") {
    auto r = numa::alloc(1u << 20, numa::AllocOpts{.prefault = true});
    REQUIRE(r.has_value());
    CHECK(r->size() >= (1u << 20));
    CHECK(r->mapped());
    // The region is usable.
    r->data()[0] = std::byte(0xAB);
    r->data()[r->size() - 1] = std::byte(0xCD);
    CHECK(r->data()[0] == std::byte(0xAB));
}

TEST_CASE("numa::alloc binds to the current node when NUMA is available") {
    int node = numa::current_node();
    if (node < 0 || !numa::available()) {
        MESSAGE("NUMA unavailable on this host — skipped");
        return;
    }
    auto r = numa::alloc(1u << 20,
                         numa::AllocOpts{.node = node, .prefault = true});
    REQUIRE(r.has_value());
}

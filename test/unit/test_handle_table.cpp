#include <doctest/doctest.h>

#include "afx/core/handle_table.hpp"

using namespace afx;

struct Widget { int v; };
using WidgetId = Handle<struct WidgetTag>;

TEST_CASE("HandleTable insert/get/release") {
    HandleTable<Widget, WidgetId> t;
    auto [id, w] = t.emplace();
    w->v = 7;
    CHECK(id.valid());
    CHECK(t.get(id)->v == 7);
    CHECK(t.contains(id));
    CHECK(t.size() == 1);

    t.release(id);
    CHECK(t.get(id) == nullptr);       // gen bumped at once
    CHECK(!t.contains(id));
}

TEST_CASE("stale handles never alias a new object") {
    HandleTable<Widget, WidgetId> t;
    auto [a, wa] = t.emplace();
    wa->v = 1;
    t.release(a);
    t.reclaim();                        // slot returns to the free list

    auto [b, wb] = t.emplace();        // reuses slot 0
    wb->v = 2;
    CHECK(b.idx == a.idx);
    CHECK(b.gen != a.gen);
    CHECK(t.get(a) == nullptr);        // stale id does not alias
    CHECK(t.get(b)->v == 2);
}

TEST_CASE("double release is a no-op") {
    HandleTable<Widget, WidgetId> t;
    auto [id, _] = t.emplace();
    t.release(id);
    t.release(id);
    t.reclaim();
    CHECK(t.size() == 0);
}

TEST_CASE("reclaim is deferred until called") {
    HandleTable<Widget, WidgetId> t;
    auto [a, _] = t.emplace();
    t.release(a);
    // slot not yet reusable
    auto [b, _2] = t.emplace();
    CHECK(b.idx != a.idx);
    t.reclaim();
    auto [c, _3] = t.emplace();
    CHECK(c.idx == a.idx);
}

TEST_CASE("for_each visits live objects only") {
    HandleTable<Widget, WidgetId> t;
    t.emplace().second->v = 1;
    t.emplace().second->v = 2;
    auto [dead, _] = t.emplace();
    dead = dead;
    t.release(dead);
    int sum = 0;
    t.for_each([&](Widget& w, WidgetId) { sum += w.v; });
    CHECK(sum == 3);
}

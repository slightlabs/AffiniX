#include <doctest/doctest.h>

#include "afx/core/context.hpp"

using namespace afx;

TEST_CASE("Deadline basics") {
    Deadline none;
    CHECK(!none.is_set());
    CHECK(!none.expired(TimePoint{}));
    CHECK(none.remaining(TimePoint{}) == Duration::max());

    TimePoint t0(1000ms);
    Deadline d = Deadline::at(t0 + 5ms);
    CHECK(d.is_set());
    CHECK(!d.expired(t0));
    CHECK(d.expired(t0 + 5ms));
    CHECK(d.remaining(t0) == 5ms);
    CHECK(d.remaining(t0 + 10ms) == Duration::zero());
}

TEST_CASE("Deadline::earliest_of only tightens") {
    Deadline a = Deadline::at(TimePoint(100ms));
    Deadline b = Deadline::at(TimePoint(50ms));
    Deadline none;

    CHECK(a.earliest_of(b).point() == TimePoint(50ms));
    CHECK(b.earliest_of(a).point() == TimePoint(50ms));
    CHECK(a.earliest_of(none).point() == TimePoint(100ms));
    CHECK(none.earliest_of(b).point() == TimePoint(50ms));
    CHECK(!none.earliest_of(Deadline{}).is_set());
}

TEST_CASE("TraceId short form is stable") {
    TraceId t{0x1234, 0x5678, 9};
    CHECK(t.valid());
    CHECK(t.short_id() == (0x1234 ^ 0x5678));
    TraceId empty;
    CHECK(!empty.valid());
}

TEST_CASE("StopToken cooperative cancellation") {
    StopSource src;
    StopToken tok = src.token();
    CHECK(tok);
    CHECK(!tok.stop_requested());
    src.request();
    CHECK(tok.stop_requested());

    StopToken empty;
    CHECK(!empty);
    CHECK(!empty.stop_requested());
}

TEST_CASE("ambient_context defaults to empty") {
    CHECK(!detail::ambient_context().deadline.is_set());
    CHECK(detail::ambient_context().priority == 0);
}

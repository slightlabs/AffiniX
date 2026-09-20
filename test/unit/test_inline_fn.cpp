#include <doctest/doctest.h>

#include <array>
#include <memory>
#include <string>

#include "afx/sys/inline_fn.hpp"

using namespace afx;

TEST_CASE("InlineFn stores small callables inline") {
    InlineFn<void(), 48> f = [x = 3]() mutable { ++x; };
    CHECK(f);
    CHECK(!f.heap_allocated());
    f();
    f();
}

TEST_CASE("InlineFn invocation returns values") {
    InlineFn<int(int), 48> f = [](int v) { return v * 2; };
    CHECK(f(21) == 42);
}

TEST_CASE("InlineFn heap fallback for large callables") {
    std::array<std::byte, 256> big{};
    InlineFn<void(), 16> f = [big]() mutable {};
    CHECK(f);
    CHECK(f.heap_allocated());
}

TEST_CASE("InlineFn is move-only and moves correctly") {
    static_assert(!std::is_copy_constructible_v<InlineFn<void()>>);
    int hits = 0;
    InlineFn<void(), 48> a = [&] { ++hits; };
    InlineFn<void(), 48> b = std::move(a);
    CHECK(!a);
    CHECK(b);
    b();
    CHECK(hits == 1);
}

TEST_CASE("InlineFn destruction runs the callable's destructor") {
    auto sp = std::make_shared<int>(0);
    {
        InlineFn<void(), 48> f = [sp] {};
        CHECK(sp.use_count() == 2);
        f();
        CHECK(sp.use_count() == 2);  // callable survives invocation
    }
    CHECK(sp.use_count() == 1);  // destroyed with the InlineFn

    {
        // heap path
        std::array<std::byte, 256> big{};
        InlineFn<void(), 8> f = [sp, big] {};
        CHECK(sp.use_count() == 2);
    }
    CHECK(sp.use_count() == 1);
}

TEST_CASE("InlineFn empty and reset") {
    InlineFn<void(), 48> f;
    CHECK(!f);
    f = [] {};
    CHECK(f);
    f.reset();
    CHECK(!f);
}

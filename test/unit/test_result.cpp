#include <doctest/doctest.h>

#include <memory>

#include "afx/sys/result.hpp"

using namespace afx;

TEST_CASE("Error is 4 bytes and categories work") {
    static_assert(sizeof(Error) == 4);
    Error e = make_error(ErrorCategory::Net, Err::ConnectFailed);
    CHECK(!e.ok());
    CHECK(e.category == ErrorCategory::Net);
    CHECK(e.code == std::uint16_t(Err::ConnectFailed));
    CHECK(e.message().size() > 0);

    Error ok{};
    CHECK(ok.ok());
}

TEST_CASE("Result<T> value and error paths") {
    Result<int> ok = 42;
    CHECK(ok.has_value());
    CHECK(*ok == 42);
    CHECK(ok.value_or(7) == 42);

    Result<int> bad = make_error(ErrorCategory::Sys, Err::Full);
    CHECK(!bad.has_value());
    CHECK(bad.error().code == std::uint16_t(Err::Full));
    CHECK(bad.value_or(7) == 7);
}

TEST_CASE("Result<void>") {
    Result<void> ok;
    CHECK(ok.has_value());
    Result<void> bad = make_error(ErrorCategory::Itc, Err::Full);
    CHECK(!bad.has_value());
    CHECK(bad.error().category == ErrorCategory::Itc);
}

TEST_CASE("Result move semantics") {
    Result<std::unique_ptr<int>> r = std::make_unique<int>(5);
    auto r2 = std::move(r);
    CHECK(r2.has_value());
    CHECK(*r2.value() == 5);
}

static Result<int> might_fail(bool fail) {
    if (fail) return make_error(ErrorCategory::Config, Err::Invalid);
    return 9;
}
static Result<int> caller(bool fail) {
    AFX_TRY(int v, might_fail(fail));
    return v + 1;
}

TEST_CASE("AFX_TRY propagates errors") {
    CHECK(*caller(false) == 10);
    auto r = caller(true);
    CHECK(!r.has_value());
    CHECK(r.error().category == ErrorCategory::Config);
}

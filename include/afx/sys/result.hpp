#pragma once

// DESIGN.md §18: Result<T>. std::expected is C++23; this is a compact C++20
// equivalent with the pieces the framework actually uses.

#include <cassert>
#include <type_traits>
#include <utility>

#include "afx/sys/error.hpp"

namespace afx {

namespace detail {
template <class T>
struct ResultStorage {
    union U {
        T val;
        Error err;
        U() : err{} {}
        ~U() {}
        U(const U& o) : err(o.err) {}
        U& operator=(const U& o) {
            err = o.err;
            return *this;
        }
    } u;
    bool has = false;

    ResultStorage() = default;
    ~ResultStorage() {
        if (has) u.val.~T();
    }
    ResultStorage(const ResultStorage& o) : has(o.has) {
        if (has)
            new (&u.val) T(o.u.val);
        else
            u.err = o.u.err;
    }
    ResultStorage(ResultStorage&& o) noexcept(
        std::is_nothrow_move_constructible_v<T>)
        : has(o.has) {
        if (has)
            new (&u.val) T(std::move(o.u.val));
        else
            u.err = o.u.err;
    }
    ResultStorage& operator=(const ResultStorage& o) {
        if (this == &o) return *this;
        if (has) u.val.~T();
        has = o.has;
        if (has)
            new (&u.val) T(o.u.val);
        else
            u.err = o.u.err;
        return *this;
    }
    ResultStorage& operator=(ResultStorage&& o) noexcept(
        std::is_nothrow_move_constructible_v<T>) {
        if (this == &o) return *this;
        if (has) u.val.~T();
        has = o.has;
        if (has)
            new (&u.val) T(std::move(o.u.val));
        else
            u.err = o.u.err;
        return *this;
    }
};
}  // namespace detail

template <class T>
class Result {
  public:
    Result() : s_{} {
        s_.has = true;
        new (&s_.u.val) T();
    }
    Result(const T& v) {
        s_.has = true;
        new (&s_.u.val) T(v);
    }
    Result(T&& v) {
        s_.has = true;
        new (&s_.u.val) T(std::move(v));
    }
    Result(Error e) { s_.u.err = e; }
    Result(Err e) { s_.u.err = make_error(ErrorCategory::Internal, e); }

    bool has_value() const noexcept { return s_.has; }
    explicit operator bool() const noexcept { return s_.has; }

    T& value() & noexcept {
        assert(s_.has);
        return s_.u.val;
    }
    const T& value() const& noexcept {
        assert(s_.has);
        return s_.u.val;
    }
    T&& value() && noexcept {
        assert(s_.has);
        return std::move(s_.u.val);
    }
    T& operator*() & noexcept { return value(); }
    const T& operator*() const& noexcept { return value(); }
    T* operator->() noexcept { return &value(); }
    const T* operator->() const noexcept { return &value(); }

    Error error() const noexcept {
        assert(!s_.has);
        return s_.u.err;
    }

    template <class U>
    T value_or(U&& u) const& {
        return s_.has ? s_.u.val : T(std::forward<U>(u));
    }

  private:
    detail::ResultStorage<T> s_;
};

template <>
class Result<void> {
  public:
    Result() : err_{} {}
    Result(Error e) : err_(e) {}
    Result(Err e) : err_(make_error(ErrorCategory::Internal, e)) {}

    bool has_value() const noexcept { return err_.ok(); }
    explicit operator bool() const noexcept { return err_.ok(); }
    void value() const noexcept { assert(err_.ok()); }
    Error error() const noexcept {
        assert(!err_.ok());
        return err_;
    }

  private:
    Error err_;
};

// AFX_TRY: evaluate expr (a Result<T>); on failure return the error from the
// enclosing function. Names a temporary so the result stays usable.
#define AFX_TRY_CONCAT_(a, b) a##b
#define AFX_TRY_CONCAT(a, b) AFX_TRY_CONCAT_(a, b)
#define AFX_TRY(decl, expr)                                 \
    auto&& AFX_TRY_CONCAT(_afx_res_, __LINE__) = (expr);    \
    if (!AFX_TRY_CONCAT(_afx_res_, __LINE__))               \
        return AFX_TRY_CONCAT(_afx_res_, __LINE__).error(); \
    decl = std::move(*AFX_TRY_CONCAT(_afx_res_, __LINE__))

}  // namespace afx

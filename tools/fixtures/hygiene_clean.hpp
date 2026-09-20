#pragma once

// Fixture: the same shape as hygiene_violation.hpp, but detail:: never
// escapes a private section or the detail namespace itself.

namespace afx {
namespace detail {
struct Impl {
    int value = 0;
};

// Calls to detail:: functions/types from within namespace detail are fine —
// this is the detail namespace, not a leak from it.
inline int read(const Impl& i) { return i.value; }
} // namespace detail

class Widget {
public:
    int state() const { return state_.value; }
    void set(int v) { state_.value = v; }

private:
    // A detail:: type is fine here: it's private, not part of the API.
    detail::Impl state_{};
};

} // namespace afx

#pragma once

// Deliberate fixture: a detail:: type leaking into a public signature, used
// by test_hygiene_scripts.py to prove check_header_hygiene.py actually
// catches what it claims to.

namespace afx {
namespace detail {
struct Impl {
    int value = 0;
};
} // namespace detail

class Widget {
public:
    // Violation: detail::Impl (a return type) is not part of the public API.
    detail::Impl& state() { return state_; }

    // Violation: a public data member of a detail:: type.
    detail::Impl exposed;

private:
    detail::Impl state_{};
};

} // namespace afx

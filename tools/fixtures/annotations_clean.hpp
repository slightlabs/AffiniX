#pragma once

// Fixture: the same shape as annotations_violation.hpp, but annotated.

#include "afx/sys/annotations.hpp"

namespace afx {

class Gadget {
public:
    void spin() AFX_REQUIRES(cap_) {
        AFX_ASSERT_CURRENT(*this);
        // ...
    }

    // A public EM-affine member may also opt out explicitly.
    void legacy_spin() AFX_NO_TSA {
        AFX_ASSERT_CURRENT(*this);
    }

private:
    bool is_current() const noexcept { return true; }
    EmContext cap_;
};

} // namespace afx

#pragma once

// Deliberate fixture: a public, EM-affine method with no AFX_REQUIRES/
// AFX_NO_TSA, used by test_hygiene_scripts.py to prove
// check_annotations.py actually catches what it claims to.

#include "afx/sys/annotations.hpp"

namespace afx {

class Gadget {
public:
    void spin() {
        AFX_ASSERT_CURRENT(*this);
        // ...
    }

private:
    bool is_current() const noexcept { return true; }
};

} // namespace afx

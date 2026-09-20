// Positive control for the must-not-compile suite (M1-02): this file MUST
// compile under -Wthread-safety. If it ever fails, the enforcement machinery
// is broken rather than the negative cases passing trivially — a lint script
// that cannot demonstrate its own failure mode is a placebo.

#include "cases.hpp"

int main() {
    afx::test::AffineCell c;
    {
        afx::test::ScopedHold hold(c.cap_);
        c.touch();
        c.guarded = 1;
    }
    c.release();  // legal once the hold is dropped
    return 0;
}

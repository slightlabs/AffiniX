// Must-NOT-compile case (M1-02): an AFX_EXCLUDES(cap_) entry point — the
// loop-side boundary that asserts the capability itself — must reject a call
// made while the capability is already held.

#include "cases.hpp"

int main() {
    afx::test::AffineCell c;
    afx::test::ScopedHold hold(c.cap_);
    c.release();  // excludes c.cap_
    return 0;
}

// Must-NOT-compile case (M1-02): an AFX_REQUIRES(cap_) member called with no
// capability held is an affinity violation — the analysis must reject it.

#include "cases.hpp"

int main() {
    afx::test::AffineCell c;
    c.touch();  // requires holding c.cap_
    return 0;
}

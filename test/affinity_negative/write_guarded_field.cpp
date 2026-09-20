// Must-NOT-compile case (M1-02): writing an AFX_GUARDED_BY field without the
// guarding capability is an affinity violation.

#include "cases.hpp"

int main() {
    afx::test::AffineCell c;
    c.guarded = 42;  // guarded by c.cap_
    return 0;
}

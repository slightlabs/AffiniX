#pragma once

// Shared fixture for the must-not-compile suite (M1-02, ADR-0009).
// AffineCell models the shape the framework uses: an EmContext capability
// member stands in for "the EM's thread", affine methods are marked
// AFX_REQUIRES(cap_), and the loop asserts the capability by holding a
// ScopedHold across the bodies that dispatch affine work.

#include "afx/sys/annotations.hpp"

namespace afx::test {

// Acquire/release scope over an EmContext token. Holding one is how code
// proves to the analysis that it runs on the EM's thread.
class AFX_SCOPED_CAPABILITY ScopedHold {
  public:
    explicit ScopedHold(EmContext& c) AFX_ACQUIRE(c) : c_(c) {}
    ~ScopedHold() AFX_RELEASE() {}
    ScopedHold(const ScopedHold&) = delete;
    ScopedHold& operator=(const ScopedHold&) = delete;

  private:
    EmContext& c_;
};

struct AffineCell {
    EmContext cap_;
    int guarded AFX_GUARDED_BY(cap_) = 0;

    void touch() AFX_REQUIRES(cap_) { ++guarded; }
    void release() AFX_EXCLUDES(cap_) {}
};

}  // namespace afx::test

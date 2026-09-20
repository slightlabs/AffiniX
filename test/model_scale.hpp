#pragma once

// Scale factor for long randomized sweeps (model/fuzz tests). Sanitizer
// builds run the same seeds with fewer steps — state-space coverage comes
// from step diversity, not raw count — keeping PR-gate jobs bounded while
// normal builds and nightly runs (AFX_MODEL_SCALE=1+, or higher) keep the
// full sweep.

#include <cstdlib>

namespace afx::test {

inline double model_scale() {
    if (const char* s = std::getenv("AFX_MODEL_SCALE"))
        if (double d = std::atof(s); d > 0) return d;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && \
     (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer)))
    return 0.2;
#else
    return 1.0;
#endif
}

} // namespace afx::test

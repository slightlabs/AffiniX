#pragma once

// Compile-time affinity annotations (DESIGN.md §6.3, ADR-0009).
//
// Under Clang with -Wthread-safety these map onto the thread-safety analysis
// attributes, treating the EventManager's EmContext as a capability. On GCC —
// and on Clang builds without AFX_THREAD_SAFETY_ANALYSIS — they expand to
// nothing, which is the documented behaviour: the enforcement point is the
// dedicated Clang CI job, plus debug `is_current()` asserts everywhere else.

#if defined(__clang__) && defined(AFX_THREAD_SAFETY_ANALYSIS)
#  include <mutex>
#  define AFX_CAPABILITY(x)    __attribute__((capability(x)))
#  define AFX_REQUIRES(...)    __attribute__((requires_capability(__VA_ARGS__)))
#  define AFX_EXCLUDES(...)    __attribute__((locks_excluded(__VA_ARGS__)))
#  define AFX_GUARDED_BY(x)    __attribute__((guarded_by(x)))
#  define AFX_ACQUIRE(...)     __attribute__((acquire_capability(__VA_ARGS__)))
#  define AFX_RELEASE(...)     __attribute__((release_capability(__VA_ARGS__)))
#  define AFX_NO_TSA           __attribute__((no_thread_safety_analysis))
#else
#  define AFX_CAPABILITY(x)
#  define AFX_REQUIRES(...)
#  define AFX_EXCLUDES(...)
#  define AFX_GUARDED_BY(x)
#  define AFX_ACQUIRE(...)
#  define AFX_RELEASE(...)
#  define AFX_NO_TSA
#endif

namespace afx {

// Never locked; a pure token naming the EventManager capability (§6.3).
class AFX_CAPABILITY("em") EmContext {};

// Debug-build assertion hook for affine entry points. Kept as a macro so it
// costs nothing in release and reports the call site when it fires.
#ifdef AFX_DEBUG
#  include <cassert>
#  define AFX_ASSERT_CURRENT(em) assert((em).is_current() && "EM-affine call off-thread")
#else
#  define AFX_ASSERT_CURRENT(em) ((void)0)
#endif

} // namespace afx

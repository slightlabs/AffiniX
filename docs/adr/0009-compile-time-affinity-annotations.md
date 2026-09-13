# ADR-0009: Thread affinity is checked at compile time where the toolchain allows

**Status:** accepted
**Date:** 2026-09-13
**One-way door:** yes (coverage must be total from the start)

## Context

The framework's central invariant is that an EM's objects are touched only by
the EM's thread. Violating it is a data race — the failure mode is rare,
non-deterministic corruption in production, which is the worst kind of bug and
exactly the kind users will blame on the framework.

Three enforcement mechanisms are available, and they catch different things:

| Mechanism | Catches | When |
|---|---|---|
| `assert(is_current())` | any direct misuse that executes | run time, debug |
| Generation-checked handles (ADR-0003) | stale/foreign references | run time, always |
| Clang thread-safety analysis | direct calls from the wrong context | **compile time** |

The third is the only one that finds the bug before it ships, and it costs
nothing at run time. It is, however, only useful if annotation coverage is
complete: a user who sees a clean build with 70 % coverage has been given false
confidence, which is worse than no checking at all. Coverage can only
realistically be complete if annotations go in with the first headers.

## Decision

Model the EM as a Clang *capability* and annotate every non-thread-safe member
of `EventManager`, `Connection`, `TcpServer`, `TcpClient`, `TimerWheel` and the
pools with `AFX_REQUIRES(em_)`. The thread-safe three (`post`, `stop`,
`mailbox`) carry no requirement. Details in DESIGN.md §6.3.

The macros live in `afx/sys/annotations.hpp` and expand to
`__attribute__((...))` under Clang and to nothing otherwise. A dedicated CI job
builds with Clang and `-Wthread-safety -Werror`; a `test/affinity_negative/`
suite asserts that known violations **fail** to compile; an
annotation-coverage script rejects new public non-thread-safe members that lack
an annotation.

All three mechanisms above are kept. They are complementary: the analysis is
intra-procedural and cannot see through type erasure or a runtime-resolved
handle, which is exactly where handles and the debug asserts do the work.

## Alternatives rejected

- **Run-time asserts only.** The status quo of most frameworks. Catches the bug
  on the developer's machine only if the wrong-thread path is actually
  exercised in a debug build — and affinity bugs are usually in rarely-taken
  error or shutdown paths.
- **TSAN only.** Finds real races, but needs the race to execute during the
  test, slows execution 5–15×, and cannot run on the spinning/`SCHED_FIFO`
  configurations where this framework is most used. Kept in CI as a
  complement, not as the primary mechanism.
- **A custom static analyser / clang-tidy check.** More capable in principle
  (could follow capabilities through handles), and much more to build and
  maintain. Revisit only if the built-in analysis proves insufficient (open
  question 9).
- **Types that make misuse impossible** — e.g. requiring an `EmToken&`
  parameter on every affine method. Genuinely stronger, and it deforms every
  signature and every call site in the public API for a benefit the annotations
  provide for free on the compiler that supports them. Rejected as too high a
  tax on ergonomics.

## Consequences

- Clang users get compile-time enforcement; GCC users get the run-time
  mechanisms only. This asymmetry is documented rather than hidden, and CI's
  Clang job is what protects GCC users from annotation rot.
- Annotations appear in public headers, so they are part of the API's
  appearance if not its semantics. They are macro-wrapped so they can be
  removed or redefined without touching declarations.
- Contributors must annotate new methods. Enforced by script, not by review
  memory.
- One more CI job and one more macro header.

## Tripwire

Revisit if the analysis produces false positives that force widespread
`AFX_NO_THREAD_SAFETY_ANALYSIS` escapes (more than a handful across the
framework means the model does not fit and the annotations are becoming
decoration), or if annotation coverage cannot be kept at 100 % of public
non-thread-safe members — in which case the honest move is to remove the
mechanism entirely rather than ship partial checking that users will trust.

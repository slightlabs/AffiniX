# ADR-0001: Clock is a policy type; virtual time exists from day one

**Status:** accepted
**Date:** 2026-09-13
**One-way door:** yes

## Context

The framework's core features are timers, and timers are the hardest thing to
test honestly. If `EventManager` calls `std::chrono::steady_clock::now()`
directly, then every timer test must sleep in real time. Such suites are slow,
flaky under CI load, and — worse — nobody writes the interesting tests (a
one-hour timer, a 49-day wheel rollover, a repeating timer that overruns) at
all.

Retrofitting a clock seam later is impractical: by then `now()` calls are
scattered across the wheel, the connection idle logic, the backoff, the stats
and the backend timeouts.

## Decision

`Clock` is a policy type resolved at compile time. `EventManager` obtains all
time through it, caches `now()` once per loop iteration, and exposes
`em.now()`. Two implementations ship together in milestone 1:

- `SteadyClock` — `clock_gettime(CLOCK_MONOTONIC)`, with an optional TSC fast
  path for the spin loop.
- `VirtualClock` — time advances only when a test says so; `advance(d)` fires
  every timer due in the interval, in order, without sleeping.

The backend wait timeout is derived from the clock too, so `SimBackend` +
`VirtualClock` compose into fully deterministic execution (ADR-0002, §22.3).

## Alternatives rejected

- **Direct `steady_clock` calls.** Simplest, and forecloses deterministic
  testing permanently.
- **Runtime-injected `Clock*` interface.** Works, but puts a virtual call on a
  path taken once per iteration *and* in every timeout computation, and it
  cannot be optimised away in release builds.
- **Mocking at the syscall level** (LD_PRELOAD, `libfaketime`). Fragile,
  platform-specific, and does not give the test control over ordering.

## Consequences

- `EventManager` is a template on `Clock` (with `afx::EventManager` aliasing
  the steady instantiation, so users never see it).
- Any code inside the framework that wants the time must take it from the EM;
  calling `steady_clock` directly is a review error and is caught by a
  clang-tidy check on the framework's own sources.
- Timer tests run in microseconds and are deterministic.
- Cost: one template parameter threaded through the core types, and a discipline
  requirement on contributors.

## Tripwire

Revisit if the template parameter measurably harms compile times (> 20 % on the
examples build) *and* a runtime-dispatched clock proves free in release
benchmarks — or if `VirtualClock` turns out not to be sufficient for testing
the I/O paths, in which case the seam needs to be wider rather than removed.

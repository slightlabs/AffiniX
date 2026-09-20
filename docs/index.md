# AffiniX

A C++20 framework for thread-affine, event-driven applications.

> A thread is the unit of concurrency, and an `EventManager` **is** that
> thread's execution context. Nothing is shared implicitly. Cross-thread
> interaction is always an explicit message.

## Highlights

- One `EventManager` per thread, with no implicit thread hopping
- Pluggable I/O backends: epoll and io_uring (auto-selected at runtime)
- Proactor-shaped completion model across all backends
- Deterministic simulation: `VirtualClock` + `SimBackend` drive an
  `EventManager` with no real time, sockets, or threads
- Timer wheel with group cancel, FixedRate/FixedDelay repeats, and
  coalesced-overrun reporting

## Documentation

- [Design](DESIGN.md) — architecture, invariants, and subsystem contracts
- [Implementation Plan](IMPLEMENTATION_PLAN.md) — milestone roadmap
- [Decisions (ADRs)](adr/README.md) — significant design decisions and
  their tripwires
- [Spikes](spikes/2026-09-20-uring.md) — timeboxed investigations

## Build

```sh
cmake --preset gcc-debug
cmake --build --preset gcc-debug -j"$(nproc)"
ctest --preset gcc-debug --output-on-failure -j"$(nproc)"
```

Requires CMake ≥ 3.24, a C++20 compiler, and Linux (epoll; io_uring on
kernels ≥ 6.0). See `CONTRIBUTING.md` for the full workflow.

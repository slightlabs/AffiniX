# AffiniX

A C++20 framework for thread-affine, event-driven applications.

> A thread is the unit of concurrency, and an `EventManager` **is** that
> thread's execution context. Nothing is shared implicitly. Cross-thread
> interaction is always an explicit message.

## Highlights

- One `EventManager` per thread — no implicit shared state, no thread hopping
- TCP servers/clients, UDP + multicast, Unix-domain sockets with
  SCM_RIGHTS descriptor passing, async DNS, Happy Eyeballs
- Compile-time protocols: fixed-header and general streaming parsers
- Pluggable I/O backends: epoll, io_uring (auto-selected), kqueue on
  macOS/BSD — all behind one proactor-shaped completion model
- Timers and timer groups with group cancel, FixedRate/FixedDelay repeats,
  and coalesced-overrun reporting
- Opt-in coroutines: `ConnRef` sessions, `when_all`/`when_any`/
  `with_timeout`, structured `TaskScope` — arena-backed frames
- Lock-free ITC: mailboxes, SPSC/MPSC channels, fan-out routing, barriers,
  `Sharded<T>` per-EM state — bounded and context-propagating
- Explicit backpressure: write watermarks, bounded queues, deadline-aware
  work shedding
- CPU affinity and NUMA-aware arenas (`LocalAlloc`/`Interleave`,
  hugepages, prefault)
- Observability: per-EM stats, log-scale latency histograms, always-on
  flight recorder, crash dumps, admin HTTP endpoint with Prometheus-style
  metrics
- Deterministic simulation: `SimRuntime` drives whole multi-shard
  scenarios on one thread with virtual time and scripted faults —
  same seed, byte-identical replay

## Guide

- [Getting started](guide/getting-started.md) — build, run, first server
- [Event manager](guide/event-manager.md) — the loop, config, lifecycle
- [Runtime & placement](guide/runtime.md) — groups, CPU/NUMA pinning, shutdown
- [Timers](guide/timers.md) — `after`/`every`, groups, repeat modes
- [Protocols](guide/protocols.md) — framing concepts, fixed-header, streaming
- [TCP](guide/tcp.md) — servers, clients, flow control, Unix sockets, SCM_RIGHTS
- [UDP](guide/udp.md) — datagrams, batches, multicast
- [Async DNS](guide/dns.md) — the `Resolver`
- [Inter-thread communication](guide/itc.md) — mailboxes, channels, fan, `Sharded`
- [Coroutines](guide/coroutines.md) — `CoroTask`, `ConnRef`, combinators, `TaskScope`
- [Backends](guide/backends.md) — epoll / io_uring / kqueue / sim
- [Memory](guide/memory.md) — arenas, pools, frame cache, `InlineFn`
- [Errors & context](guide/errors.md) — `Result`, `Error`, deadlines, stop tokens
- [Observability](guide/observability.md) — stats, histograms, flight recorder, admin
- [Simulation](guide/simulation.md) — `SimRuntime`, faults, replay
- [Testing](guide/testing.md) — the test pyramid and how to run it
- [Benchmarking](guide/benchmarking.md) — benches, baselines, `afx-load`

## Design documents

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

Requires CMake ≥ 3.24, a C++20 compiler, and Linux (epoll; io_uring where
available) — kqueue on macOS/BSD. See `CONTRIBUTING.md` for the full
workflow.

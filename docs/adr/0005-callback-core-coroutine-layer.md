# ADR-0005: Callback core, coroutines as an opt-in layer

**Status:** accepted
**Date:** 2026-09-13
**One-way door:** no (but the reverse order would be)

## Context

C++20 coroutines make network code read linearly, which is a large ergonomic
win. They also bring frame allocation, harder debugging (stack traces through
`resume`), subtle lifetime rules for parameters, and a cost model that is hard
to reason about in a hot path. A framework aimed at "very high performance"
cannot make them mandatory; a framework aimed at application authors cannot
omit them.

The ordering question is the real decision: build the core on coroutines and
offer callbacks as an adapter, or the reverse.

## Decision

The core is callback-based. `Task<T>` and the awaiters are a thin layer that
registers callbacks and resumes coroutines from them.

Rules for the layer:

- Awaiters always resume on the `EventManager` that suspended them.
- `Task<T>` is lazy, `[[nodiscard]]`, and uses symmetric transfer at
  `final_suspend` so awaiting in a loop does not grow the stack.
- Frames are allocated from the EM arena via
  `operator new(std::size_t, EventManager&)`.
- Cancellation is a `StopToken` in the promise, propagated to in-flight
  awaiters; `with_timeout` is built from it.

## Alternatives rejected

- **Coroutines as the core, callbacks as an adapter.** Every callback user then
  pays for a coroutine frame or an awkward shim, and there is no way to opt out
  of the cost model — the opposite of this framework's purpose. It is also the
  irreversible direction: you can add coroutines on top of callbacks at any
  time, but you cannot remove them from underneath.
- **Callbacks only.** Rejected because session-oriented protocols
  (handshake → auth → subscribe → stream) are genuinely painful as callback
  chains, and users would build their own coroutine layers with worse resumption
  guarantees.

## Consequences

- Two ways to write a handler, which is more documentation and more tests
  (every awaiter needs a callback-mode equivalent test).
- A user who measures a coroutine handler as too slow can rewrite that one
  handler in callback form without leaving the framework — the escape hatch
  that makes the layer safe to offer.
- The core stays testable without coroutine support in the toolchain.

## Tripwire

Revisit if the coroutine layer cannot be kept within ~10 % of the callback form
on `bench/echo` (in which case document it as a convenience layer rather than a
recommended default), or if arena allocation of frames proves unsound in
practice (DESIGN.md open question 5).

## Outcome (M9, 2026-10)

- **Spelling:** the task type is `afx::CoroTask<T>` — `afx::Task` was already
  the mailbox work-item closure (`InlineFn<void(), 48>`) before this layer
  existed. DESIGN.md §17 and IMPLEMENTATION_PLAN §25 record the deviation.
- **Tripwire measured:** `bench_echo_coro` vs `bench_echo` share one harness
  (the same file compiled twice — only the session shape differs). On the dev
  host, Release build, closed loop, 8 conns: coroutine echo measured *level
  with* callback echo on both backends (epoll 91.1k vs 90.9k rps; uring
  127.4k vs 126.0k rps — within run-to-run noise). The layer costs nothing
  measurable on this workload; baselines committed under
  `bench/baselines/echo_coro_*.json`.
- **Frame allocation:** sound via the size-bucketed `FrameCache` over the EM
  arena (spike `docs/spikes/0002-coro-arena.md`); heap fallback is counted
  (`stats().coro_heap_frames`), never silent.
- **Core impact:** one narrow seam — `Connection` gained a `CoroState` block
  and `enable_coro()`, plus `EngineOps` thunks on the EM. No behavioural
  change to callback mode.

# ADR-0007: Shared-nothing shards, no work stealing in I/O loops

**Status:** accepted
**Date:** 2026-09-13
**One-way door:** no

## Context

Two established models:

- **Thread pool over shared state:** work is queued centrally, any thread runs
  anything, shared data is protected by locks. Easy to load-balance; contention
  and cache-line bouncing set the ceiling, and tail latency is at the mercy of
  lock convoys.
- **Shared-nothing shards:** each thread owns its data and its connections;
  cross-thread work is an explicit message. Linear scaling, predictable tails;
  load imbalance between shards is the user's problem.

The stated requirements — per-thread `EventManager`, affinity control, explicit
ITC, very high performance — already describe the second model.

## Decision

Shared-nothing. One `EventManager` per thread, owning its connections, timers,
buffers and pools. No work stealing between event loops. Callbacks registered on
an EM run only on that EM's thread, for the life of the registration.

Load balancing is explicit and chosen by the user:

- `SO_REUSEPORT` sharded listeners (kernel distributes accepts);
- `Fan<T>` with round-robin or hash-by-key routing;
- `Sharded<T>` for per-shard state with `invoke_on` / `map_reduce`.

CPU-bound offload goes to an explicit `WorkerPool` whose threads own no event
loop; work stealing *inside that pool* is acceptable, since nothing there is
affinity-bound.

## Alternatives rejected

- **Work stealing across EMs.** Directly contradicts affinity: a stolen task
  touches data owned by another core's cache, and the handle/lifetime model
  (ADR-0003) and the no-blocking rule (ADR-0004) both assume single-threaded
  ownership. Adopting it would invalidate most of this design.
- **Hybrid: steal only "pure" tasks.** Requires users to correctly classify
  tasks as data-independent; misclassification is a data race that tests will
  not catch reliably.

## Consequences

- Scaling is linear to the extent that sharding is balanced; a hot key
  concentrated on one shard is visible as a high `busy_ratio` on that EM (hence
  per-EM `idle_ratio` in §21 from day one).
- No locks needed anywhere in the data path, so no lock-related tail latency.
- Users must think about their sharding key. This is a real cost and is treated
  as a documentation obligation, with `Fan` and `Sharded<T>` supplying the
  common patterns.

## Tripwire

Revisit the *load-balancing helpers* (not the model) if real deployments show
persistent imbalance that hashing cannot fix — the answer there is connection
migration at accept time or shard rebalancing between EMs, both of which are
explicit and compatible with shared-nothing. Work stealing inside the I/O loops
stays rejected as long as ADR-0003 and ADR-0004 stand.

# AffiniX — Implementation Plan

**Status:** M0–M12 landed; M13 layers are post-v1
**Companion to:** [DESIGN.md](DESIGN.md), [adr/](adr/README.md), and the
user-facing [guide](guide/getting-started.md)

This document turns the design into ordered, verifiable work. DESIGN.md says
*what* the framework is; this says *in what order it gets built, and how we
know each step is done*.

---

## Table of contents

1. [How this plan is meant to be used](#1-how-this-plan-is-meant-to-be-used)
2. [Working agreements](#2-working-agreements)
3. [Definition of done](#3-definition-of-done)
4. [Test taxonomy](#4-test-taxonomy)
5. [CI pipeline, built up in stages](#5-ci-pipeline-built-up-in-stages)
6. [Benchmark methodology](#6-benchmark-methodology)
7. [Milestone 0 — project setup](#milestone-0-project-setup)
8. [Milestone 1 — foundations](#milestone-1-foundations)
9. [Milestone 2 — loop skeleton and epoll backend](#milestone-2-loop-skeleton-and-epoll-backend)
10. [Milestone 3 — inter-thread communication](#milestone-3-inter-thread-communication)
11. [Milestone 4 — timers](#milestone-4-timers)
12. [Milestone 5 — runtime and placement](#milestone-5-runtime-and-placement)
13. [Milestone 6 — networking](#milestone-6-networking)
14. [Milestone 7 — robustness and observability](#milestone-7-robustness-and-observability)
15. [Milestone 8 — io_uring, timestamping, load generator](#milestone-8-io_uring-timestamping-load-generator)
16. [Milestone 9 — coroutine layer](#milestone-9-coroutine-layer)
17. [Milestone 10 — deterministic simulation](#milestone-10-deterministic-simulation)
18. [Milestone 11 — breadth](#milestone-11-breadth)
19. [Milestone 12 — operability](#milestone-12-operability)
20. [Milestone 13 — post-v1 optional layers](#milestone-13-post-v1-optional-layers)
21. [Spike schedule](#21-spike-schedule)
22. [Open-question resolution schedule](#22-open-question-resolution-schedule)
23. [Risk register](#23-risk-register)
24. [Requirement traceability](#24-requirement-traceability)
25. [Deviations from DESIGN.md](#25-deviations-from-designmd)

---

## 1. How this plan is meant to be used

- **Milestones are gates, not sprints.** A milestone is merged when its exit
  criteria pass, not when its tasks are individually finished. Half a milestone
  on `main` is a liability, because the next milestone builds on interfaces
  that have not yet been validated by a working example.
- **Effort is relative, not calendar.** Tasks are sized **S** (a sitting),
  **M** (a focused stretch), **L** (needs decomposition when started). There
  are deliberately no dates: this plan's value is ordering and acceptance
  criteria, and invented dates would only decay.
- **Task IDs are stable.** `M4-03` keeps its meaning even if the task is
  re-scoped, so commits, ADRs and benchmark baselines can cite it.
- **Order within a milestone is mostly free**, except where a task lists
  `after:`.

---

## 2. Working agreements

1. **One branch per milestone**, PRs per task or small task group. `main` stays
   green: every merge builds on GCC and Clang, and passes every test that
   exists at that point.
2. **No public API lands without a test and a use in an example.** An interface
   with no caller is an interface shaped by guessing (§26.2 of DESIGN.md).
3. **Deviating from DESIGN.md requires a doc change in the same PR.** If the
   deviation touches a recorded decision, it requires an ADR amendment or a new
   ADR that supersedes it. Code and docs drifting apart is how a design
   document becomes fiction.
4. **Annotations are not optional.** A new public non-thread-safe member
   without `AFX_REQUIRES` fails CI (ADR-0009). If the annotation is genuinely
   wrong for a member, that is a design discussion, not an
   `AFX_NO_THREAD_SAFETY_ANALYSIS`.
5. **Benchmarks accompany performance-relevant work**, with a recorded
   baseline, in the same PR. "We will measure it later" is how §24's targets
   become decoration.
6. **Every bug fixed gets a test that fails without the fix.** For concurrency
   bugs, the test is a seed for the simulator once milestone 10 exists, and a
   stress test before that.
7. **Commit messages explain why.** The task ID goes in the body, not the
   subject.

---

## 3. Definition of done

**A task is done when:**

- the code compiles warning-free on GCC and Clang at `-Wall -Wextra
  -Wpedantic`;
- unit tests cover the stated behaviour *and* at least one failure path;
- public headers carry annotations (ADR-0009) and no `detail::` types appear in
  public signatures;
- debug builds assert `is_current()` on every affine entry point;
- anything with a stated performance target has a benchmark and a baseline;
- DESIGN.md is updated if the implementation differs from it.

**A milestone is done when:**

- every task is done;
- its **exit criteria** (listed per milestone) pass on a clean checkout;
- its example programs run and are referenced from the README;
- the sanitizer matrix that exists at that point is green;
- the milestone's open questions are either resolved with evidence or
  explicitly re-deferred with a reason.

---

## 4. Test taxonomy

| Kind | Location | Introduced | Purpose |
|---|---|---|---|
| Unit | `test/unit/` | M1 | Types, containers, algorithms in isolation |
| Model-based | `test/model/` | M4 | Compare a fast structure against a naive reference implementation on random operation sequences |
| Property / fuzz | `test/fuzz/` | M6 | Framer against arbitrary bytes and arbitrary splits |
| Must-not-compile | `test/affinity_negative/` | M1 | Assert that affinity violations fail to build (Clang) |
| Invariant | `test/invariant/` | M2 | No-allocation steady state, no blocking in an EM thread, close-exactly-once, annotation coverage |
| Stress | `test/stress/` | M3 | Randomised concurrency under TSAN; nightly, longer |
| Integration | `test/integration/` | M6 | Real sockets, loopback, real backends |
| Simulation | `test/sim/` | M10 | Deterministic multi-EM scenarios with fault injection |
| Benchmark | `bench/` | M3 | Micro and macro, with baselines |

Two rules about this table:

- **Model-based testing is the primary defence for the timer wheel and the
  rings.** These are exactly the data structures where hand-written cases miss
  the interesting states (level rollover, wrap-around, batch-claim boundaries),
  and where a naive reference model is trivial to write and obviously correct.
- **Stress tests are not a substitute for simulation.** They are what we have
  until milestone 10, and they are retained afterwards because they run against
  the real kernel.

---

## 5. CI pipeline, built up in stages

Added incrementally so each stage is meaningful when introduced:

| Stage | Added in | Fails the build on |
|---|---|---|
| GCC + Clang debug/release build | M0 | any warning |
| clang-format, clang-tidy | M0 | style, obvious bug patterns |
| Clang `-Wthread-safety -Werror` | M1 | affinity annotation violations |
| Annotation coverage script | M1 | unannotated public affine member |
| Header hygiene script | M1 | `detail::` in a public signature |
| ASan + UBSan | M1 | any report |
| Must-not-compile suite | M1 | a violation that compiles |
| TSAN | M3 | any report |
| Invariant tests (no-alloc, no-block) | M2/M6 | violation |
| Benchmark baselines | M3 | regression beyond threshold (§6) |
| Fuzz (short, seeded corpus) | M6 | crash or timeout |
| Nightly: long stress, long fuzz, random sim seeds | M3/M10 | any failure, filed with the seed |
| macOS build (kqueue) | M11 | build or test failure |

---

## 6. Benchmark methodology

Performance targets in DESIGN.md §24 are unvalidated until measured, and a
benchmark that cannot distinguish a real regression from runner noise is worse
than none — it trains everyone to ignore it. Therefore:

- **Absolute numbers come from a pinned machine only**: fixed CPU model,
  `isolcpus` + `nohz_full` for the spinning cores, performance governor, SMT
  state recorded, hugepages configured. Recorded in `bench/ENVIRONMENT.md`
  alongside every published figure.
- **CI compares ratios, not absolutes.** Each benchmark run includes a
  calibration workload; the gate is `candidate/calibration` versus the stored
  `baseline/calibration` ratio, with a threshold of **5 %** for micro
  benchmarks and **10 %** for macro. This survives shared runners.
- **Baselines are committed** in `bench/baselines/<benchmark>.json` and updated
  only in a PR that explains the change.
- **Latency is reported as HDR histograms**, never as a mean. Load generators
  are open-loop by default so coordinated omission cannot hide a stall.

---

## Milestone 0 — project setup

**Goal:** a repository where a trivial `main()` builds, is linted, and is
checked by CI on both compilers. No framework code.

| ID | Task | Size |
|---|---|---|
| M0-01 | `CMakeLists.txt`: `afx` interface target + `afx_impl` static target, `afx::afx` alias, install/export, version header | M |
| M0-02 | `CMakePresets.json`: `gcc-debug`, `gcc-release`, `clang-debug`, `clang-release`, `asan`, `ubsan`, `tsan`, `clang-tsa` | S |
| M0-03 | Options: `AFX_WITH_URING`, `AFX_BACKEND_ONLY`, `AFX_BUILD_TESTS`, `AFX_BUILD_BENCH`, `AFX_SANITIZE`, `AFX_DEBUG_CHAOS`, `AFX_FLIGHT_RECORDER`, `AFX_THREAD_SAFETY_ANALYSIS`, `AFX_WITH_TIMESTAMPING` | S |
| M0-04 | `FetchContent` for doctest and nanobench, test-only, pinned by tag and hash | S |
| M0-05 | `.clang-format`, `.clang-tidy`, `.editorconfig` | S |
| M0-06 | GitHub Actions: build matrix, format check, tidy | M |
| M0-07 | `tools/check_header_hygiene.py`, `tools/check_annotations.py` (stubs that pass trivially, wired into CI now so they cannot be "added later") | S |
| M0-08 | `LICENSE` — needs a decision (README currently says "not yet chosen") | S |
| M0-09 | `CONTRIBUTING.md`: the working agreements in §2, condensed | S |

**Exit criteria:** clean clone → `cmake --preset gcc-debug && ctest` succeeds
with zero tests; all CI jobs green; both hygiene scripts run (and are proven to
fail when fed a deliberate violation — a fixture test for the scripts
themselves, because an unverified lint script is a placebo).

**Open decision:** the license. Apache-2.0 is the usual choice for a library
intended to be vendored (patent grant, permissive); MIT if simplicity matters
more. Needs an owner's call before the first external contribution.

---

## Milestone 1 — foundations

**Goal:** the vocabulary types everything else is written in, and the
enforcement machinery, before there is any code to retrofit.

| ID | Task | Size | Notes |
|---|---|---|---|
| M1-01 | `sys/annotations.hpp`: `AFX_CAPABILITY`, `AFX_REQUIRES`, `AFX_EXCLUDES`, `AFX_GUARDED_BY`, `AFX_NO_TSA` | S | Clang attributes; empty on GCC |
| M1-02 | `test/affinity_negative/`: CMake harness asserting that listed sources **fail** to compile under Clang | M | Uses `try_compile`; skipped on GCC with a loud message |
| M1-03 | `tools/check_annotations.py` for real: parse public headers, flag non-thread-safe members lacking `AFX_REQUIRES` | M | Allow-list for the thread-safe three |
| M1-04 | `sys/error.hpp`: `Error` (4 bytes), `ErrorCategory`, `errno` mapping, message table | S | |
| M1-05 | `sys/result.hpp`: `Result<T>`, `AFX_TRY`, `std::expected` alias under C++23 | M | Monadic `and_then`/`transform` only if used |
| M1-06 | `sys/inline_fn.hpp`: `InlineFn<Sig, N>`, move-only, inline storage, heap fallback | M | Used by every callback type |
| M1-07 | `sys/clock.hpp`: `SteadyClock`, `VirtualClock`, TSC calibration + `invariant_tsc` detection | L | See note below |
| M1-08 | `core/context.hpp`, `core/deadline.hpp`: `Context`, `Deadline`, `TraceId`, `StopToken`, `ContextScope` | M | ADR-0008 |
| M1-09 | `sys/topology.hpp`: parse cores, SMT siblings, NUMA nodes, cache sharing | L | Sysfs root injectable |
| M1-10 | `sys/affinity.hpp`: `CoreSet`, pin/unpin, sched policy, capability probing | M | |
| M1-11 | `sys/numa.hpp`: node-local alloc, `mbind`, hugepages, prefault | M | No dependency on libnuma; raw syscalls |
| M1-12 | Unit tests for all of the above | L | |

Notes on the two tasks that are easy to underestimate:

- **M1-07 (clock).** `VirtualClock` must be complete enough to drive the loop
  and the backend timeout, not just to answer `now()`, or milestone 4's tests
  cannot use it. TSC use requires `constant_tsc` + `nonstop_tsc` detection and
  a calibration against `CLOCK_MONOTONIC`; when either is missing, fall back
  silently to `clock_gettime` and record the fallback in stats (a silent
  wrong-clock is a whole class of unexplainable latency bugs).
- **M1-09 (topology).** Make the sysfs root a constructor parameter. Then
  topology parsing is a pure function over a directory tree, and CI can test
  dual-socket, SMT-off, restricted-cpuset and hybrid P/E-core layouts from
  committed fixtures without needing that hardware. Without this, the placement
  code is only ever tested on whatever machine the developer has.

**Exit criteria:** every type unit-tested; `InlineFn` proven allocation-free
within its inline budget by a poisoned-`malloc` test; `VirtualClock` can
advance and order timers (no wheel yet — a `std::multimap` reference
implementation is used, and is kept afterwards as the model for M4);
`-Wthread-safety` job green; must-not-compile suite has at least three real
cases; ASan/UBSan green; topology fixtures for at least four machine shapes.

---

## Milestone 2 — loop skeleton and epoll backend

**Goal:** a running event loop with a raw-fd escape hatch, plus the io_uring
spike that validates the backend concept **before** it is frozen.

| ID | Task | Size | Notes |
|---|---|---|---|
| M2-01 | `backend/backend.hpp`: `IoBackend` concept, `Completion`, `Timestamps`, `Interest`, `UserData` tagging | M | Tag = kind (8b) + slot (24b) + generation (32b) |
| M2-02 | `core/handle_table.hpp`: slab + generation + deferred reclamation | M | ADR-0003; reused for conns, timers, io, groups |
| M2-03 | **io_uring spike** (throwaway), ~200 lines: accept + echo with registered buffers, multishot recv, linked SQEs | M | after: M2-01 draft |
| M2-04 | Spike report → `docs/spikes/0001-io-uring.md`; amend `IoBackend` concept | S | after: M2-03 |
| M2-05 | `backend/epoll.hpp`: proactor emulation, edge-triggered, `eventfd` wake, timeout from deadline | L | after: M2-04 |
| M2-06 | `core/event_manager.hpp`: loop stages 1–7, `run`, `poll_once`, `stop`, cached `now()`, `defer`, `on_idle` | L | |
| M2-07 | `watch`/`modify`/`unwatch` raw-fd API | M | |
| M2-08 | Context plumbing: ambient `Context` set per work item, `with_context` scope | M | after: M1-08 |
| M2-09 | `Stats` skeleton: counters only, no histograms yet | S | |
| M2-10 | `examples/raw_echo.cpp` using `watch()` only | S | Proves the loop before the net layer exists |
| M2-11 | `test/invariant/no_alloc_steady_state.cpp` (loop + raw echo) | M | |

**Sequencing note:** M2-03 is scheduled *before* M2-05 on purpose (ADR-0002,
§26.2). Writing epoll first and io_uring later is the documented trap: the
concept ends up shaped by readiness semantics. The spike is expected to be
deleted; its output is the report and the concept amendments.

**Exit criteria:** `raw_echo` handles a loopback client under load; loop stage
order matches DESIGN.md §7.3 and is covered by a test that counts stage
executions; steady state allocates zero; the spike report exists and either
confirms the concept or lists the amendments made; open question 4 (io_uring
buffer rings) has an answer recorded.

---

## Milestone 3 — inter-thread communication

**Goal:** correct, fast, lost-wakeup-free messaging between EMs, with context
propagation.

| ID | Task | Size | Notes |
|---|---|---|---|
| M3-01 | `itc/spsc_ring.hpp`: padded, batch-claim both sides | M | |
| M3-02 | `itc/mpsc_ring.hpp`: Vyukov slot-sequence | M | |
| M3-03 | Model-based tests for both rings vs `std::deque` reference | M | Random op sequences, seeded |
| M3-04 | `itc/mailbox.hpp`: `Mailbox` handle, `post`, `post_batch`, `try_post`, `OverflowPolicy` | L | |
| M3-05 | **Arm/block protocol** (DESIGN.md §8.1) + dedicated lost-wakeup stress test | L | The highest-risk code in the framework |
| M3-06 | Loop integration: drain stage bounded by `max_itc_batch`; deadline check before dispatch | M | after: M1-08 |
| M3-07 | `post_and_reply` + per-EM pending-request table with deadline-driven expiry | M | ADR-0004 |
| M3-08 | `itc/channel.hpp`: typed `channel<T>`, SPSC/MPSC, batch delivery, `attach` | M | |
| M3-09 | `itc/fan.hpp`: round-robin, hash-by-key, broadcast | S | |
| M3-10 | `itc/barrier.hpp` | S | |
| M3-11 | `itc/sharded.hpp`: `Sharded<T>`, `invoke_on`, `map_reduce` | M | Needs M5 for real shards; single-EM tests now |
| M3-12 | `test/invariant/no_blocking_in_em.cpp` | S | |
| M3-13 | `bench/itc_pingpong`, `bench/channel_throughput` + baselines | M | First benchmark gate |
| M3-14 | `examples/ping_pong_itc.cpp` | S | |
| M3-15 | Nightly TSAN stress job | S | |

**On M3-05, the lost-wakeup test.** C++ has no Loom equivalent, so correctness
rests on three complementary things, none of which is sufficient alone: the
written argument in DESIGN.md §8.1; a stress test with producers posting at
randomised intervals straddling the spin budget while the consumer flips
between spinning and blocking, asserting that every message is eventually
delivered and that the loop never sleeps with a non-empty queue; and TSAN over
that test. A missed wakeup that happens once per 10^9 transitions will hang a
production shard, so this test runs long in nightly, not only in PR CI.

**Exit criteria:** rings pass model-based tests with ≥ 10^7 random ops;
lost-wakeup stress passes 30 minutes under TSAN with no stall; `ping_pong_itc`
p50 within §24's 200 ns target on the pinned machine (or the target is revised
with measurements and a note); context and deadline demonstrably propagate
across a hop (test asserts a budget set on EM A bounds work on EM B); channel
throughput baseline recorded — and open question 7 (`Context` copy cost)
answered with that number.

---

## Milestone 4 — timers

**Goal:** all timer semantics, proven against virtual time.

| ID | Task | Size | Notes |
|---|---|---|---|
| M4-01 | `core/timer_wheel.hpp`: 4 × 256 hierarchical wheel, cascade on rollover | L | |
| M4-02 | 4-ary min-heap for precise/far deadlines + next-deadline query | M | |
| M4-03 | `TimerId` generation handling; cancel of stale ids is a no-op | S | after: M2-02 |
| M4-04 | `TimerGroup`: intrusive list through wheel nodes, `cancel_group` | M | §10.1; fixes node layout, so it lands with the wheel |
| M4-05 | `after` / `at` / `every` / `reschedule` / `time_until` | M | |
| M4-06 | `RepeatMode::FixedRate` vs `FixedDelay`; overrun coalescing with `missed` | M | |
| M4-07 | Cancel-from-callback, self-cancel, deferred removal | M | |
| M4-08 | Deadline-derived timers registered in the work item's group | S | after: M3-06 |
| M4-09 | Loop integration: expiry stage bounded by one tick; backend timeout from next deadline | M | |
| M4-10 | Model-based tests vs the M1 `multimap` reference, all under `VirtualClock` | L | Includes level rollover and 49-day horizon |
| M4-11 | `bench/timer_arm_cancel`, `bench/timer_fire_storm` + baselines | M | |
| M4-12 | `examples/timer_zoo.cpp` | S | |

**Exit criteria:** model-based suite green over ≥ 10^6 random
arm/cancel/advance operations including multi-level cascades; every semantic in
DESIGN.md §10 has a named test (drift, coalescing, self-cancel, group cancel,
stale cancel, rollover); zero real-time sleeps anywhere in the timer suite;
arm+cancel benchmark meets or revises §24's 50 ns target; open question 2
(tick granularity) resolved by measuring cascade cost at 1 ms vs 256 µs.

---

## Milestone 5 — runtime and placement

**Goal:** threads placed deliberately, failures reported rather than swallowed,
and shutdown that drains.

| ID | Task | Size | Notes |
|---|---|---|---|
| M5-01 | `Runtime`, `ThreadConfig`, `spawn`, `spawn_group`, `each`, `start`, `join` | L | |
| M5-02 | Placement algorithm as a pure function over `Topology` + `ThreadConfig` | M | Unit-tested against M1-09 fixtures |
| M5-03 | SMT-aware `OnePerPhysicalCore`; `Explicit`; `None` for restricted cpusets | M | |
| M5-04 | Sched policy application; permission failures surface as `Error` | S | Never silently ignored |
| M5-05 | NUMA-local arena allocation per EM | M | after: M1-11 |
| M5-06 | NIC locality check and warning | S | `/sys/class/net/*/device/numa_node` |
| M5-07 | Startup placement log (always, one line per thread) | S | |
| M5-08 | `signalfd` handling on a designated EM; `on_signal` | M | |
| M5-09 | `Runtime::shutdown(timeout)` drain sequence (DESIGN.md §20) | L | |
| M5-10 | `Sharded<T>` completed against real shards | S | after: M3-11 |
| M5-11 | Container/cpuset integration test (restricted affinity → graceful degradation) | M | |

**Exit criteria:** placement unit tests pass for all committed topology
fixtures; running under a restricted cpuset yields a warning and
`Placement::None` rather than an abort or a silent single-core pile-up;
`shutdown` drains in-flight work and is proven by a test that counts completed
vs abandoned items; signal handling never runs user code in a signal handler.

---

## Milestone 6 — networking

**Goal:** TCP servers and clients with user-defined framing, and an `echo_server`
that is allocation-free in steady state.

| ID | Task | Size | Notes |
|---|---|---|---|
| M6-01 | `core/io_buffer.hpp`: headroom, cursors, `prepend`, `compact` | M | |
| M6-02 | `core/pool.hpp`: slab/arena object pools, startup-sized | M | |
| M6-03 | `net/endpoint.hpp`, `SockAddr`, `SocketOptions` (nodelay, quickack, buffers, keepalive, busy-poll, timestamping) | M | |
| M6-04 | `net/protocol.hpp`: `ParseResult`, `Protocol` concept | M | ADR-0006 |
| M6-05 | **Line-delimited framer first** (the awkward case) | S | after: M6-04; §26.2 rule of two |
| M6-06 | `FixedHeaderProtocol` + `FixedHeaderFramer<P>` adapter over `Protocol` | M | after: M6-05 |
| M6-07 | `MessageBatch<P>`, `FrameView<H>`, `retain()` → `BufferSlice` | M | |
| M6-08 | `net/connection.hpp`: read path, framing loop, write queue, state machine | L | |
| M6-09 | Close semantics: `on_close` exactly once, `CloseReason`, deferred slot reclaim | M | |
| M6-10 | `net/acceptor.hpp`: bounded accepts per iteration, `SO_REUSEPORT` per shard | M | |
| M6-11 | `net/tcp_server.hpp`, `Handlers<P>`, `make_server` | M | |
| M6-12 | `net/tcp_client.hpp`: connect, timeout, jittered backoff reconnect, `on_state_change` | L | IP-only until M11 DNS |
| M6-13 | Debug chaos: randomised byte splits, dispatch order, generation starts | M | §26.4 |
| M6-14 | Fuzz target for the framer | M | |
| M6-15 | Integration tests: loopback echo, partial frames, oversized frames, peer reset, half-close | L | |
| M6-16 | `examples/echo_server.cpp`, `examples/custom_framing.cpp` | M | |
| M6-17 | `bench/driver` — minimal load driver for internal use | M | Grows into `afx-load` in M8 |
| M6-18 | Extend no-alloc invariant test to the full server path | S | |

**Exit criteria:** `echo_server` survives the fuzz corpus and a 10 000
connection soak with zero allocations after warm-up; the line-delimited and
fixed-header protocols both work through the same read path; `on_close` fires
exactly once in every tested termination path (peer FIN, local close, framing
error, reset, shutdown); a frame larger than `kMaxBody` is rejected without
allocating; chaos mode on in all debug tests.

---

## Milestone 7 — robustness and observability

**Goal:** the server behaves correctly at its limits, and explains itself when
it does not.

| ID | Task | Size | Notes |
|---|---|---|---|
| M7-01 | `FlowControl`: watermarks, `SendResult`, `on_writable`, `auto_pause_reads` | L | |
| M7-02 | `OverflowPolicy`: `Disconnect`, `DropNewest`, `DropOldest`, `StopReading` | M | |
| M7-03 | Idle read/write timeouts, handshake timeout — all in the connection's `TimerGroup` | S | after: M4-04 |
| M7-04 | Full `Stats`: all counters in DESIGN.md §21 | M | |
| M7-05 | HDR histograms, per-EM, no atomics on the hot path | L | |
| M7-06 | `idle_ratio` derivation and export | S | |
| M7-07 | `core/flight_recorder.hpp` + record calls at the stats call sites | L | §21.1 |
| M7-08 | Crash/stall dump path (`SIGSEGV`/`SIGABRT` handler that only touches the ring and `write(2)`) | M | |
| M7-09 | `tools/afx-flight` decoder (minimal: per-EM timeline) | M | |
| M7-10 | Stall detector with handler identity and stage | M | |
| M7-11 | Slow-consumer test: bounded memory, correct policy behaviour | M | |
| M7-12 | Read-buffer strategy experiment: per-connection vs shared-with-promotion | L | Resolves open question 3 |

**Exit criteria:** a deliberately slow consumer cannot grow process memory
without bound under any `OverflowPolicy`; each policy has a test asserting its
specific behaviour; flight recorder is on in release and costs less than a
measured 10 ns per record; a forced segfault produces a decodable timeline;
open question 3 answered with memory and throughput numbers at 1 k / 10 k /
100 k connections.

---

## Milestone 8 — io_uring, timestamping, load generator

**Goal:** the second real backend behind the frozen concept, and trustworthy
comparative numbers.

| ID | Task | Size | Notes |
|---|---|---|---|
| M8-01 | `backend/uring.hpp`: setup, submission batching, CQE draining | L | |
| M8-02 | Feature probing via `IORING_REGISTER_PROBE`; graceful fallback to epoll | M | |
| M8-03 | Registered/provided buffer rings | L | Per M2-04's answer to open question 4 |
| M8-04 | Multishot accept and recv | M | |
| M8-05 | `MSG_RING`-based cross-EM wake (optional path, benchmark against `eventfd`) | M | |
| M8-06 | `SO_TIMESTAMPING` plumbing into `Completion::stamps` | M | §9.4 |
| M8-07 | Wire-level histograms (NIC→kernel, kernel→dequeue, dequeue→handler) | M | after: M7-05 |
| M8-08 | `SQPOLL` support for `Spin` mode | M | |
| M8-09 | **`tools/afx-load`**: protocol-aware, open/closed loop, HDR output | L | Promoted from M12 (see §25) |
| M8-10 | Head-to-head benchmark suite: epoll vs io_uring, same workloads | M | |
| M8-11 | Backend-parity test suite: every integration test runs against both | M | |

**Exit criteria:** every integration test passes on both backends with no
backend-specific branches in test code; `bench/echo` results for both recorded
in `bench/baselines/` with `ENVIRONMENT.md`; ADR-0002's tripwire evaluated
explicitly (is epoll emulation within 5 % of a hand-written epoll loop?) and
the answer written into the ADR; `afx-load` produces coordinated-omission-free
histograms.

---

## Milestone 9 — coroutine layer

**Goal:** linear-looking code on top of an unchanged callback core.

| ID | Task | Size | Notes |
|---|---|---|---|
| M9-01 | **Arena allocation spike** for coroutine frames | M | Resolves open question 5 *before* the rest |
| M9-02 | `coro/task.hpp`: lazy `Task<T>`, symmetric transfer, `[[nodiscard]]` | L | after: M9-01 |
| M9-03 | `Context` + `StopToken` in the promise; propagation across `co_await` | M | ADR-0008 |
| M9-04 | Awaiters: `recv`, `send`, `sleep`, `connect`, `post_and_reply` | L | Each resumes on the originating EM |
| M9-05 | `when_all`, `when_any`, `with_timeout` | M | |
| M9-06 | `coro/scope.hpp`: `TaskScope` structured concurrency | M | Designed with cancellation, not after |
| M9-07 | Callback-mode equivalence tests for every awaiter | M | |
| M9-08 | `bench/echo_coro` vs `bench/echo` | M | ADR-0005 tripwire: within 10 % |
| M9-09 | `examples/session_coro.cpp` | S | |

**Exit criteria:** no change required to the callback core to support any of
this (if a change was required, that is an ADR-0005 finding and gets written
up); every awaiter proven to resume on the originating EM by a test that
asserts thread identity; `TaskScope` proven to cancel and join children on
scope exit, including on exception; coroutine echo within 10 % of callback echo
or the gap documented in ADR-0005.

---

## Milestone 10 — deterministic simulation

**Goal:** reproducible multi-EM concurrency bugs.

| ID | Task | Size | Notes |
|---|---|---|---|
| M10-01 | `backend/sim.hpp`: in-memory sockets, scriptable completions | L | Second `IoBackend` impl for real (ADR-0002) |
| M10-02 | `SimRuntime`: N EMs on one thread, seeded scheduler, virtual clock | L | after: M1-07 |
| M10-03 | Deterministic ITC: mailbox delivery ordered by the seeded scheduler | M | |
| M10-04 | `FaultProfile`: packet loss, partial reads, slow peer, partition, reset | M | |
| M10-05 | Invariant checkers runnable per step | M | |
| M10-06 | Replay: same seed → identical event sequence, asserted | M | The property that makes all of this worthwhile |
| M10-07 | Port key integration scenarios to simulation | L | |
| M10-08 | Nightly randomised-seed job; failing seeds filed automatically | M | |

**Exit criteria:** the same seed reproduces byte-identical event sequences
across runs and across machines; at least three previously-found bugs (or
deliberately injected ones) are reproducible from a seed; a full multi-shard
proxy scenario runs in simulation in under a second of wall time.

---

## Milestone 11 — breadth

| ID | Task | Size | Notes |
|---|---|---|---|
| M11-01 | `net/udp_socket.hpp`: `recvmmsg`/`sendmmsg`, batch receive | L | |
| M11-02 | Multicast join/leave with source filtering | M | |
| M11-03 | Async DNS resolver on a dedicated EM, replying by mailbox | L | Deadline-aware (§7.4) |
| M11-04 | Happy Eyeballs in `TcpClient` | M | after: M11-03 |
| M11-05 | Unix domain sockets | M | |
| M11-06 | `SCM_RIGHTS` fd passing | M | Foundation for hot restart |
| M11-07 | `backend/kqueue.hpp` | L | Proactor emulation, same as epoll |
| M11-08 | macOS CI runner and platform test triage | M | |

**Exit criteria:** DNS resolution never blocks an EM (proven by a test using a
deliberately slow resolver); UDP batch receive benchmarked; kqueue passes the
backend-parity suite from M8-11; macOS CI green.

---

## Milestone 12 — operability

| ID | Task | Size | Notes |
|---|---|---|---|
| M12-01 | Admin EM + HTTP/1.1 subset served with AffiniX itself | L | Dogfooding |
| M12-02 | Endpoints: stats, histograms, connections, placement, config, version | M | |
| M12-03 | Prometheus text exposition | S | |
| M12-04 | Flight recorder dump endpoint; `afx-flight` cross-shard merge | M | after: M7-09 |
| M12-05 | Runtime toggles: stall threshold, chaos mode, log level | S | |
| M12-06 | `examples/observable_server.cpp` | S | |

**Exit criteria:** the admin endpoint runs on a `Block`-mode EM without
measurably affecting a spinning shard (benchmarked, not assumed); every §21
metric is reachable; a cross-shard request timeline can be reconstructed from a
live dump.

---

## Milestone 13 — post-v1 optional layers

Each item here is independent, gated on a real consumer, and gets its own plan
when started: TLS codec (+ kTLS), file I/O via io_uring, zero-copy send,
`WorkerPool`, hot restart, shared-memory IPC transport, client resilience
primitives, pub/sub fan-out, `tpause` spin refinement, privilege drop and
seccomp. See DESIGN.md §28.2.

---

## 21. Spike schedule

Spikes are throwaway code whose output is a written finding. They are scheduled
*before* the interface they inform, per §26.2 of DESIGN.md.

| Spike | When | Question it answers | Output |
|---|---|---|---|
| io_uring echo | M2-03, before epoll | Does the completion-shaped concept fit io_uring's real features? Who owns read buffers with buffer rings? | `docs/spikes/0001-io-uring.md` + concept amendments |
| Coroutine arena frames | M9-01, before `Task<T>` | Can frames be arena-allocated soundly? What happens when a frame outlives the arena? | `docs/spikes/0002-coro-arena.md`; may reject arena allocation |
| Read-buffer strategy | M7-12 | Per-connection vs shared-with-promotion, at 1 k/10 k/100 k connections | Numbers in the PR; resolves open question 3 |
| Context cost | M3-13 | Does a ~40 B per-hop copy matter at channel throughput? | Numbers; resolves open question 7 |

---

## 22. Open-question resolution schedule

DESIGN.md §29's questions, each assigned to the milestone where evidence
becomes available. A question is not closed by opinion.

| # | Question | Resolved in | By what evidence |
|---|---|---|---|
| 1 | MPSC ring: hand-rolled vs moodycamel | M3 | `bench/channel_throughput` head-to-head |
| 2 | Timer tick: 1 ms vs 256 µs | M4 | Cascade cost + timer lateness histograms |
| 3 | Read buffer strategy | M7 | Memory and throughput at three connection counts |
| 4 | io_uring buffer ring ownership | M2 | Spike 0001 |
| 5 | Coroutine frame allocation | M9 | Spike 0002 |
| 6 | Windows/IOCP | post-v1 | Deferred; seam must not preclude it |
| 7 | `Context` size and cost | M3 | Channel benchmark with and without context |
| 8 | Flight recorder TSC merging | M7 | Calibration accuracy measurement; documented claim |
| 9 | Annotation coverage enforcement | M1 | Whether the grep script produces false confidence in practice |
| 10 | Deadlines across hosts | M13 (RPC) | Decide wire form before the first RPC user |

---

## 23. Risk register

| Risk | Severity | Trigger to watch | Mitigation |
|---|---|---|---|
| Lost wakeup in the arm/block protocol | **Critical** — a hung shard in production | Any stall in the M3-05 stress test | Written correctness argument, long nightly stress, TSAN, simulation once M10 exists |
| Backend concept shaped by epoll | **High** — forfeits io_uring's value | Spike 0001 requires concept changes late | Spike before epoll (M2-03), ADR-0002 tripwire measured at M8 |
| Annotation coverage decays | High — false confidence | Any `AFX_NO_TSA` added, or coverage < 100 % | CI script; ADR-0009 says remove the mechanism rather than ship partial checking |
| Performance targets unrealistic | Medium — targets get ignored | First benchmark far from §24 | Targets are revised *with measurements* in a PR, not quietly dropped |
| CI benchmark noise | Medium — gate gets muted | Frequent spurious failures | Ratio-to-calibration method (§6); absolutes only on the pinned machine |
| Coroutine layer forces core changes | Medium — ADR-0005 violated | Any core change needed in M9 | Write the finding up; consider documenting coroutines as a convenience layer |
| Scope creep into L5 services | Medium — v1 never ships | Work starting on RPC/HTTP before M12 | M13 gating; DESIGN.md §3 non-goals |
| Single-platform blindness | Low/Medium | kqueue work reveals deep assumptions at M11 | Keep `#ifdef` out of the net layer; backend-parity suite from M8-11 |
| Solo-maintainer bus factor | Medium | — | ADRs with tripwires, spike reports, executable acceptance criteria — this plan is the mitigation |

---

## 24. Requirement traceability

The capabilities required in DESIGN.md §1, mapped to where they are built and
how they are proven.

| Requirement | Built in | Proven by |
|---|---|---|
| One `EventManager` per thread, no thread hopping | M2, M5 | `is_current()` asserts, must-not-compile suite, thread-identity tests |
| Any number of TCP servers/clients per EM | M6 | Integration test with multiple servers of different protocols on one EM |
| Customisable TCP message header | M6-04…07 | Line-delimited and fixed-header protocols through one read path; fuzz |
| Affinity, enabled as required | M5 | Placement unit tests over topology fixtures; restricted-cpuset test |
| Inter-thread communication | M3 | Model-based ring tests, lost-wakeup stress, `ping_pong_itc` |
| Polling in the event manager | M2, M8 | `Spin`/`SpinThenBlock` latency benchmarks; `SQPOLL` path |
| One-shot and repeating timers | M4 | Model-based virtual-time suite |
| High performance | M3, M6, M8 | §24 targets with recorded baselines; no-alloc invariant test |

---

## 25. Deviations from DESIGN.md

Recorded here rather than silently applied, per §2, agreement 3.

1. **`afx-load` moves from milestone 12 to milestone 8** (task M8-09).
   DESIGN.md §27 listed the load generator under operability tooling, but
   milestone 8's whole purpose is a defensible epoll-vs-io_uring comparison,
   and that comparison is only as trustworthy as the load generator producing
   it. A minimal internal driver (M6-17) appears earlier still, for M6's soak
   tests. DESIGN.md §27 has been updated to match.
2. **Milestone 0 is new.** DESIGN.md §27 started at "Foundations"; project
   scaffolding, CI and the two hygiene scripts are separated out because
   ADR-0009's guarantee depends on the enforcement existing before the first
   header does.
3. **The license is an open decision** (M0-08), tracked here because it blocks
   external contribution but not implementation.
4. **The coroutine task type is spelled `afx::CoroTask<T>`, not `afx::Task<T>`**
   (M9-02). `afx::Task` was already the posted-work closure
   (`InlineFn<void(), 48>` in `itc/mailbox.hpp`) by the time the coroutine
   layer landed, and renaming it would have churned the entire core for a
   name. DESIGN.md §17's examples read `CoroTask` now; ADR-0005 records the
   outcome.
5. **`FaultProfile` splits the design's `partition_ms` field into
   `partition_chance` + `partition_window`** (M10-04). The §22.3 sketch
   implied automatic partitions of `{50, 500}` ms; the shipped model makes
   a partition a per-delivery Bernoulli draw that blackholes the link for a
   window drawn uniformly from `partition_window` — an explicit probability
   is what a seeded sweep needs. `net().partition(link, window)` remains a
   direct test hook. §22.3's example is updated.
6. **`SockAddr::unix_domain()`, not `::unix()`** (M11-05). GCC predefines
   `unix` as a macro expanding to `1`; the factory name had to move. Abstract
   namespace paths (leading NUL) are also deliberately not translated — the
   filesystem path is what `SockAddr` stores and what gets unlinked.
7. **macOS/BSD signal handling is a self-pipe, not `EVFILT_SIGNAL`** (M11-08).
   The signal thread is a dedicated `std::thread`, not an EM, so there is no
   kqueue to register a signal filter on. `sigaction` handlers write the
   signal number to the existing shutdown self-pipe; user callbacks still
   run on the dedicated thread. DESIGN.md §20 updated.
8. **`DefaultPollBackend` is the platform readiness backend; `AutoBackend`
   is Linux-only** (M11-07). On macOS/BSD `EventManager` is
   `BasicEventManager<SteadyClock, KqueueBackend>` directly — there is no
   io_uring to auto-select against, so the alias collapses rather than
   probes. `EventManagerConfig::backend` remains but is ignored by fixed
   backends, same as `SimBackend` already did.
9. **`pin_this_thread` returns `Err::Unsupported` on macOS/BSD, which
   `Runtime` degrades to unpinned-with-a-warning** (M11-08). Requested
   placement stays fatal on Linux; on platforms with no hard pinning the
   honest outcome is a loud degradation, not a dead shard — and not silent
   affinity-tag "pinning" that isn't binding.
10. **`Connection::shutdown_write()` is close-after-drain** (M12-01). The
    admin endpoint needs FIN to follow the queued response bytes; an
    immediate `::shutdown(SHUT_WR)` raced the unsent `write_buf_` and lost
    the reply. `ConnState::ShutdownWrite` now delays the syscall until the
    send queue drains, which is strictly closer to §13's semantics.
11. **`pending_writes_` is pending work in `wait_timeout()`** (M12-01). A
    mailbox task that queues a write (exactly what an admin gather reply
    does) used to leave the EM free to block before stage 6 flushed —
    responses stalled until the next unrelated event. Non-empty pending
    writes now force a nonblocking iteration.
12. **The admin HTTP/1.1 subset is one-request-per-connection**
    (M12-01). Every response carries `Connection: close`; pipelined bytes
    behind a request are dropped after the reply. That covers health checks,
    Prometheus scrapes and `afx-flight` fetches — keep-alive/pipelining is
    deliberately out of scope for a debug endpoint.
13. **Cross-shard gathers copy only what the endpoint renders** (M12-02).
    `LatencyMetrics` histograms are ~5 KB each; the gather payload is gated
    by route so `/stats`/`/conns`/`/config`/`/flight` never touch them.
    `afx-flight` also accepts the framed `AFXFLT01` multi-shard dump that
    `/flight` emits, in addition to raw concatenated records.
14. **`bench/admin_impact` measures a spinning shard under three admin
    states** (M12 exit): baseline, admin EM running idle, and admin under
    request load — split into `/healthz` (admin-local) vs gather endpoints
    (one shard mailbox task per request). On the 4-logical-cpu dev box the
    idle admin EM moved a pinned spinning shard by ~1% (noise); under a
    3-thread request storm the delta over `/healthz`-only contention is the
    gather's per-request mailbox cost. Note the run also exposed the
    `ShutdownWrite` fd-leak fixed in note 17 — accept error storms, not
    gather work, dominated the earliest measurements.
15. **Per-stage duration histograms exist but are opt-in**
    (`EventManagerConfig::profile_stages`, M12 exit). §21 lists them; the
    seven extra clock reads per iteration stay off the hot path unless
    enabled, and `afx_stage_ns{stage=…}` series are emitted either way.
16. **`TcpClient::start()` is idempotent and `connect_next()`
    close-before-destroys** (M12 hardening). A double `start()` used to
    orphan a live connecting socket's in-flight SQE; its EBADF completion
    then dispatched into the recycled sink slot and killed the healthy
    replacement conn. io_uring-only, found via `/conns` testing.
17. **`ShutdownWrite` conns keep a drain read outstanding** (M12 hardening).
    `on_recv` used to drop every completion in non-Established states, so a
    respond-and-close conn never observed the peer's FIN and leaked its fd —
    under admin load that meant ~1024 leaked sockets, `EMFILE` on accept,
    and a hot re-arm loop that starved the endpoint. The conn now drains
    (discarding pipelined bytes) until FIN/error, and `AdminServer` sets
    `idle_read_timeout` so a peer that never FINs is bounded.
18. **`for_each_shard` copies the apply lambda into each posted task**
    (M12 hardening). Capturing it by reference posted a task that read the
    caller's dead stack frame — toggles landed with garbage values.

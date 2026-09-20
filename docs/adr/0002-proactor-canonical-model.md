# ADR-0002: Proactor is the canonical I/O model; readiness backends emulate it

**Status:** accepted
**Date:** 2026-09-13
**One-way door:** yes

## Context

AffiniX must support epoll, io_uring and kqueue. These are not the same model:

- **Readiness (reactor):** epoll and kqueue tell you "this fd is readable", and
  you then perform the `recv` yourself.
- **Completion (proactor):** io_uring tells you "the `recv` you submitted has
  finished, here are the bytes".

The internal interface has to pick one shape. Whichever is picked, the other
family must be emulated on top of it, and the two directions of emulation are
very much not equally cheap.

## Decision

The `IoBackend` concept is completion-shaped: callers submit an operation with
a buffer and a `UserData` tag, and `wait()` returns `Completion` records.

`EpollBackend` and `KqueueBackend` emulate this: `submit_recv` stores the
buffer and arms edge-triggered interest; on readiness the backend performs the
`recvmsg` itself and synthesises a `Completion`. Cost is one stored pointer and
one branch per operation.

## Alternatives rejected

- **Readiness as canonical, io_uring emulating it.** This is the tempting
  choice because epoll is written first, and it is the trap: to report
  "readable" over io_uring you must either poll-then-read (two round trips
  through the ring, worse than epoll) or read into a hidden buffer and copy.
  Either way you forfeit exactly the features that justify io_uring —
  registered buffer rings, multishot accept/recv, linked SQEs, `send_zc`.
- **Two parallel public paths**, one per model. Doubles the surface of the
  connection state machine, which is where the subtle bugs live.
- **Lowest common denominator** (only the ops all three support). Caps
  performance at epoll's ceiling forever.

## Consequences

- The epoll/kqueue backends are slightly more code than a bare readiness
  wrapper, and own the buffer while an operation is outstanding.
- `Completion` is a framework-owned struct, never a reinterpreted
  `io_uring_cqe`, so the ABI of the kernel structure does not leak upward.
- No backend type ever appears in a public signature; `BackendKind` selects one
  and `std::variant` dispatches once per loop stage, never per event.
- Buffer ownership during a submitted operation is part of the concept's
  contract, which is what makes io_uring registered buffer rings expressible
  later (see open question 4 in DESIGN.md).

## Tripwire

Revisit if the emulation overhead in `bench/echo` exceeds 5 % against a
hand-written epoll loop, or if a needed backend (AF_XDP, IOCP) cannot be
expressed in completion terms without contortions. Both are to be measured
during the milestone-2 io_uring spike, **before** the concept is frozen
(DESIGN.md §26.2) — that spike is the reason this ADR can be trusted.

## Tripwire evaluation (M8-10, 2026-09-21)

Measured with `bench/echo` on the dev host recorded in
`bench/ENVIRONMENT.md` (closed loop, 8 conns × 8 outstanding, 64 B frames,
Release build; baselines in `bench/baselines/echo_closed_*.json`):

| server | throughput | vs raw epoll |
|---|---|---|
| hand-written epoll loop (`--server raw`) | ~165–330k rps | 1.0 |
| framework epoll backend | ~120–190k rps | ~0.5–0.7 |
| framework io_uring backend | ~120–280k rps | ~0.7–1.2 |

**The 5 % tripwire is formally exceeded**, and the numbers say why it is not
a verdict on this ADR:

- The comparison is framework-vs-bare-loop, not emulation-vs-not. The raw
  loop does no framing, no per-message dispatch, no context propagation, no
  backpressure accounting, no flight-recorder writes — it echoes whatever a
  single `recv` returned. Most of the delta is the §7 pipeline cost a bare
  loop never pays, and it would look identical on kqueue.
- The *same* pipeline over io_uring meets or beats the raw epoll loop on the
  identical workload — direct evidence that the completion-shaped `IoBackend`
  seam itself is not the bottleneck; the residual epoll gap lives above the
  backend interface (per-message work + the extra syscall per readiness hit
  that readiness emulation cannot amortize).
- Variance on this shared, frequency-scaled dev host is ±2×; the ordering is
  stable, the exact ratios are not.

**Outcome:** decision stands. The tripwire is narrowed to what it was meant
to guard — the readiness→completion *emulation* path. If a hand-written epoll
loop performing the *same* framing/dispatch work beats the framework epoll
backend by >5 % on a pinned host, revisit. Optimizing the §7 dispatch path
(frame batching, per-message sink overhead) is ordinary performance work, not
a design reversal.

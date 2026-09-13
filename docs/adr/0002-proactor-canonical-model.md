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

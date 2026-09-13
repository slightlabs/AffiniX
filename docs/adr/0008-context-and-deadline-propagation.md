# ADR-0008: Context and deadline propagation are part of the core

**Status:** accepted
**Date:** 2026-09-13
**One-way door:** yes

## Context

Work in AffiniX crosses boundaries: a connection read hands off to another
shard, which sets a timer, which issues an outbound connect, which replies back.
Three things need to follow that work item everywhere it goes:

- a **deadline** — the absolute time by which it is pointless to continue;
- a **trace id** — so the hops can be reassembled after the fact;
- a **cancellation token** — so abandonment propagates.

Without a deadline that travels, each stage gets its own independently-chosen
timeout. The sum of those timeouts is the real worst case, it is always larger
than anyone intends, and under overload the server spends its capacity
completing work whose caller has already given up — the classic overload
collapse, where throughput of *useful* work falls as load rises.

Every timeout-taking signature, every ITC entry point and every awaiter is
involved in carrying it. That is what makes it a one-way door: added now it is
a struct and a scope guard; added later it is a breaking change to most of the
public API, and the usual outcome is that it never gets added at all.

## Decision

A `Context` (deadline, trace id, stop token, advisory priority) is ambient per
work item, accessible as `em.context()`, and propagated automatically by
`post`, `post_and_reply`, `channel::push`, `Fan::dispatch` and every coroutine
`co_await`. Details and rules in DESIGN.md §7.4. In summary:

- a hop may only **tighten** a deadline, never extend it (`earliest_of`);
- framework-owned waits (connect, recv, DNS, reply correlation,
  `with_timeout`) derive their timeout from the remaining budget;
- a queued item whose deadline has passed is **dropped before execution** and
  counted, which is the load-shedding mechanism;
- deadline timers live in the work item's `TimerGroup`, so cancellation is
  automatic;
- `with_context()` explicitly starts a fresh unit of work that should not
  inherit the caller's budget.

## Alternatives rejected

- **Per-call timeout arguments only.** What every framework does by default,
  and it produces unbounded total latency plus no load shedding. Users can
  build propagation on top only by threading a parameter through their own
  code *and* through framework waits they do not control.
- **Thread-local context.** Tempting in a thread-affine design, and wrong in
  detail: a context must be captured at *post* time and restored at *run* time,
  which a thread-local does not do by itself. It would also silently leak
  between unrelated work items on the same thread, which is worse than having
  no propagation, because the deadline would be someone else's.
- **Context only in the coroutine layer.** Leaves callback users — the intended
  high-performance path — without it, and splits the framework's semantics
  along an axis users are told is only about syntax.
- **Duration-based (relative) deadlines.** Cheap to represent, but they must be
  decremented at every hop, which means every hop must know its own queueing
  delay. Absolute deadlines are correct by construction within a process
  (relative form is needed only when crossing hosts — DESIGN.md §29, item 10).

## Consequences

- Every ITC message carries ~40 bytes more. Measurable on the highest-rate
  channels; the mitigation options are recorded as open question 7 rather than
  pre-emptively engineered.
- Load shedding becomes a property of the framework rather than something each
  application reinvents, and the two new counters
  (`deadline_expired_before_start`, `deadline_expired_in_flight`) make overload
  visible instead of mysterious.
- Distributed tracing becomes nearly free later (§28.3), since the trace id is
  already flowing.
- Users who do not set a deadline pay one empty-struct copy per hop and get
  today's behaviour.
- New framework code must remember to propagate. This is enforced by the
  `deadline_chain` example and by simulation tests asserting that a budget set
  at ingress bounds every downstream operation.

## Tripwire

Revisit if `Context` copying costs more than 2 % on the `bench/channel`
throughput benchmark (in which case move to a context handle, not to removing
propagation), or if the "tighten only" rule proves too strict for a legitimate
case such as a long-lived subscription spawned from a short request — which
`with_context()` is intended to cover, and whose adequacy should be checked
against a real application before v1.

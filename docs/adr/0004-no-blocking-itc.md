# ADR-0004: No blocking cross-EventManager calls

**Status:** accepted
**Date:** 2026-09-13
**One-way door:** yes

## Context

Users will want a synchronous cross-thread call — `auto r = other.call(q);` —
because it reads well. If the framework provides it, then an EM thread blocks
inside its own loop, which means:

- every connection and timer on that thread stalls for the duration;
- two EMs calling each other deadlock, permanently and non-obviously;
- the same code deadlocks against *itself* if the target resolves to the
  calling EM (easy to hit once sharding is dynamic);
- the stall is invisible in metrics unless specifically instrumented.

These bugs surface under production load and are very hard to reproduce.

## Decision

There is no blocking cross-EM call anywhere in the API. No `future.get()`, no
`call_on()`, no condition variable an EM thread may wait on. The only
request/response mechanism is a posted reply:

```cpp
mb.post_and_reply(Query{id}, em.mailbox(),
                  [](Reply r) { /* resumes on the originating EM */ });
```

The coroutine layer makes this read almost as well as the blocking version
(`auto r = co_await ask(mb, q);`) without blocking anything — which is a large
part of why the coroutine layer exists (ADR-0005).

Debug builds assert that an EM thread never waits on a framework
synchronisation primitive.

## Alternatives rejected

- **Provide it with a deadlock detector.** Detection after the fact still means
  a stalled shard in production, and the detector cannot see user-level cycles.
- **Provide it only for non-EM threads** (a main thread calling into a shard).
  This is actually safe and genuinely useful for startup and tests — but it
  cannot be enforced at compile time, and the one available overload will be
  copied into loop code. Offered instead as a clearly-named
  `Mailbox::blocking_call_from_foreign_thread()`, available only in
  setup/test builds.
- **Expose `std::future` results.** Same problem with a familiar name, which
  makes it more dangerous, not less.

## Consequences

- Some user code becomes continuation-passing. The coroutine layer and
  `post_and_reply` cover most of the ergonomic loss.
- Request/response needs a correlation mechanism (a per-EM pending-request
  table keyed by a handle, with timeout), which the framework provides rather
  than each user reinventing.
- An entire class of production incident is structurally impossible.
- Restrictive-first: if a compelling case appears, a blocking call can be added
  later as opt-in. Removing one after users depend on it is not possible.

## Tripwire

Revisit if a real workload cannot be expressed without it *and* the coroutine
form is measurably too slow — noting that "the code is more awkward" is not a
tripwire, since awkwardness is the intended price of the guarantee.

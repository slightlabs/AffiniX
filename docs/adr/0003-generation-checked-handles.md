# ADR-0003: Objects are named by generation-checked handles, not pointers

**Status:** accepted
**Date:** 2026-09-13
**One-way door:** yes

## Context

In a thread-affine framework, a reference to a connection may outlive the
connection in two distinct ways: across a loop iteration (a callback closes the
connection while an outer frame still holds it) and across threads (a worker
holds a reference to a connection owned by another EM). Both are
use-after-free.

The usual answers are `shared_ptr` (atomic refcount traffic on the data path,
and it merely delays destruction to an unpredictable thread) or raw pointers
plus discipline (which fails in production).

## Decision

EM-owned objects live in slabs and are named by 8-byte handles:

```cpp
struct ConnId  { std::uint32_t idx; std::uint32_t gen; };
struct TimerId { std::uint32_t slot; std::uint32_t gen; };
struct IoId    { std::uint32_t slot; std::uint32_t gen; };
```

- Resolution checks the generation; a stale handle resolves to nothing, and the
  operation returns `Closed` / `false` instead of crashing.
- Slot reuse bumps the generation, so a recycled slot never aliases.
- Slot reclamation is deferred to the end of the loop iteration, so a callback
  cannot free the object it was invoked on.
- Cross-thread use is `ConnHandle {Mailbox, ConnId}`, which posts. There is no
  way to touch another EM's connection directly.

## Alternatives rejected

- **`shared_ptr<Connection>`.** Atomic increments per operation, cross-thread
  destruction on an arbitrary thread, cycles through handlers, and it hides the
  thread-affinity violation rather than preventing it.
- **`weak_ptr`.** Solves the dangling reference, keeps the atomics, and still
  permits touching another EM's object from the wrong thread.
- **Raw pointers plus documentation.** Fast and unsafe; the failure mode is a
  rare production crash, which is the worst kind.

## Consequences

- One indirection (slab index) and one comparison per access. Measured at
  milestone 7; budgeted at well under 1 %.
- Handles are trivially copyable, cheap to log, cheap to put in a message, and
  stable to serialise for tracing.
- Randomised starting generations in debug builds (DESIGN.md §26.4) stop users
  from depending on ids being small or sequential.
- The API is restrictive-first: a pinned/raw reference can be *added* later for
  a proven hot spot without invalidating any existing code, whereas taking
  pointers away would break everyone.

## Tripwire

Revisit if slab indirection shows up above 2 % in `bench/echo` profiles, or if
a legitimate use case needs a stable address for an EM-owned object (e.g.
passing it to a C library) — which would call for an opt-in pinned reference,
not for abandoning handles.

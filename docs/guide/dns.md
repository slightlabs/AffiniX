# Asynchronous DNS

`getaddrinfo(3)` is a blocking syscall — running it on a loop thread is the
classic production stall. `afx::Resolver` owns one dedicated EventManager on
its own thread: requests arrive via that EM's mailbox, `getaddrinfo` runs
there serially (DNS is not a hot path), and the result is posted back to the
requester's mailbox.

```cpp
#include "afx/afx.hpp"

Resolver resolver;                     // spawns its thread at construction

// EM-affine: call resolve() on the EM's own loop thread.
resolver.resolve("example.com", 443, em, /*timeout=*/5s,
                 [](Result<Resolver::AddrList> r) {
                     if (!r) { /* ResolveFailed or Expired */ return; }
                     for (const SockAddr& a : *r) use(a);
                 },
                 /*family=*/AF_UNSPEC);  // or AF_INET / AF_INET6
```

## Semantics

- **EM-affine request, EM-affine reply**: `resolve` runs on the requesting
  EM's thread; the callback fires exactly once on that same thread — with
  the `AddrList`, `Err::ResolveFailed`, or `Err::Expired` when the timeout
  wins the race.
- **Deadline-aware**: the requester's timer wheel owns the deadline. A reply
  that lands after expiry is dropped; the worker also skips jobs whose
  deadline already passed, so a backlog of stale lookups burns no time.
- **Family**: `AF_UNSPEC` returns both families — `TcpClient` interleaves
  them for Happy Eyeballs.
- One `Resolver` per process is the intended shape; the worker thread is a
  `Block`-mode EM (`DefaultPollBackend`) and costs nothing while idle.

## With `TcpClient`

Attach the resolver to `ClientConfig::resolver` and `target` may be a DNS
name:

```cpp
Resolver resolver;
auto cli = em.make_client<MyProto>(ClientConfig{
    .target = {"example.com", 443},
    .resolver = &resolver,
    .happy_eyeballs = true,     // staggered v6-first parallel connects
}, std::move(h));
```

Without a resolver, a non-numeric `target.host` falls back to a **blocking**
`getaddrinfo` inside `start()` — a one-time stall on the EM thread. That
path exists for setup-only code; anything resolving on a live loop should
attach a `Resolver`.

## Test hook

`resolver.set_testing_delay(d)` sleeps `d` inside the worker before each
lookup — exercises the requester-side deadline against a deliberately slow
resolver without depending on external DNS.

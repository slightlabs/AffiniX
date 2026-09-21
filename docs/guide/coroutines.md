# Coroutines

The coroutine layer is opt-in over the callback core (ADR-0005) — nothing in
the core depends on it, and callback-mode servers remain fully supported.
`CoroTask<T>` is lazy, move-only, and `[[nodiscard]]`; every awaiter resumes
on the EM it suspended on, under the task's ambient `Context`.

```cpp
using namespace afx;

CoroTask<void> worker(EventManager& em) {
    co_await em.sleep(50ms);          // Result<void> — Cancelled on stop
}

em.spawn(worker(em));                  // root task: starts now, self-destroys
em.spawn(worker(em), ctx);             // with a Context (trace/deadline/stop)
```

`em.spawn(task, ctx)` starts the task as an EM root: it runs to its first
suspension immediately, is tracked in the EM's root list (cancelled at EM
teardown), and merges `ctx` with the ambient context — trace inherits,
deadlines tighten, stop tokens share.

## Arena-backed frames

A coroutine whose **first parameter** carries the EM —
`session(ConnRef<...>)`, `worker(EventManager&)` — allocates its frame from
the owning EM's `FrameCache`: size-bucketed blocks carved from the NUMA-local
arena. Tasks without an EM-carrying first parameter get ordinary heap frames.

- Frames recycle: a bump arena alone would leak one frame per session.
- Arena exhaustion or oversized frames fall back to `operator new` —
  **visible**, never silent: `em.coro_heap_frames()` and
  `stats().coro_heap_frames` count every fallback.
- A frame never outlives its EM: teardown cancels parked roots first, then
  destroys them.

## `ConnRef` — coroutine sessions

`make_coro_server` turns each accepted connection into a spawned session
coroutine:

```cpp
using Conn = ConnRef<EchoProto, EventManager>;

CoroTask<void> session(Conn c) {
    for (;;) {
        auto rr = co_await c.recv();          // Result<span<const Msg>>
        if (!rr) co_return;                    // peer closed / stopped
        for (auto& m : *rr) {
            // ... build response into stable storage ...
            if (co_await c.send(bytes) == SendResult::Closed) co_return;
        }
    }
}

// in setup:
em.make_coro_server<EchoProto>(cfg, &session);   // session(ConnRef) per conn
```

| `ConnRef` member | Awaits |
|---|---|
| `co_await c.recv()` | Next parsed batch → `Result<std::span<const Message>>` |
| `co_await c.send(bytes)` | Queue bytes; suspends only when the write queue is at the high watermark, resuming below the low watermark → `SendResult` |
| `c.close()` / `c.shutdown_write()` | Sync — no await |
| `c.valid()` / `c.resolve()` / `c.id()` / `c.em()` | Introspection |

**Pacing is free backpressure**: while a `recv` is outstanding the socket
read is armed; once a batch delivers, the read is *not* re-armed — the next
`recv()` drives the next read. A session that stops awaiting stops reading.

**Lifetime**: a delivered batch's bodies point into the connection's read
buffer — valid until the next `recv()` *completes*. Copy the payload into
frame-resident storage (like `examples/session_coro.cpp`'s `frame` vector)
before awaiting again. Batches parsed while no waiter is parked stash into
a bounded `held` list (256 messages); overflowing it closes the connection
with `CloseReason::HeldOverflow` — a session that never reads is a resource
leak the framework refuses to carry.

## `em.sleep(d)`

`co_await em.sleep(d)` → `Result<void>`; wheel resolution (same as
`after()`). Cancellable: a stop request or EM teardown unwinds it with a
`Cancelled`-category error.

## Cross-EM: `coro::post_and_reply`

```cpp
CoroTask<void> fetch(EventManager& em, Mailbox db) {
    auto r = co_await coro::post_and_reply(db, [] { return query(); });
    if (!r) co_return;   // target EM gone / stopped
    use(*r);             // Result<R>
}
```

The work item runs on the target EM; the reply hops back through the home
mailbox and resumes the coroutine. The exchange state is a shared node —
EM teardown may destroy the suspended frame while its request is in flight,
and the reply drops safely.

## Combinators

```cpp
// All children run on the same EM; all inherit context (tighten-only).
auto all = co_await coro::when_all(task_a(em), task_b(em));
//   → Result<tuple<Ra, Rb>>; a child's stored exception rethrows

auto any = co_await coro::when_any(slow(em), fast(em));
//   → Result<pair<index, variant<Ra, Rb>>>; losers get request_stop()

auto t = co_await coro::with_timeout(30s, recv_once(c));
//   → Result<T>; on expiry the child is cancelled and Err::Expired returns
```

`with_timeout` is the idiom for recv-side deadlines — see
`examples/session_coro.cpp`, which wraps `c.recv()` in a one-batch task to
bound each read.

## `TaskScope` — structured concurrency

```cpp
CoroTask<void> parent(EventManager& em) {
    coro::TaskScope scope(em);
    scope.spawn(child(em));       // scope-owned, not an EM root
    scope.spawn(child(em));
    // ...
    co_await scope.join();        // resolves when the last child finishes
}
```

- `request_stop()` cancels every live child — each unwinds through its own
  awaiters (a parked `recv`/`sleep`/`send` resumes with Cancelled).
- `join()` suspends until the last child finishes; children spawned while a
  join is in progress still count.
- A scope destroyed with live children force-destroys them — no frame
  outlives its owner. EM-thread only.

## Exceptions

A coroutine that throws stores the exception in its promise; `co_await`
rethrows it at the awaiting site. Spawned roots have no awaiting site — an
escaping exception on a root terminates through `unhandled_exception`
semantics (store + self-destroy; there is no one to rethrow to).

## Example

`examples/session_coro.cpp` — a coroutine session server with greeting,
echo, and a per-recv 30 s idle deadline via `with_timeout`. Compare it
line-for-line with `echo_server.cpp` to see the callback↔coroutine mapping.

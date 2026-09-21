# EventManager

The `EventManager` (EM) is the per-thread event loop and execution context —
the centre of the programming model. One EM owns one thread; everything it
manages (connections, timers, coroutines, watched fds, deferred work) runs on
that thread only. `afx::EventManager` is the production default:

```cpp
// Linux with AFX_WITH_URING: BasicEventManager<SteadyClock, AutoBackend>
// macOS/BSD or no uring:   BasicEventManager<SteadyClock, DefaultPollBackend>
EventManager em(EventManagerConfig{});
```

`BasicEventManager<Clock, Backend>` is the full template — tests instantiate
`BasicEventManager<VirtualClock, SimBackend>` for deterministic simulation
(see [Simulation](simulation.md)).

## Lifecycle

```cpp
EventManager em(EventManagerConfig{.name = "worker", .wait = WaitStrategy::Block});

// Construct servers, timers, etc. on the thread that will run() — or let
// Runtime do it via Group::each (see runtime.md).

em.run();          // blocks; iterates until stop()
```

| Method | Thread affinity | Meaning |
|---|---|---|
| `run()` | becomes owner | Claim the loop and iterate until `stop()`. |
| `poll_once()` | EM thread | Run exactly one iteration; returns true if any stage did work. The driver for custom scheduling and tests. |
| `stop()` | **any thread** | Idempotent; wakes the backend so `run()` returns promptly. |
| `is_running()` | any | True while `run()` is inside the loop. |
| `is_current()` | any | True on the EM's owning thread — debug builds assert it on every EM-affine entry point. |
| `begin_shutdown(deadline)` | EM thread | Graceful drain sequence (see below). |

`run()` may be called on a different thread than construction — the first
`run()` claims ownership. That is how `Runtime` spawns EMs inside shard
threads while `main` keeps handles to them.

## The iteration

`poll_once()` runs seven stages, in this order (the order is the public
contract — [DESIGN.md](../DESIGN.md) §7.3):

1. **Mailbox drain** — posted tasks run first, bounded by `max_itc_batch`
   per iteration. Items whose `Context::deadline` already expired are
   *shed* — counted in `stats().deadline_expired_before_start`, never run.
2. **Timers** — fire expired timers.
3. **Wait** — block in the backend until an I/O event or the next timer
   deadline (see wait strategies).
4. **Completions** — dispatch I/O completions to their sinks.
5. **Deferred** — run `defer()`-ed tasks, bounded by `max_defer_batch`.
6. **Write flush** — one sendv per dirty connection (write coalescing).
7. **Bookkeeping** — record `IterationInfo`, call `on_idle`/`on_iteration`,
   reclaim dead handle-table slots.

Because user callbacks run inline in stages 1/2/4/5, **a callback must never
block**. A blocking call starves every other connection, timer, and posted
task on the EM — see `stall_threshold` below for the diagnostic.

## Posting work

```cpp
em.defer([] { /* same thread, end of this iteration */ });
em.post([]  { /* any thread, lands in the mailbox */ });
PostResult r = em.mailbox().post([]{ /* from another thread entirely */ });
```

- `defer(Task)` — EM-thread only; runs in stage 5 of the *current* iteration.
  Zero synchronisation.
- `post(Task)` — thread-safe; lands in the EM's bounded mailbox ring and
  wakes the loop. Returns `PostResult::Ok` / `Full` / `Closed`. The ambient
  `Context` (trace id, deadline, stop token) is captured automatically.
- `mailbox()` — a copyable `Mailbox` handle you can hand to other threads
  (see [ITC](itc.md)).

`post()` on a full mailbox honours `EventManagerConfig::mailbox_overflow`:
`Fail` (default — returns `Full`), `SpinRetry` (poll until space frees), or
`Abort` (fail-fast). Posting to a dead EM always returns `Closed` — a
`Mailbox` outlives its EM safely.

## Wait strategies

`EventManagerConfig::wait` selects how stage 3 behaves:

| Strategy | Behaviour | Use |
|---|---|---|
| `Block` (default) | Sleep in the backend until an event or deadline | General services; near-zero idle CPU |
| `SpinThenBlock` | Poll with timeout 0 while work arrived within `spin_budget`, then block | Low-latency services that still idle |
| `Spin` | Never sleep; `wait()` always polls with timeout 0 | Pinned-core dataplanes; burns a core |

With `Spin`, the io_uring backend additionally enables SQPOLL (kernel-side
submission polling — zero-syscall submits) when `uring_sqpoll` is set and the
kernel allows it; it degrades silently to a normal ring otherwise.

## Configuration reference

```cpp
struct EventManagerConfig {
    std::string name;                        // shard label (set by Runtime)
    WaitStrategy wait = WaitStrategy::Block;
    Nanos spin_budget = 50us;                // SpinThenBlock hysteresis
    Nanos timer_tick = 1ms;                  // wheel resolution
    std::size_t max_io_events = 256;         // completion batch cap
    std::size_t max_itc_batch = 128;         // mailbox drain cap per iter
    std::size_t max_defer_batch = 256;       // defer cap per iter
    BackendKind backend = BackendKind::Auto; // Auto/Epoll/Uring/Kqueue/Sim
    MemoryConfig memory{};                   // see memory.md
    CallbackErrorPolicy on_callback_error = CallbackErrorPolicy::RouteToHandler;
    std::size_t mailbox_capacity = 4096;     // power of two
    OverflowPolicy mailbox_overflow = OverflowPolicy::Fail;
    Nanos stall_threshold = Nanos::zero();   // 0 = off; see below
    bool uring_sqpoll = true;                // SQPOLL when wait==Spin
    bool profile_stages = false;             // per-stage latency histograms
};
```

### Callback error policy

A user callback that throws is caught at the dispatch boundary and counted
in `stats().callback_errors`. `on_callback_error` controls what happens next:
`RouteToHandler` (default — count and continue), `CloseConnection` (net
layer closes the offending connection), or `Terminate`.

### Stall detector

With `stall_threshold` set, every guarded dispatch is timed; a callback that
runs longer than the threshold emits a flight-recorder `StateChange` event
and bumps `stats().internal_errors`. Set it at runtime via
`em.set_stall_threshold(ns)` or the admin endpoint
(`POST /admin/stall_threshold?ns=...`). It's a detector, not a watchdog —
the callback still runs to completion.

## Raw fd watching

`watch()` is the escape hatch for fds that aren't AffiniX connections —
eventfd, pipes, device fds, third-party sockets. This is how `UdpSocket` is
implemented internally.

```cpp
auto w = em.watch(fd, Interest::Readable,
                  [](IoId id, std::int32_t mask) {
                      if (mask & CompletionFlag::ReadyRead) { /* read fd */ }
                      if (mask & CompletionFlag::ReadyErr)  { /* ... */ }
                  });
if (w) {
    IoId id = *w;
    em.modify(id, Interest::ReadWrite);   // change interest
    em.unwatch(id);                        // detach + release
}
```

The callback receives a readiness mask (`CompletionFlag::ReadyRead`,
`ReadyWrite`, `ReadyErr`, `ReadyHangup`). On io_uring the watch is a
multishot `POLL_ADD`; on epoll/kqueue it's edge-triggered readiness. The
handler must perform the actual syscall — a readiness notification means
"now is a good time", and the fd must be nonblocking.

## Hooks and introspection

```cpp
em.on_idle([] { /* ran on iterations that found no work */ });
em.on_iteration([](const IterationInfo& i) {
    // i.iteration, i.did_mailbox/timers/io/defer, i.duration
});
const Stats& s = em.stats();          // counters — see observability.md
const LatencyMetrics& l = em.latency();// histograms
FlightRecorder& fr = em.recorder();   // last 4096 events
```

`now()` returns the cached per-iteration time — cheap, and the same clock
timers use. `clock()` exposes the policy object (`VirtualClock` in sim).
`next_wakeup()` reports the earliest instant a timer could fire
(`TimePoint::max()` when none are armed) — the sim scheduler uses it to skip
dead time.

## Context and deadlines

Every dispatched work item runs under a `Context` — deadline, trace id, stop
token, priority ([DESIGN.md](../DESIGN.md) §7.4, ADR-0008):

```cpp
Context c;
c.deadline = Deadline::in(500ms);       // absolute; work is shed after it
c.trace = TraceId{0xabc, 0x123, 1};     // follows every hop
{
    auto scope = em.with_context(c);    // RAII: ambient until restore
    em.after(10ms, [](TimerCtx){ /* runs under c */ });
}
```

- `post()` snapshots the *ambient* context — the context of whatever work
  item is running — so a deadline set once follows the work across mailbox
  hops, timer fires, and coroutine suspensions.
- Deadlines only ever tighten: `Deadline::earliest_of` picks the sooner.
- `em.deadline()` reads the ambient deadline inside a callback.
- Work found expired before it starts is counted in
  `stats().deadline_expired_before_start` and dropped.

## Graceful shutdown

`em.begin_shutdown(deadline)` runs the defined drain sequence (DESIGN.md
§20): listeners close → `on_shutdown` callbacks fire per connection →
in-flight writes drain until the deadline → connections half-close
(`shutdown_write`) → the loop stops and destructors hard-close the rest.
`Runtime::shutdown(timeout)` drives this on every shard through each EM's
mailbox — see [Runtime](runtime.md).

`shutting_down()` reports whether the sequence has begun.

## The raw fd rule

All fds managed by AffiniX must be nonblocking, and all AffiniX-owned fds
(connections, UDP sockets, watches) are closed by the framework — never
`close()` a fd you handed to `watch()` without calling `unwatch()` first.

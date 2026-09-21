# Timers

Timers live on an EventManager and fire on its thread — same affinity rules
as everything else. The implementation is a 4-level × 256-slot hierarchical
wheel for ordinary `after`/`every` timers (O(1) arm/cancel, ≈49-day horizon
at the default 1 ms tick) plus a 4-ary min-heap for `at()` deadlines that
need sub-tick precision and for timers beyond the wheel's horizon.

```cpp
#include "afx/afx.hpp"

EventManager em(EventManagerConfig{});
```

## Arming

```cpp
// One-shot, relative.
TimerId t1 = em.after(100ms, [](TimerCtx c) { /* ... */ });

// One-shot, absolute — precise: goes to the heap, not the wheel.
TimerId t2 = em.at(em.now() + 250ms, [](TimerCtx c) { /* ... */ });

// Repeating every 50 ms (first fire after one period).
TimerId t3 = em.every(50ms, [](TimerCtx c) { /* ... */ });

// Repeating with an initial delay and an explicit mode.
TimerId t4 = em.every(50ms, [](TimerCtx c) { /* ... */ },
                      /*initial_delay=*/5s, RepeatMode::FixedDelay);
```

`every(period, fn, initial_delay = 0, mode = FixedRate, group = {})` — when
`initial_delay` is zero the first fire is after `period`.

All signatures take an optional trailing `TimerGroup` (see below) and capture
the ambient `Context` at arm time — the callback runs under the deadline,
trace id, and stop token that were ambient when it was armed.

## `TimerCtx`

Every callback receives a `TimerCtx`:

```cpp
em.every(50ms, [](TimerCtx c) {
    c.id;          // TimerId of the firing timer
    c.scheduled;   // when it should have fired
    c.now;         // when it actually fired
    c.lateness();  // now - scheduled (Duration)
    c.missed;      // coalesced overruns (FixedRate only, see below)
});
```

`timer_lateness_ns` in `em.latency()` records the observed lateness of every
fire — the quickest way to see whether a shard is keeping up.

## Repeat modes

| `RepeatMode` | Next expiry computed as | Behaviour under overrun |
|---|---|---|
| `FixedRate` (default) | `scheduled + period` | Missed fires are **coalesced**: one fire, `c.missed` counts how many periods elapsed. No catch-up storm after a stall. |
| `FixedDelay` | `now + period` after the callback | Period is measured from completion — no drift, no coalescing. |

Coalesced misses are also counted in `stats().timer_coalesced`.

## Cancelling and rescheduling

```cpp
bool ok = em.cancel(t1);                    // false if already fired/cancelled
bool ok = em.reschedule(t3, 250ms);         // re-arm relative to now
std::optional<Duration> d = em.time_until(t3);  // nullopt when dead
```

`TimerId` is a generation-checked handle: cancelling a timer whose slot has
since been reused can never hit the new occupant. `cancel()` inside the
timer's own callback is legal — so is cancelling an already-fired one-shot
(it returns false).

## Timer groups

A `TimerGroup` is an intrusive list of timers that can be cancelled together
in O(members) — the idiom for "all the timers belonging to one connection /
session / feature":

```cpp
TimerGroup g = em.make_timer_group();

em.after(10ms,  [](TimerCtx) { /* t1 */ }, g);
em.every(5ms,   [](TimerCtx) { /* t2 */ }, Duration::zero(),
         RepeatMode::FixedRate, g);

std::size_t n = em.cancel_group(g);   // cancels all live members
```

The bookkeeping cost of membership is two pointers on the timer node — no
allocation. When the owner goes away, one `cancel_group` cleans up
everything it armed (`TcpClient` uses this for its connect-timeout timers).

## Resolution and precision

- Wheel timers quantize to `EventManagerConfig::timer_tick` (default **1 ms**).
  Deadlines round *up* — a timer never fires early.
- `at()` timers are "precise": they live on the heap and can fire on any
  nanosecond boundary the loop visits.
- `timer_tick` can be tightened (e.g. `100us`) for finer wheel resolution at
  the cost of more frequent expiry passes.

## Interaction with the loop

- Timers run in stage 2 of every iteration, before the backend wait — an
  expired timer delays the wait by zero.
- The backend wait blocks at most until the next timer deadline; a `Block`
  EM with no timers and no I/O sleeps indefinitely (`next_deadline_timeout`
  → 24 h sentinel).
- In simulation, `SimRuntime` jumps virtual time straight to the next
  pending instant — a `em.after(60s, ...)` fires in microseconds of real
  time. See [Simulation](simulation.md).

## Example

`examples/timer_zoo.cpp` exercises every form — one-shots, a `FixedRate`
repeat that reports `missed`, a group whose members cancel each other, and a
final `after` that stops the loop:

```cpp
em.after(100ms, [](TimerCtx) { std::puts("after 100ms"); });

em.every(50ms,
         [](TimerCtx c) { std::printf("tick (missed=%u)\n", c.missed); },
         Duration::zero(), RepeatMode::FixedRate);

auto g = em.make_timer_group();
em.after(10ms, [](TimerCtx) { std::puts("t1"); }, g);
em.after(30ms, [g, &em](TimerCtx) { em.cancel_group(g); }, g);
em.after(40ms, [](TimerCtx) { std::puts("never fires"); }, g);

em.after(300ms, [&em](TimerCtx) { em.stop(); });
em.run();
```

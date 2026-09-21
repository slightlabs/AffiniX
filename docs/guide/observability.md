# Observability

Three layers: per-EM `Stats` + `LatencyMetrics` counters (plain thread-local,
no atomics on the hot path), a 4096-entry `FlightRecorder` ring written
unconditionally, and an `AdminServer` — a real AffiniX HTTP server on its
own `Block`-mode thread that exposes all of it over HTTP.

## Admin HTTP endpoint

```cpp
#include "afx/afx.hpp"

admin::AdminServer admin(rt, admin::AdminConfig{
    .bind = SockAddr::loopback(9100),   // loopback by default; opt in wider
    .gather_timeout = 2s,               // cross-shard collect deadline
    .request_timeout = 10s,             // bounds request read + FIN drain
    .backlog = 16,
});
auto bound = admin.start();             // Result<SockAddr> once listening
// admin.stop() — or let the destructor handle it
```

The endpoint is dogfood: `HttpProto` over `make_server` on a dedicated EM,
so an idle admin costs a spinning dataplane shard nothing.

| Route | Content |
|---|---|
| `GET /healthz` | `200 OK`, `ok\n` |
| `GET /version` | version/build/chaos JSON |
| `GET /stats` | per-shard counter rollup |
| `GET /metrics` | Prometheus-style text exposition |
| `GET /conns` | live connection table |
| `GET /config` | effective configuration |
| `GET /placement` | shard→CPU placement JSON |
| `GET /flight` | flight-recorder records (binary) |
| `GET /` | plain-text endpoint listing |
| `POST /admin/stall_threshold?ns=N[&shard=i]` | set the stall detector |
| `POST /admin/chaos?on=0|1[&shard=i]` | toggle `AFX_DEBUG_CHAOS` paths |
| `POST /admin/log_level?level=debug|info|warn|error` | runtime log level |

Anything else → `404`. Responses are `Connection: close` — one request per
connection. Cross-shard routes (`/stats`, `/conns`, `/flight`…) fan out via
mailbox posts and merge within `gather_timeout`.

```sh
curl -s localhost:9100/metrics
curl -sX POST 'localhost:9100/admin/log_level?level=warn'
```

## `Stats` and `LatencyMetrics`

`em.stats()` returns the per-EM counters — plain `u64`s, read cross-thread
only via a posted gather:

- **loop**: `iterations`, `idle_iterations` (→ `idle_ratio()`), `spins`,
  `blocks`, `wakeups`
- **net**: `accepts`, `conns_opened/closed`, `bytes_in/out`,
  `msgs_in/out`, `frame_errors`
- **backpressure**: `write_hwm_hits`, `write_drops`
- **mailbox**: `mailbox_pushes/pops`, `mailbox_full`
- **timers**: `timers_armed/fired/cancelled`, `timer_coalesced`,
  `groups_cancelled`
- **deadlines**: `deadline_expired_before_start`, `..._in_flight`
- **errors**: `callback_errors`, `internal_errors`
- **coroutines**: `coro_spawned/completed`, `coro_heap_frames` — watch this
  for arena undersizing

`em.latency()` exposes HDR-style `Histogram`s (64 log2 buckets — constant
relative precision, honest at the tail): `iteration_ns`,
`timer_lateness_ns`, `mailbox_queue_ns`, `recv_to_handler_ns`,
`write_queue_depth`, `deadline_headroom_ns`, and a wire-level breakdown —
`nic_to_kernel_ns` (needs synced PHC), `kernel_to_dequeue_ns`,
`dequeue_to_handler_ns`. `profile_stages = true` adds per-`poll_once`-stage
histograms: `mailbox`, `timers`, `wait`, `completions`, `defer`, `flush`,
`bookkeeping`.

## Flight recorder

A fixed ring of 4096 × 32-byte records written unconditionally (~5 ns per
record: one TSC read, masked increment, one store). `AFX_FLIGHT_RECORDER=OFF`
compiles the writes out entirely.

Each record: `{tsc, kind, handle, a, b, trace}` where `kind` is one of
`Accept`, `Recv`, `Frame`, `Send`, `TimerFire`, `StateChange`, `ItcPost`,
`ItcRun`, `Wakeup`, `Backpressure`, `Drop`, `DeadlineExpired`,
`CallbackError`, `User` — and `trace` is `TraceId::short_id()`, so a
context's trace id follows it through the ring.

**Custom records** — `em.recorder().record(EventKind::User, handle, a, b)`
— stamp your own `User` events into the same ring.

### Dump paths

- **Crash**: `install_crash_dump(dir)` (called by `Runtime::start()`;
  standalone binaries call it directly) installs SIGSEGV/SIGABRT/SIGBUS
  handlers that write every registered ring to `afx-crash-<pid>-<sig>.bin`
  using only `open`/`write`/`close` — then re-raise so the normal
  core-dump path still runs.
- **On demand**: `GET /flight` gathers each shard's live window; decode
  with `tools/afx-flight` (raw 32-byte records and framed cross-shard
  dumps).

## Stall detection

`stall_threshold` (default off) bounds a single `poll_once` iteration: an
iteration that runs longer dumps the flight ring and flags the stall. Set
it per-shard live via `POST /admin/stall_threshold?ns=5000000&shard=0` or
in `EventManagerConfig`.

## Chaos toggle

Builds with `AFX_DEBUG_CHAOS=ON` run fault-injection checks on the hot
path; `POST /admin/chaos?on=1` toggles them at runtime — flip chaos on for
a canary shard without restarting.

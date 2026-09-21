# Runtime and placement

`Runtime` owns the threads. It constructs one `EventManager` per shard
inside its own thread, applies CPU affinity and scheduler policy *before*
the EM's memory is allocated (so the arena lands on the right NUMA node),
runs each shard's setup functions, and drives a defined shutdown sequence.

```cpp
#include "afx/afx.hpp"

Runtime rt(Topology::detect());
auto g = rt.spawn_group("io", /*n=*/4, ThreadConfig{});
g.each([](EventManager& em) { /* configure servers, timers, ... */ });
rt.on_signal({SIGINT, SIGTERM}, [&] { rt.shutdown(5s); });
rt.start();   // spawn threads
rt.join();    // wait for all shards to exit
```

The destructor runs `shutdown(5s)` + `join()` itself, so a `Runtime` at
function scope is safe.

## Topology

`Topology::detect()` parses sysfs (`/sys/devices/system/cpu`) into `Core`
records: logical id, package, physical core id, NUMA node, SMT siblings.
`detect(sysfs_root)` accepts an alternate root — the test-suite injects
fixture trees to exercise dual-socket, SMT-off, restricted-cpuset, and
hybrid layouts.

```cpp
Topology topo = Topology::detect();
topo.size();                       // logical CPUs visible
topo.physical_cores();             // one id per physical core
topo.numa_node_of(cpu);            // -1 when unknown
topo.numa_node_of_nic("eth0");     // NIC locality (Linux sysfs)
```

## Groups and shards

`spawn_group(name, n, cfg)` creates `n` shards named `name-0` … `name-(n-1)`
sharing one `ThreadConfig`. Multiple groups coexist — a common shape is a
spinning dataplane group plus a `Block`-mode control group:

```cpp
auto data = rt.spawn_group("data", 8, spin_cfg);
auto ctl  = rt.spawn_group("ctl",  1, block_cfg);
```

`Group::each(fn)` registers a setup function that runs on **each shard's own
thread** before its loop starts; registrations compose (called in order).
`Group::mailboxes()` returns the shard mailboxes — it spins briefly until
each thread has published its EM, so call it after `start()`. `Group::size()`
is the shard count.

```cpp
rt.start();
auto mbs = g.mailboxes();          // post work to shard i via mbs[i]
mbs[0].post([] { /* runs on shard 0 */ });
```

`rt.mailboxes()` collects every shard's mailbox, and `rt.em(i)` returns the
shard's `EventManager*` (nullptr before start).

## Placement

`ThreadConfig::placement` picks how a shard maps to a core:

| `Placement` | Behaviour |
|---|---|
| `OnePerPhysicalCore` (default) | Shards occupy distinct physical cores — SMT siblings are skipped. Right default for spinning loops. |
| `OnePerLogicalCore` | Round-robin over every logical CPU, SMT siblings included. |
| `Explicit` | `ThreadConfig::cores` (`CoreSet`) is the pool; shard *i* gets `cpus()[i % size]`. |
| `None` | No pinning — restricted containers, dev machines. |

```cpp
ThreadConfig cfg;
cfg.placement = Placement::Explicit;
cfg.cores = CoreSet::range(2, 9);        // shards pin to cores 2..9
cfg.em.wait = WaitStrategy::Spin;
cfg.nic_ifname = "eth0";                 // NUMA locality check, see below
```

`CoreSet` is a declarative CPU set: `CoreSet::range(first,last)` (inclusive),
`CoreSet::of({...})`, `CoreSet::all()`, or `.add(cpu)` incrementally.

Pinning failures are reported, not swallowed — except where the platform has
no hard pinning (macOS/BSD), which degrades to unpinned with a warning. Every
shard logs its resolved placement once at startup:

```
afx: shard 'echo-0' -> core 4 (numa 0) wait=spin-then-block
```

## Scheduler policy

`ThreadConfig::sched` is a `SchedPolicy` variant: `Other` (default),
`Fifo{prio}`, or `Rr{prio}`. Real-time policies need `CAP_SYS_NICE` —
a failed `Fifo`/`Rr` apply marks the shard `failed` and it never runs;
`Other` failing is impossible in practice.

## NUMA

`ThreadConfig::numa` (default `LocalAlloc`) becomes the arena's
`NumaPolicy` — see [Memory](memory.md). Because pinning happens before the
EM (and its arena) is constructed, a `LocalAlloc` shard's arena is bound to
the node it's pinned on.

Set `nic_ifname` and startup warns loudly if the shard landed on a different
NUMA node than the NIC (`shard 'x' on numa 1 but NIC 'eth0' is on numa 0`) —
cross-node traffic costs every packet.

`rt.shard_info(i)` reports what was asked vs what happened:
`{name, requested_core, bound_core, numa_node, running, failed}` — the
`/placement` admin endpoint renders exactly this.

## Signals

```cpp
rt.on_signal({SIGINT, SIGTERM}, [&] { rt.shutdown(5s); });
rt.on_signal({SIGHUP},            [&] { /* reload */ });
```

Handled signals are blocked in `main` *before* shards spawn so every thread
inherits the mask; a dedicated signal thread then receives them — `signalfd`
on Linux, a self-pipe + byte-forwarding `sigaction` handler elsewhere. The
callback runs on the signal thread, never in a signal handler, so it may
safely call `rt.shutdown()` (which is just mailbox posts).

## Shutdown

`rt.shutdown(timeout)` is idempotent and drives the drain sequence on every
shard via its mailbox ([DESIGN.md](../DESIGN.md) §20):

1. Listeners close — no new accepts.
2. `Handlers::on_shutdown` fires per live connection — the app's last
   chance to flush a response.
3. In-flight writes drain until the deadline expires.
4. Connections half-close (`shutdown_write` → FIN).
5. Loops stop; destructors hard-close what remains.

`rt.join()` then joins every shard and the signal thread. A shard whose EM
isn't running falls back to plain `stop()`. `rt.stopping()` reports whether
a shutdown has been initiated.

## Inspecting a running system

The admin endpoint renders shard placement and liveness live — see
[Observability](observability.md):

```sh
curl -s localhost:9100/placement
# [{"name":"echo-0","requested_core":4,"bound_core":4,"numa_node":0,
#   "running":true,"failed":false}, ...]
```

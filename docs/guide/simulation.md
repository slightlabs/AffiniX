# Deterministic simulation

`SimRuntime` runs whole multi-EM scenarios on **one thread** with a virtual
clock and a scripted network — no sleeping, no real I/O, same seed →
byte-identical replay traces (M10, DESIGN.md §22.3). This is how the test
suite exercises interleavings that would be flaky against real sockets.

```cpp
SimRuntime sim(seed);                    // one mt19937_64 stream drives all

sim.add_shards(4, [](auto& em) {         // each shard: VirtualClock + SimBackend
    em.make_server<P>(cfg, handlers);    //   attached to the shared SimNet
});

sim.inject(FaultProfile{
    .packet_loss = 0.05,
    .partial_reads = true,
    .slow_peer = 0.1,
    .reset_chance = 0.01,
    .partition_chance = 0.02,
    .partition_window = {100ms, 500ms},
    .min_latency = 1ms, .max_latency = 20ms,
    .slow_latency = 200ms,
});

sim.on_step([](SimRuntime& s) { /* per-step invariant checks */ });
sim.run_for(60s);                        // virtual time — microseconds on wall clock
CHECK(sim.invariants_held());
```

## How a step works

One step: pop every due net event and apply it, then `poll_once` each shard
until quiescent — in a **seeded-shuffled** per-step order, so cross-shard
ordering is part of the seed's interleaving rather than incidental timing.
Virtual time then jumps to the earliest pending instant (net event, timer,
or scheduled post) — no idle grinding.

- `run_for(d)` / `run_until(t)` — advance virtual time.
- `run_until_idle(max_steps)` — drain to quiescence; returns `false` if the
  cap hits (a live-lock smell).
- `sim.post(shard, fn, delay)` — the deterministic analogue of cross-thread
  `Mailbox::post`: lands at a scheduler-chosen instant.
- `sim.shard(i)`, `sim.backend(i)`, `sim.net()` — reach in to drive
  scenarios.
- `sim.trace()` — every delivered event as `{seq, due_ns, kind, fd, aux,
  bytes_hash, shard}`; same seed produces a byte-identical vector, so a
  failing run replays exactly.

## Fault profile

All probabilities are per-delivery draws against the runtime's seeded RNG:

| Knob | Effect |
|---|---|
| `packet_loss` | drop probability per delivery |
| `partial_reads` | fragment deliveries into 1–3 pieces |
| `slow_peer` | fraction of deliveries given `slow_latency` |
| `reset_chance` | RST probability per delivery |
| `partition_chance` + `partition_window` | chance a delivery blackholes its link for a window |
| `min_latency`/`max_latency` | uniform per-link delivery delay |
| `slow_latency` | extra delay for `slow_peer` draws |

TCP's ordered-stream semantics are preserved: per-direction FIFO
watermarks mean fragments never overtake on a link.

## The backend alone

For single-EM tests, use `SimBackend` + `VirtualClock` without a `SimNet` —
the harness feeds bytes in and inspects what the app wrote out:

```cpp
BasicEventManager<VirtualClock, SimBackend> em(cfg, VirtualClock{}, {});
SimBackend& b = em.backend();

int fd = b.add_fd();                    // real socket fd, never kernel-driven
b.feed(fd, "GET / HTTP/1.1\r\n\r\n");   // peer → app bytes
b.deliver_accept(listen_fd, peer_fd);   // pending accept fires
b.deliver_watch(fd, Interest::Readable);// readiness event
b.close_peer(fd);                       // orderly FIN
b.fail_peer(fd, ECONNRESET);            // peer error
const auto& out = b.sent(fd);           // everything the app wrote
em.clock().advance(10ms);               // move virtual time
em.poll_once();
```

Virtual time, virtual wires: `set_timestamping` is a no-op, the SCM_RIGHTS
inbox never fills, `wake_ring_fd()` is `-1` — those are real-kernel
facilities. `wake()` only counts calls (`wake_count()`).

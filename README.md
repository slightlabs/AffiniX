# AffiniX

A C++20 framework for thread-affine, event-driven applications.

> A thread is the unit of concurrency, and an `EventManager` **is** that
> thread's execution context. Nothing is shared implicitly. Cross-thread
> interaction is always an explicit message.

**Status: design only.** No implementation yet. The design is the deliverable
at this stage; the API sketches below are proposals, not shipped interfaces.

## What it is for

- One `EventManager` per thread, with no implicit thread hopping
- Any number of TCP servers and clients per `EventManager`
- Application-defined message headers and framing, inlined into the read loop
- Declarative CPU affinity, SMT-aware placement and NUMA locality
- Lock-free inter-thread communication (task posting, typed channels, fan-out)
- Selectable wait strategy per thread, including busy-polling
- One-shot and repeating timers in the loop, O(1) to arm and cancel, with
  group cancellation
- Deadlines and trace ids that propagate across every hop, with load shedding
  for work whose caller has already given up
- Affinity violations caught at compile time (Clang), plus an always-on flight
  recorder and wire-level timestamps for diagnosing the rest
- Pluggable I/O backend: epoll, io_uring, kqueue, and a deterministic simulator

## Shape of the API

```cpp
struct MyWire {                            // your wire format, your rules
    struct Header { std::uint32_t magic, len; std::uint16_t type; };
    static constexpr std::size_t kHeaderSize = sizeof(Header);
    using Message = afx::FrameView<Header>;
    afx::Result<void>        validate (const Header&) const;
    afx::Result<std::size_t> body_size(const Header&) const;
};

afx::Runtime rt(afx::Topology::detect());
auto io = rt.spawn_group("io", 4, {
    .cores = afx::CoreSet::range(2, 6),
    .em    = {.wait = afx::WaitStrategy::SpinThenBlock},
});

io.each([](afx::EventManager& em) {                 // runs on each shard
    em.make_server<MyWire>({.bind = {"0.0.0.0", 9000}, .reuse_port = true},
        afx::Handlers<MyWire>{
            .on_messages = [](afx::ConnId id, auto batch) { /* ... */ },
        });
    em.every(1s, [](afx::TimerCtx) { publish_stats(); });
});

rt.on_signal({SIGINT, SIGTERM}, [&] { rt.shutdown(30s); });
rt.start();
rt.join();
```

## Documentation

- [docs/DESIGN.md](docs/DESIGN.md) — the full design: threading model,
  `EventManager`, backends, timers, ITC, framing, backpressure, affinity,
  observability, testing, roadmap
- [docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md) — 14 milestones
  with task-level breakdown, exit criteria, spike schedule, CI build-up,
  benchmark methodology and risk register
- [docs/adr/](docs/adr/README.md) — decision records, each with an explicit
  tripwire for revisiting it

The six decisions that are genuinely hard to reverse, and are therefore made
before any code is written:

1. [Clock is a policy type](docs/adr/0001-pluggable-clock.md) — so virtual time
   and deterministic timer tests are possible
2. [Proactor is the canonical I/O model](docs/adr/0002-proactor-canonical-model.md)
   — so io_uring's advantages are not forfeited
3. [Handles, not pointers](docs/adr/0003-generation-checked-handles.md) — so
   stale cross-thread references cannot be use-after-free
4. [No blocking cross-EM calls](docs/adr/0004-no-blocking-itc.md) — so two event
   loops can never deadlock on each other
5. [Context and deadline propagation](docs/adr/0008-context-and-deadline-propagation.md)
   — because it touches every timeout, ITC hop and awaiter
6. [Compile-time affinity annotations](docs/adr/0009-compile-time-affinity-annotations.md)
   — because partial coverage is worse than none

## Requirements (planned)

- C++20: GCC 11+ or Clang 14+ (Clang additionally gives compile-time affinity
  checking; on GCC those annotations expand to nothing)
- CMake 3.24+
- Linux 5.15+ for the io_uring backend (epoll otherwise); macOS/BSD via kqueue

## License

Not yet chosen.

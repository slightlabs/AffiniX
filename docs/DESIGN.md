# AffiniX — Design Document

**Status:** draft (pre-implementation)
**Target:** C++20, Linux first (epoll + io_uring), macOS/BSD second (kqueue)
**Namespace:** `afx`

---

## Table of contents

1. [Purpose and scope](#1-purpose-and-scope)
2. [Design principles](#2-design-principles)
3. [Non-goals](#3-non-goals)
4. [Terminology](#4-terminology)
5. [Architecture](#5-architecture)
6. [Threading model](#6-threading-model)
7. [EventManager](#7-eventmanager)
8. [Wait strategies and polling](#8-wait-strategies-and-polling)
9. [I/O backends](#9-io-backends)
10. [Timers](#10-timers)
11. [Inter-thread communication](#11-inter-thread-communication)
12. [Memory and buffers](#12-memory-and-buffers)
13. [Handles, ownership and lifetime](#13-handles-ownership-and-lifetime)
14. [Protocol and framing](#14-protocol-and-framing)
15. [TCP server and client](#15-tcp-server-and-client)
16. [Backpressure and flow control](#16-backpressure-and-flow-control)
17. [Coroutine layer](#17-coroutine-layer)
18. [Error model](#18-error-model)
19. [Runtime, affinity and NUMA](#19-runtime-affinity-and-numa)
20. [Shutdown and signals](#20-shutdown-and-signals)
21. [Observability](#21-observability)
22. [Testing strategy](#22-testing-strategy)
23. [Repository layout and build](#23-repository-layout-and-build)
24. [Performance targets](#24-performance-targets)
25. [Security considerations](#25-security-considerations)
26. [Evolvability policy](#26-evolvability-policy)
27. [Roadmap](#27-roadmap)
28. [Extensions](#28-extensions)
29. [Open questions](#29-open-questions)

---

## 1. Purpose and scope

AffiniX is a C++20 framework for building thread-affine, event-driven
applications: network servers and clients, market-data handlers, proxies,
simulation engines, and any program whose performance depends on controlling
*which* core does *what*.

The organising idea is stated once because every other decision follows from
it:

> A thread is the unit of concurrency, and an `EventManager` **is** that
> thread's execution context. Nothing is shared implicitly. Cross-thread
> interaction is always an explicit message.

This yields a shared-nothing, shard-per-core architecture. The alternative — a
thread pool servicing a shared queue of work, with locks around shared state —
is easier to start with and much harder to make predictable at the tail. AffiniX
chooses predictability.

### Required capabilities

The framework must support, as first-class features:

- one `EventManager` per thread, with no implicit thread hopping;
- any number of TCP servers and clients per `EventManager`;
- application-defined TCP message headers and framing;
- CPU affinity control, opt-in, expressed declaratively;
- inter-thread communication without locks on the fast path;
- busy-polling mode in the event loop, selectable per thread;
- one-shot and repeating timers in the event loop.

---

## 2. Design principles

1. **Affinity is explicit.** A callback registered on `EventManager` *N* runs
   only on *N*'s thread, for the lifetime of the registration. There is no
   migration, no stealing, no "some thread in the pool".
2. **No hidden costs on the data path.** No `shared_ptr`, no `std::function`,
   no virtual dispatch, no allocation, and no locks per I/O event. Anything
   that costs must be visible at the call site.
3. **Bounded work per loop iteration.** Every stage of the loop has a budget.
   Unbounded stages are how event loops develop multi-millisecond tails.
4. **Compile-time polymorphism by default.** Concepts and templates for
   protocols, backends and clocks; a single runtime dispatch at most, at the
   outermost boundary.
5. **Restrictive first.** Where a choice is unclear, ship the constrained
   version. Loosening an API is backward compatible; tightening is not.
6. **Pluggable where it is hard to change later.** Clock, I/O backend and
   protocol are seams from day one, because retrofitting them is impractical.
7. **Explicit lifetime.** Cross-thread and cross-iteration references use
   generation-checked handles, not pointers.
8. **Measurable by construction.** Per-thread statistics and latency
   histograms are part of the core, not an add-on.

---

## 3. Non-goals

- **Not a thread-pool executor.** Work does not migrate between threads.
- **Not an HTTP/RPC framework.** Those are layers built on top (§28).
- **Not transparently distributed.** No implicit clustering or remoting.
- **Not a coroutine runtime.** Coroutines are an opt-in layer over a
  callback core (§17), not the foundation.
- **No work stealing.** It contradicts affinity. CPU-bound offload is served
  by an explicit `WorkerPool` (§28), never by the I/O loops.
- **No `std::execution` foundation.** A sender/receiver adapter may be added
  after v1; building on it now would consume the entire budget.

---

## 4. Terminology

| Term | Meaning |
|---|---|
| **EventManager** (EM) | The per-thread event loop, owner of all its resources |
| **Runtime** | Owns threads and places EMs onto cores |
| **Mailbox** | Copyable, thread-safe handle used to send work to an EM |
| **Backend** | The OS I/O mechanism (epoll, io_uring, kqueue) |
| **Completion** | A finished I/O operation delivered by a backend |
| **Protocol** | Application-supplied type that frames a byte stream |
| **Shard** | One EM plus the state it exclusively owns |
| **Handle** | `{index, generation}` pair naming an EM-owned object |
| **Context** | Ambient per-work-item data: deadline, trace id, cancellation |
| **Deadline** | Absolute time by which a work item must complete (§7.4) |
| **TimerGroup** | A set of timers cancellable as one unit (§10.1) |
| **Flight recorder** | Per-EM ring of recent events, dumped on fault (§21.1) |

---

## 5. Architecture

Layers depend only downward. Each layer is independently usable and testable.

```
L5  services/   rpc skeleton, http codec, discovery            (optional)
L4  net/        Acceptor, TcpServer<P>, TcpClient<P>,
                Connection<P>, Codec chain, UdpSocket
L3  itc/        Mailbox, post/defer, channel<T>, Fan, Barrier, Sharded<T>
L2  core/       EventManager, TimerWheel, HandleTable,
                IoBuffer, pools, Stats
L1  backend/    IoBackend concept -> EpollBackend, UringBackend, KqueueBackend,
                SimBackend
L0  sys/        affinity, topology, NUMA, hugepages, eventfd/timerfd/signalfd,
                Clock (real + virtual), Result/Error
```

`L0`–`L3` contain no networking code, so an `EventManager` is useful as a pure
timer-and-messaging engine on a thread that never touches a socket (a strategy
thread, a disk writer, a control plane).

---

## 6. Threading model

### 6.1 Rules

- An `EventManager` is constructed on, and bound to, the thread that runs it.
  It is neither copyable nor movable.
- All of an EM's objects (connections, timers, buffers, pools) are owned by
  that EM and touched only by its thread.
- The **only** thread-safe members of `EventManager` are `post()`, `stop()`,
  and `mailbox()`. Everything else asserts `is_current()` in debug builds and,
  where the toolchain supports it, fails to compile off-thread (§6.3).
- There is **no blocking cross-EM call** (§11.4). Request/response is done by
  posting a reply, never by waiting.

### 6.2 Typical topologies

```
Symmetric shards (SO_REUSEPORT):     Pipeline:
  EM0 [accept, io, logic] core2        EM0 [io]      core2
  EM1 [accept, io, logic] core3   -->  EM1 [decode]  core3
  EM2 [accept, io, logic] core4   -->  EM2 [logic]   core4
  EM3 [accept, io, logic] core5   -->  EM3 [egress]  core5
```

Symmetric sharding scales linearly and is the recommended default. Pipelines
are appropriate when a stage needs exclusive access to large state, at the cost
of one ITC hop (and one cache transfer) per stage boundary.

### 6.3 Compile-time enforcement of affinity

The central invariant — "only this EM's thread touches this object" — is
expressible to the compiler today via Clang's thread-safety analysis, using the
EM as a *capability* rather than a real lock:

```cpp
class CAPABILITY("em") EmContext {};      // never locked; purely a token

class Connection {
public:
    SendResult send(ByteSpan) AFX_REQUIRES(em_);   // wrong thread => build error
private:
    EmContext& em_;
};

// Inside the loop, the EM asserts the capability once per iteration:
void EventManager::run() AFX_EXCLUDES(ctx_) { ... }
```

Every non-thread-safe member of `EventManager`, `Connection`, `TcpServer`,
`TimerWheel` and the pools carries `AFX_REQUIRES(em_)`; the thread-safe three
(`post`, `stop`, `mailbox`) do not. Calling an EM-affine method from a worker
thread, from a `std::thread` lambda, or from another EM's callback is then a
compile error rather than a debug-time assert.

Honest limits, which is why this is a policy and not a guarantee:

- **Clang only.** GCC has no equivalent analysis. The `AFX_REQUIRES` /
  `AFX_EXCLUDES` / `AFX_GUARDED_BY` macros expand to nothing on GCC, so GCC
  builds are unaffected and unchecked. CI therefore runs a dedicated Clang job
  with `-Wthread-safety -Werror`, and that job is the enforcement point.
- **Analysis is intra-procedural and syntactic.** It catches a direct call on
  the wrong thread; it does not follow a capability through a type-erased
  `std::function` or a handle resolved at runtime. Handles (§13) and the debug
  `is_current()` asserts cover what the analysis cannot see. The three
  mechanisms are complementary, not redundant.
- **Coverage must be total from the start.** Partial annotation is worse than
  none, because users trust a clean build. Adding an unannotated public method
  is a review error and is checked by a script that greps public headers for
  non-thread-safe members lacking an annotation.

See [ADR-0009](adr/0009-compile-time-affinity-annotations.md).

---

## 7. EventManager

### 7.1 Configuration

```cpp
namespace afx {

enum class BackendKind { Auto, Epoll, Uring, Kqueue, Sim };

struct MemoryConfig {
    std::size_t  arena_bytes      = 8u << 20;
    std::size_t  read_buffer_size = 64u << 10;
    bool         hugepages        = false;
    NumaPolicy   numa             = NumaPolicy::Default;
    bool         prefault         = true;   // touch pages at init, not at peak
};

struct EventManagerConfig {
    std::string   name;                             // thread name + metric label
    WaitStrategy  wait          = WaitStrategy::Block;
    Nanos         spin_budget   = 50us;             // SpinThenBlock only
    Nanos         timer_tick    = 1ms;              // wheel granularity
    std::size_t   max_io_events = 256;              // completions per poll
    std::size_t   max_itc_batch = 128;              // messages per iteration
    std::size_t   max_defer_batch = 256;
    BackendKind   backend       = BackendKind::Auto;
    MemoryConfig  memory{};
    CallbackErrorPolicy on_callback_error = CallbackErrorPolicy::RouteToHandler;
};

} // namespace afx
```

Every bound above exists so that one busy connection, one flood of messages, or
one runaway timer cannot starve the other stages.

### 7.2 Public interface

```cpp
class EventManager {
public:
    explicit EventManager(EventManagerConfig);
    ~EventManager();
    EventManager(const EventManager&)            = delete;
    EventManager& operator=(const EventManager&) = delete;

    // ---- lifecycle ------------------------------------------------------
    void run();                     // blocks until stop(); the common case
    bool poll_once();               // one iteration; embed in a foreign loop
    void stop() noexcept;           // thread-safe, idempotent
    bool is_current() const noexcept;
    bool is_running() const noexcept;

    // ---- clock ----------------------------------------------------------
    TimePoint now() const noexcept; // cached once per iteration

    // ---- work -----------------------------------------------------------
    void defer(Task&&);             // same thread, end of this iteration
    void post (Task&&);             // thread-safe, from anywhere
    Mailbox mailbox() const noexcept;

    // ---- context: deadline, trace, cancellation (§7.4) -------------------
    const Context& context() const noexcept;   // ambient for the running item
    Deadline       deadline() const noexcept;  // == context().deadline
    [[nodiscard]] ContextScope with_context(Context);

    // ---- timers ---------------------------------------------------------
    TimerId after(Duration, TimerFn&&, TimerGroup = {});
    TimerId at   (TimePoint, TimerFn&&, TimerGroup = {});
    TimerId every(Duration period, TimerFn&&,
                  Duration initial_delay = Duration::zero(),
                  RepeatMode = RepeatMode::FixedRate,
                  TimerGroup = {});
    bool    cancel(TimerId) noexcept;          // safe on stale ids
    bool    reschedule(TimerId, Duration);
    std::optional<Duration> time_until(TimerId) const;

    TimerGroup make_timer_group();
    std::size_t cancel_group(TimerGroup) noexcept;   // O(group size)

    // ---- raw fd escape hatch --------------------------------------------
    Result<IoId> watch(int fd, Interest, IoFn&&);
    Result<void> modify(IoId, Interest);
    void         unwatch(IoId) noexcept;

    // ---- factories: unlimited servers/clients per EM ---------------------
    template <Protocol P, class Handlers>
    Result<TcpServer<P>*> make_server(ServerConfig, Handlers&&);

    template <Protocol P, class Handlers>
    Result<TcpClient<P>*> make_client(ClientConfig, Handlers&&);

    template <Protocol P, class Handlers>
    Result<UdpSocket<P>*> make_udp(UdpConfig, Handlers&&);

    // ---- hooks ----------------------------------------------------------
    void on_idle(IdleFn&&);         // called when an iteration found no work
    void on_iteration(IterationFn&&); // profiling hook, debug builds

    // ---- introspection --------------------------------------------------
    const Stats& stats() const noexcept;
    const EventManagerConfig& config() const noexcept;
    FlightRecorder& recorder() noexcept;       // §21.1
};
```

Servers and clients are owned by the EM and returned as raw pointers whose
lifetime is the EM's; they are closed either explicitly or at EM destruction.
There is no limit on how many exist per EM.

### 7.3 Loop iteration

The stage order is part of the public contract, because users reason about it:

```
1. drain mailbox          (<= max_itc_batch messages)
2. expire timers          (<= one tick's worth of due timers)
3. backend wait           (timeout: 0 while spinning, else next deadline)
4. dispatch completions   (<= max_io_events)
5. run deferred tasks     (<= max_defer_batch)
6. flush pending writes   (the write-coalescing point)
7. bookkeeping            (stats, on_idle, stop check)
```

Notes:

- Stage 6 is what makes request/response cheap: a handler that calls `send()`
  in stage 4 does not issue a syscall; the loop coalesces all writes per
  connection into one `writev`/`send_zc` at the end of the iteration.
- The timeout for stage 3 is `min(next_timer_deadline, remaining_spin_budget)`,
  clamped to the timer tick.
- What is **not** specified, and is actively randomized in debug builds (§26.4):
  the relative order of independent connections' callbacks, the split of a byte
  stream across `on_message` batches, and deferred-task ordering.

### 7.4 Context and deadline propagation

A work item's *context* is ambient data that must follow the work wherever it
goes — including across ITC hops, into timers, and into I/O operations.

```cpp
struct Context {
    Deadline  deadline{};        // absolute TimePoint; {} == none
    TraceId   trace{};           // 16-byte trace id + 8-byte span id
    StopToken stop{};            // cooperative cancellation
    std::uint32_t priority = 0;  // advisory; used by Fan and WorkerPool
};

class Deadline {
public:
    static Deadline in(Duration d);          // now() + d
    bool     expired(TimePoint now) const noexcept;
    Duration remaining(TimePoint now) const noexcept;
    Deadline earliest_of(Deadline other) const noexcept;   // never extends
};
```

Rules:

- `em.context()` returns the context of the work item currently running. It is
  established by whatever delivered that item: a connection read, a timer, a
  mailbox message, a coroutine resumption.
- **Propagation is automatic.** `post`, `post_and_reply`, `channel::push`,
  `Fan::dispatch` and every coroutine `co_await` carry the current context to
  the next hop. A hop can only *tighten* a deadline
  (`earliest_of`), never extend it.
- **Consumption is automatic where the framework owns the wait.** `connect`,
  `recv`, DNS resolution, `post_and_reply` correlation and
  `co_await with_timeout` all derive their timeout from
  `context().deadline.remaining(em.now())`, so a budget set once at ingress
  bounds everything downstream without the user threading timeouts by hand.
- **Expiry is checked before dispatch.** If a queued item's deadline has
  already passed when it reaches the head of the mailbox, it is dropped (and
  counted as `deadline_expired_before_start`) rather than executed. Under
  overload this is the mechanism that sheds the work nobody is waiting for any
  more — which is precisely when a server most needs it.
- A deadline-derived timer is an ordinary wheel entry in the work item's
  `TimerGroup` (§10.1), so cancelling the group cancels the timeout too.
- `with_context()` returns a scope guard for explicitly starting a new unit of
  work (a background refresh that should *not* inherit a request's 5 ms
  budget).

Why this is in the core rather than in an extension: every signature that takes
a timeout, every ITC entry point, and every awaiter is involved. Added now it is
one field and a scope guard; added later it is a breaking change to most of the
API. Trace-context propagation and priority ride along for free once the
mechanism exists. See
[ADR-0008](adr/0008-context-and-deadline-propagation.md).

---

## 8. Wait strategies and polling

```cpp
enum class WaitStrategy {
    Block,          // sleep in the backend until an event or deadline
    SpinThenBlock,  // poll with timeout 0 for spin_budget, then block
    Spin,           // never sleep
};
```

| Strategy | Wakeup latency | Core cost | Use when |
|---|---|---|---|
| `Block` | 5–50 µs | idle-friendly | control planes, background shards |
| `SpinThenBlock` | ~1 µs under load | partial | the sensible default |
| `Spin` | ~100–300 ns | one full core | isolated core, latency is the product |

`Spin` pairs with `SO_BUSY_POLL` / `SO_PREFER_BUSY_POLL`, `isolcpus` +
`nohz_full`, and io_uring `SQPOLL`.

### 8.1 The arm/block protocol

Transitioning from spinning to blocking is the single most bug-prone mechanism
in the framework: a producer that enqueues during the transition must not have
its wakeup lost. It is solved once, in `Mailbox`, and nothing else needs to
reason about it.

```
Consumer (EM thread)                    Producer (any thread)
--------------------                    ---------------------
spin budget exhausted                   push(msg)                      (1)
state.store(Blocked, seq_cst)   (A)     if (state.load() == Blocked)    (2)
if (!queue.empty())             (B)         eventfd_write(fd, 1)        (3)
    state.store(Running); continue
backend.wait(timeout)           (C)
state.store(Running)
```

Correctness argument: (A) precedes (B), and (1) precedes (2), both with
`seq_cst` ordering on `state`. If the producer at (2) does not observe
`Blocked`, then (A) has not yet happened, so (B) has not yet run and will
observe the pushed message. If it does observe `Blocked`, it signals, and the
`eventfd` is in the wait set at (C). Either way the consumer makes progress.

The consequence worth stating: in `Spin` mode, `state` is never `Blocked`, so
the `eventfd` write is never paid. Cross-thread posting costs one ring push.

---

## 9. I/O backends

### 9.1 The seam

The backend is selected by `BackendKind` at construction and then never
dispatched dynamically per operation: `EventManager` holds a `std::variant` of
backends and resolves it once per loop stage, not once per event.

```cpp
struct Completion {
    UserData      user;      // 64-bit tag: handle + op kind
    std::int32_t  result;    // bytes transferred, or -errno
    std::uint32_t flags;     // incl. HasHwStamp, HasSwStamp, More (multishot)
    Timestamps    stamps;    // see below; zeroed when unavailable
};

struct Timestamps {          // 24 bytes, all optional
    std::uint64_t hw_ns;     // NIC hardware receive time (SO_TIMESTAMPING)
    std::uint64_t sw_ns;     // kernel software receive time
    std::uint64_t tsc;       // TSC at the moment the loop dequeued it
};

template <class B>
concept IoBackend = requires (B b, int fd, Interest i, UserData u,
                              MutByteSpan mut, ByteSpan in,
                              std::span<Completion> out, Nanos timeout) {
    { b.attach(fd, i, u) }        -> std::same_as<Result<void>>;
    { b.modify(fd, i, u) }        -> std::same_as<Result<void>>;
    { b.detach(fd) }              -> std::same_as<Result<void>>;

    { b.submit_recv(u, fd, mut) } -> std::same_as<Result<void>>;
    { b.submit_send(u, fd, in) }  -> std::same_as<Result<void>>;
    { b.submit_sendv(u, fd, std::span<const ByteSpan>{}) };
    { b.submit_accept(u, fd) };
    { b.submit_connect(u, fd, SockAddr{}) };
    { b.cancel(u) };

    { b.wait(out, timeout) }      -> std::same_as<int>;  // completions written
    { b.wake() }                  -> std::same_as<void>; // from another thread
    { B::kProactor }              -> std::convertible_to<bool>;
};
```

### 9.2 Proactor as the canonical model

Reactor semantics ("the fd is readable, now you read") and proactor semantics
("your read has completed") are not trivially interchangeable. The choice made
here is that **the proactor model is canonical**, and readiness-based backends
emulate it:

- `EpollBackend::submit_recv` records the buffer, arms `EPOLLIN` (edge
  triggered), and on readiness performs the `recvmsg` itself, then synthesises a
  `Completion`. Cost is one extra branch and one stored pointer per operation.
- The reverse mapping — synthesising readiness over io_uring — would forfeit
  registered buffers, multishot accept/recv, linked SQEs and `send_zc`, which
  are the reasons to want io_uring at all.

This is a one-way door; see [ADR-0002](adr/0002-proactor-canonical-model.md).

### 9.3 Backend matrix

| Capability | epoll | io_uring | kqueue | sim |
|---|---|---|---|---|
| Emulated proactor | yes | native | yes | native |
| Multishot accept/recv | no | ≥ 5.19 | no | n/a |
| Registered buffers | no | yes | no | n/a |
| Zero-copy send | `MSG_ZEROCOPY` | `send_zc` | no | n/a |
| Batched submission | no | yes (one syscall) | partial | n/a |
| Timer source | `timerfd` | `IORING_OP_TIMEOUT` | `EVFILT_TIMER` | virtual clock |
| Wakeup source | `eventfd` | `eventfd`/`MSG_RING` | `EVFILT_USER` | direct |

### 9.4 Timestamping

`Completion::stamps` exists so that latency can be measured from the *wire*
rather than from the loop. With `SO_TIMESTAMPING` enabled (per socket, opt-in
via `SocketOptions::timestamping`), the backend extracts the NIC hardware and
kernel software receive times from the control message and fills `hw_ns` /
`sw_ns`; `tsc` is always stamped when the loop dequeues the completion.

This turns three otherwise-invisible intervals into metrics (§21):
NIC→kernel, kernel→loop dequeue, and dequeue→handler entry. The middle one is
how you prove a latency spike was kernel queueing or a missed wakeup rather
than application code.

The seam matters more than the feature: `Completion` is a struct that would
otherwise be frozen without a place to put a timestamp, and adding a field to
it later changes every backend and every dispatch site. Hardware stamps need
PTP/`phc2sys` to be comparable with `CLOCK_REALTIME`; when the NIC does not
support them, `hw_ns` is zero and the software stamp is used, which is stated
explicitly because silently substituting clocks would make the histograms lie.

### 9.5 Backend selection

`Auto` probes io_uring with `IORING_REGISTER_PROBE` for the ops actually used,
falls back to epoll on Linux and kqueue elsewhere, and logs the choice once at
startup. `AFX_BACKEND_ONLY=<kind>` compiles out the others entirely.

`SimBackend` (§22.3) is deterministic, needs no kernel objects, and exists from
the start so that the concept has two genuinely different implementations
before it is frozen (§26.2).

---

## 10. Timers

Two data structures, selected by deadline, because no single structure serves
both patterns well.

**Hierarchical timing wheel** — 4 levels × 256 slots, tick = `timer_tick`,
covering `tick × 256^4` (≈ 49 days at 1 ms). O(1) insert, O(1) cancel, O(1)
amortised expiry. This carries the dominant network-code pattern: arm a 5 s
timeout, cancel it 200 µs later, millions of times.

**Four-ary min-heap** — for `at()` deadlines needing sub-tick precision and for
computing the backend wait timeout. O(log n), cache-friendlier than a binary
heap.

```cpp
struct TimerId {                  // 8 bytes, trivially copyable
    std::uint32_t slot = 0;
    std::uint32_t gen  = 0;
    bool valid() const noexcept { return gen != 0; }
};

struct TimerCtx {
    TimerId    id;
    TimePoint  scheduled;   // when it should have fired
    TimePoint  now;         // when it actually fired
    std::uint32_t missed;   // coalesced overruns (FixedRate only)
    Duration   lateness() const { return now - scheduled; }
};

using TimerFn = InlineFn<void(TimerCtx), 48>;  // inline storage, heap fallback
```

- **Generation counters** make `cancel()` of an already-fired or already-
  cancelled id a safe no-op rather than a use-after-free. Same mechanism as
  connection handles (§13).
- **Repeat modes:** `FixedRate` (next = scheduled + period, so drift does not
  accumulate) and `FixedDelay` (next = now + period). On overrun, `FixedRate`
  **coalesces** missed ticks into one callback and reports `missed`, rather
  than queueing a burst.
- **Cancellation from a callback** is legal, including self-cancellation;
  removal is deferred to the end of the expiry stage.
- **Clock:** `Clock::now()` is called once per iteration and cached
  (`em.now()`). A callback that needs a fresher reading calls
  `Clock::now_uncached()` explicitly. The clock is a policy type, which is what
  makes virtual time possible (§22.2) — see
  [ADR-0001](adr/0001-pluggable-clock.md).

```cpp
em.every(1s, [](TimerCtx c) {
    if (c.missed) stats.heartbeat_overruns += c.missed;
    send_heartbeat();
});
auto t = em.after(5s, [id](TimerCtx) { close_idle(id); });
// ... later, on the same thread:
em.cancel(t);   // O(1), always safe
```

### 10.1 Timer groups

Most timers belong to something: a connection, a request, a session. When that
thing ends, all of its timers must go, and making users track a vector of
`TimerId`s for this is both tedious and a reliable source of leaked timers.

```cpp
struct TimerGroup { std::uint32_t idx; std::uint32_t gen; };   // a handle, as ever

auto g = em.make_timer_group();
em.after(5s,  on_handshake_timeout, g);
em.every(1s,  on_heartbeat,         g);
em.after(30s, on_idle_timeout,      g);
// connection closes:
em.cancel_group(g);        // all three, one call
```

Implementation is an intrusive doubly-linked list threaded through the wheel
nodes, so group membership costs 16 bytes per timer and nothing in time.
Cancelling a group is O(group size) with no wheel traversal.

Every framework-internal timeout (connection idle, handshake, connect, deadline
expiry, reconnect backoff) is registered in the owning object's group, which is
what makes "close the connection and nothing dangles" true by construction
rather than by remembering. The reason this is core and not an extension: it
fixes the layout of the wheel node, which is the one part of the timer
implementation that everything else is built on.

---

## 11. Inter-thread communication

Three tiers, deliberately. Offering only the general-purpose one would push
users into type erasure on their hottest path.

### 11.1 Tier 1 — task posting (flexible, MPSC)

```cpp
Mailbox mb = other_em.mailbox();     // copyable, thread-safe, 32 bytes
mb.post([x] { handle(x); });         // InlineFn<void(), 48>, heap fallback
mb.post_batch(span_of_tasks);        // one arm/block check for the whole batch
```

Bounded by construction: the ring has a fixed capacity and `try_post()` returns
`PostResult::Full` rather than growing without limit. `post()` applies the
mailbox's configured `OverflowPolicy` (`Fail`, `SpinRetry`, or `Abort`).

### 11.2 Tier 2 — typed channels (the fast path)

```cpp
auto [tx, rx] = afx::channel<MarketTick>(4096, ChannelKind::SPSC);
rx.attach(em, [](std::span<MarketTick> batch) { for (auto& t : batch) ...; });
tx.try_push(tick);                   // wait-free, no type erasure, no alloc
tx.try_push_bulk(ticks);
```

- `SPSC`: cache-line-padded head/tail, batch claim on both sides, wait-free.
- `MPSC`: slot-sequence (Vyukov) ring. Benchmarked against
  `moodycamel::ConcurrentQueue` before committing to the hand-rolled version.
- Receivers are drained in loop stage 1 and delivered **as batches**, so the
  consumer amortises its own per-item work.

### 11.3 Tier 3 — topology helpers

```cpp
afx::Fan<Request> fan(rt.mailboxes(), FanPolicy::HashBy{&Request::key});
fan.dispatch(req);                       // shard-affine routing
fan.broadcast(Tick{...});

afx::Barrier barrier(rt, [] { swap_config(); });  // coordinated reconfiguration

afx::Sharded<OrderBookMap> books(rt);    // one instance per EM
books.invoke_on(shard_of(sym), [sym](OrderBookMap& m) { m.touch(sym); });
books.map_reduce(count_fn, std::plus<>{}, [](std::size_t total) { ... });
```

`Sharded<T>` is the intended replacement for a mutex-guarded global; it exists
so users are not tempted to reintroduce shared mutable state.

### 11.4 No blocking cross-EM calls

There is no `future.get()`, no synchronous `call_on()`, no condition variable
an EM thread may wait on. Request/response is:

```cpp
mb.post_and_reply(Query{id}, em.mailbox(),
                  [](Reply r) { /* runs back on the originating EM */ });
```

This eliminates the entire class of two-loops-deadlocked-on-each-other bugs,
which are miserable to diagnose in production. Debug builds assert that no EM
thread blocks on a synchronisation primitive owned by the framework. See
[ADR-0004](adr/0004-no-blocking-itc.md).

---

## 12. Memory and buffers

### 12.1 IoBuffer

```cpp
class IoBuffer {
public:
    // layout: [ headroom | readable | writable ]
    ByteSpan     readable() const noexcept;
    MutByteSpan  writable(std::size_t at_least);
    void         commit(std::size_t n);     // producer wrote n bytes
    void         consume(std::size_t n);    // consumer read n bytes
    MutByteSpan  prepend(std::size_t n);    // write a header in front, no copy
    void         reserve(std::size_t);
    void         compact();                 // reclaim consumed prefix
};
```

`prepend` exists so that serialising a body and then writing a header in front
of it is not a copy — the common shape of every framed protocol.

### 12.2 Allocation policy

- Per-EM arena and slab allocators. No cross-thread `free` on the data path; a
  block that must cross threads is returned by mailbox, or, preferably, the
  design avoids it.
- Object pools for `Connection`, framing state, timer nodes and buffers,
  sized at startup.
- **Steady state is allocation-free.** Enforced by a test that poisons `malloc`
  after initialisation and runs the echo server (§22.4).
- Optional hugepages plus `mbind` to the EM's NUMA node; `prefault` touches
  pages at init so the first burst does not take page faults.

### 12.3 Scatter/gather

```cpp
conn.send(header_bytes);                        // coalesced in stage 6
conn.send_scatter(std::array{hdr, body, tail}); // writev / vectored SQE
conn.send_zero_copy(large_span, on_release);    // MSG_ZEROCOPY / send_zc
```

---

## 13. Handles, ownership and lifetime

This is the mechanism that makes a thread-affine framework *safe* at speed.

```cpp
struct ConnId {
    std::uint32_t idx = 0;
    std::uint32_t gen = 0;
    bool valid() const noexcept { return gen != 0; }
};
```

- Connections live in a slab owned by their EM. Users never hold a
  `Connection*` across an iteration boundary and never hold one on another
  thread — they hold a `ConnId`.
- `ConnRef` resolves an id with a generation check; operations on a dead
  connection return `SendResult::Closed`, they do not crash.
- Slot reuse bumps the generation, so a stale id can never alias a new
  connection.
- Slot reclamation is **deferred to the end of the iteration**, so a callback
  cannot free memory out from under the code that invoked it.

```cpp
// Cross-thread send, explicit about the hop:
ConnHandle h = conn.handle();             // {Mailbox, ConnId}
h.send(bytes);                            // == mb.post([=]{ resolve(id).send(bytes); })
```

See [ADR-0003](adr/0003-generation-checked-handles.md).

### 13.1 Connection state machine

```
                 ┌──────────────┐
   connect() ──> │  Connecting  │ ──error──┐
                 └──────┬───────┘          │
   accept()  ──────────>│                  v
                 ┌──────v───────┐    ┌───────────┐
                 │ Established  │──> │  Closing  │ ──> Closed(reason)
                 └──────┬───────┘    └───────────┘
                        │ shutdown_write()   ^
                 ┌──────v───────┐            │
                 │ ShutdownWrite│────────────┘
                 └──────────────┘
```

`on_close(ConnId, CloseReason)` fires **exactly once** for every connection
that reached `Established` (and for failed `Connecting` attempts, with the
error reason). `CloseReason` distinguishes: peer FIN, local close, framing
error, idle timeout, write-queue overflow, error.

---

## 14. Protocol and framing

The application owns its wire format. AffiniX supplies the read loop and calls
a user-supplied framer that is **inlined** into it — no virtual calls, no
`std::function`.

### 14.1 Concepts

```cpp
template <class M>
struct ParseResult {                // one of:
    enum class Kind { Message, NeedMore, Error };
    Kind          kind;
    M             message;          // Kind::Message
    std::size_t   consumed;         // Kind::Message: bytes to retire
    std::size_t   need;             // Kind::NeedMore: at least this many more
    Error         error;            // Kind::Error
};

// General form: covers TLV, varint, line-delimited, self-describing formats.
template <class P>
concept Protocol = requires (const P& p, ByteSpan in) {
    typename P::Message;
    { p.parse(in) } -> std::same_as<ParseResult<typename P::Message>>;
};

// Convenience form for the common case: a fixed-size header states the body
// length. Adapted to Protocol by FixedHeaderFramer<P>.
template <class P>
concept FixedHeaderProtocol = requires (const P& p, const typename P::Header& h) {
    typename P::Header;
    { P::kHeaderSize }  -> std::convertible_to<std::size_t>;
    { p.validate(h) }   -> std::same_as<Result<void>>;
    { p.body_size(h) }  -> std::same_as<Result<std::size_t>>;
};
```

`FixedHeaderProtocol` is implemented *in terms of* `Protocol`, not beside it,
so there is exactly one read path to optimise and debug.

### 14.2 Defining a protocol

```cpp
struct MyWire {
    struct Header {
        std::uint32_t magic;
        std::uint16_t version;
        std::uint16_t type;
        std::uint32_t len;      // body length, little endian
    };
    static_assert(sizeof(Header) == 12);

    static constexpr std::size_t kHeaderSize = sizeof(Header);
    static constexpr std::uint32_t kMagic    = 0xAF11'0001;
    static constexpr std::size_t kMaxBody    = 1u << 20;

    using Message = FrameView<Header>;   // { const Header& hdr; ByteSpan body; }

    Result<void> validate(const Header& h) const {
        if (afx::le32(h.magic) != kMagic)  return Err::BadMagic;
        if (afx::le16(h.version) > 2)      return Err::UnsupportedVersion;
        if (afx::le32(h.len) > kMaxBody)   return Err::FrameTooLarge;
        return {};
    }
    Result<std::size_t> body_size(const Header& h) const {
        return afx::le32(h.len);
    }
};
```

Deliberate choices:

- **Endianness is the application's.** AffiniX provides `le16/le32/be16/be32`
  helpers and imposes nothing.
- **The framework never allocates for a frame.** `Message` is a *view* into the
  connection's read buffer, valid only for the duration of the callback.
  Retaining it is explicit: `msg.retain()` yields a refcounted `BufferSlice`.
  Making this loud prevents the classic dangling-span bug.
- **A `kMaxBody` bound is mandatory** in `validate`; an unbounded length field
  is a trivial memory-exhaustion DoS (§25).
- Header structs are *not* packed by the framework. If the wire layout has no
  padding, the user asserts that with `static_assert`, or parses field by field.
  Reinterpreting a possibly-misaligned buffer as a packed struct is UB-adjacent
  and left as an explicit user decision.

### 14.3 Batched dispatch

When one `recv` yields forty frames, forty separate callbacks cost measurably
more than one:

```cpp
.on_messages = [](ConnId id, MessageBatch<MyWire> batch) {
    for (auto&& m : batch) route(m);
    // amortise here: one flush, one lock, one commit
}
```

`on_message` (singular) is sugar over `on_messages`.

### 14.4 Optional codec chain

```
raw bytes -> [TLS] -> [decompress] -> Framer -> [decrypt/authenticate] -> handler
```

Each stage is a type satisfying `Codec`, composed at compile time. Absent
stages cost nothing. Off by default.

---

## 15. TCP server and client

### 15.1 Server

```cpp
struct ServerConfig {
    SockAddr     bind{"0.0.0.0", 0};
    int          backlog        = 1024;
    bool         reuse_port     = true;      // one listener per EM shard
    bool         reuse_addr     = true;
    bool         defer_accept   = false;
    std::size_t  max_connections = 64 * 1024;
    SocketOptions sock{};                    // nodelay, quickack, buffers, keepalive
    FlowControl  flow{};                     // §16
    Duration     idle_read_timeout{};        // 0 = disabled
    Duration     idle_write_timeout{};
    std::size_t  accepts_per_iteration = 32; // bounded, as everything is
};

template <Protocol P> struct Handlers {
    /* required */ OnMessages<P> on_messages;
    /* optional */ OnOpen       on_open;       // (ConnId, Peer)
    /* optional */ OnClose      on_close;      // (ConnId, CloseReason)
    /* optional */ OnError      on_error;      // (ConnId, Error)
    /* optional */ OnWritable   on_writable;   // backpressure relieved
};

auto* srv = em.make_server<MyWire>({.bind = {"0.0.0.0", 9000}}, Handlers<MyWire>{
    .on_messages = [](ConnId id, MessageBatch<MyWire> b) { ... },
    .on_open     = [](ConnId id, Peer p)                 { ... },
    .on_close    = [](ConnId id, CloseReason r)          { ... },
}).value();
```

Any number of servers may coexist on one EM, each with a different `Protocol`.
With `reuse_port`, every EM opens its own listening socket on the same port and
the kernel distributes accepts — no single accept thread, no funnel.

### 15.2 Client

```cpp
struct ClientConfig {
    Endpoint   target;                 // host:port; resolved asynchronously
    Duration   connect_timeout = 5s;
    Backoff    reconnect{.initial = 100ms, .max = 30s, .jitter = 0.2};
    bool       auto_reconnect  = true;
    bool       happy_eyeballs  = true; // parallel A/AAAA attempts
    std::optional<SockAddr> bind_local;
    SocketOptions sock{};
    FlowControl   flow{};
};

auto* cli = em.make_client<MyWire>({.target = {"feed.example", 443}}, Handlers<MyWire>{
    .on_messages = ...,
    .on_open     = [](ConnId id, Peer) { subscribe(id); },
}).value();
cli->on_state_change([](ClientState s) { metrics.link_state(s); });
```

Reconnection with jittered exponential backoff, connection-state observability
and asynchronous name resolution are included because every real client needs
them and hand-rolling them on top is where users introduce loop-blocking
`getaddrinfo` calls.

---

## 16. Backpressure and flow control

Unbounded write queues are the most common way an event-driven server dies.
Flow control is therefore explicit and mandatory to configure (with sane
defaults).

```cpp
struct FlowControl {
    std::size_t write_high_watermark = 1u << 20;
    std::size_t write_low_watermark  = 256u << 10;
    OverflowPolicy on_overflow = OverflowPolicy::Disconnect;
    //   Disconnect | DropNewest | DropOldest | StopReading
    bool auto_pause_reads = true;  // stop reading when the peer is too slow
};

enum class SendResult { Sent, Queued, Backpressured, Dropped, Closed };
```

- `send()` returns a `SendResult`; ignoring it is a warning
  (`[[nodiscard]]`).
- Crossing the high watermark pauses reads (if configured) and stops calling
  `on_messages`; crossing back below the low watermark fires `on_writable` and
  resumes.
- `StopReading` propagates backpressure to the peer instead of buffering, which
  is the correct default for proxies.

---

## 17. Coroutine layer

Opt-in, built strictly on top of the callback core, and never required.

```cpp
afx::CoroTask<void> session(ConnRef c) {
    while (auto msg = co_await c.recv()) {          // resumes on the same EM
        co_await c.send(build_reply(*msg));
    }
    co_return;
}

afx::CoroTask<void> poller(EventManager& em) {
    for (;;) {
        co_await em.sleep(1s);
        auto [a, b] = co_await afx::coro::when_all(fetch_a(), fetch_b());
        publish(a, b);
    }
}
```

Guarantees and implementation notes:

- **Every awaiter resumes on the EM it was suspended on.** No exceptions.
- The task type is `afx::CoroTask<T>` (`afx::Task` was already taken by the
  mailbox work-item closure — see §25 of the implementation plan). It is
  lazy, uses symmetric transfer at `final_suspend` (no stack
  growth in a loop), and is `[[nodiscard]]`.
- Frames are allocated from the EM's arena via
  `operator new(std::size_t, EventManager&)`, so a coroutine-per-connection
  design does not hit the general allocator.
- Cancellation is a `StopToken` in the promise, propagated into in-flight
  awaiters; `co_await with_timeout(5s, op())` is built from it.
- Because the core is callbacks, a handler measured to be too slow can drop to
  the callback form without leaving the framework.

---

## 18. Error model

```cpp
struct Error {                       // 4 bytes
    std::uint16_t code;
    ErrorCategory category;          // Sys, Net, Frame, Config, Itc, Internal
};
template <class T> using Result = /* std::expected in C++23, own type in C++20 */;
```

- No exceptions on the data path; no `errno` leaking into user code.
- Exceptions are permitted in setup/configuration paths only.
- A user callback that throws is caught at the loop boundary and handled per
  `CallbackErrorPolicy`: `RouteToHandler` (default), `CloseConnection`, or
  `Terminate` (fail-fast shops).
- The loop never dies silently: an unhandled internal error increments a
  counter, logs once, and — if unrecoverable — stops the EM with a reason
  observable from `Runtime`.

---

## 19. Runtime, affinity and NUMA

`EventManager` is affinity-agnostic. `Runtime` places it.

```cpp
struct Topology {
    static Topology detect();        // sockets, physical cores, SMT siblings, NUMA
    std::span<const Core> cores() const;
    int numa_node_of(int core) const;
    std::optional<int> numa_node_of_nic(std::string_view ifname) const;
};

enum class Placement { OnePerPhysicalCore, OnePerLogicalCore, Explicit, None };

struct ThreadConfig {
    CoreSet    cores;
    Placement  placement = Placement::OnePerPhysicalCore;
    SchedPolicy sched    = SchedPolicy::Other{};   // or Fifo{prio}, Rr{prio}
    NumaPolicy numa      = NumaPolicy::LocalAlloc;
    EventManagerConfig em{};
};

afx::Runtime rt(Topology::detect());

auto io = rt.spawn_group("io", 4, ThreadConfig{
    .cores = CoreSet::range(2, 6),
    .sched = SchedPolicy::Fifo{50},
    .em    = {.wait = WaitStrategy::SpinThenBlock, .spin_budget = 100us},
});
auto workers = rt.spawn_group("worker", 2, ThreadConfig{
    .cores = CoreSet::of({8, 9}),
    .em    = {.wait = WaitStrategy::Block},
});

rt.start();
rt.join();
```

Provided behaviours:

- `OnePerPhysicalCore` skips SMT siblings — usually the right default for
  spinning loops, since two spinners on one physical core halve each other.
- **NIC locality check:** the NUMA node of the relevant interface is read from
  `/sys/class/net/<if>/device/numa_node`, and a warning is logged when an EM
  polls sockets on a remote node. This is sometimes a 2× effect and is free to
  detect.
- Optional `SO_INCOMING_CPU` / `SO_ATTACH_REUSEPORT_CBPF` to align kernel RX
  steering with the shard layout.
- Affinity failures (insufficient privilege, core not in the cpuset) are
  reported as errors, not silently ignored — with a `Placement::None` mode for
  restricted containers.
- The actual resulting placement is logged once at startup, always.

---

## 20. Shutdown and signals

```cpp
rt.on_signal({SIGINT, SIGTERM}, [&] { rt.shutdown(30s); });
```

- Signals are handled with `signalfd` (Linux) or a self-pipe fed by plain
  `sigaction` handlers (macOS/BSD) on one designated thread — never in an
  async signal handler. (The design said `EVFILT_SIGNAL`; the shipped signal
  thread isn't an EM, so it owns no kqueue — the self-pipe keeps the same
  "signal byte becomes loop work" shape with no backend dependency.)
- `Runtime::shutdown(timeout)` runs a defined drain sequence:
  1. stop accepting (close listeners, so the kernel refuses new connections);
  2. notify shards via `on_draining`;
  3. let in-flight requests finish, up to `timeout`;
  4. `shutdown_write` on remaining connections;
  5. hard close, stop the loops, join threads.
- `EventManager::stop()` is thread-safe and idempotent; destruction runs
  close handlers for everything still open, in a defined order.

---

## 21. Observability

Per-EM, lock-free, no atomics on the hot path (thread-local counters read by a
collector via mailbox).

**Counters:** loop iterations, idle iterations, spins, blocks, wakeups,
accepts, connections open/closed by reason, bytes and messages in/out, frame
errors, write-queue high-watermark hits, drops, mailbox pushes/pops/full,
timers armed/fired/cancelled, timer coalescings, groups cancelled,
`deadline_expired_before_start`, `deadline_expired_in_flight`, allocation
counts (debug).

**Histograms (HDR):** loop iteration duration, per-stage duration, timer
lateness, mailbox queue latency, `recv`→`on_messages` latency, write queue
depth, and — where timestamping is available (§9.4) — NIC→kernel,
kernel→dequeue and dequeue→handler intervals, plus deadline headroom remaining
at completion (the metric that says whether budgets are realistic). Per-stage
timing is gated by `EventManagerConfig::profile_stages` — the extra clock
reads stay off the hot path unless profiling is explicitly on.

**Served by AffiniX itself** (M12): a dedicated `Block`-mode EM runs a
minimal HTTP/1.1 subset — `GET /healthz /version /stats /metrics
/conns /placement /config /flight` plus `POST /admin/stall_threshold`,
`/admin/chaos`, `/admin/log_level`. Cross-shard state is collected through
shard mailboxes with a gather deadline; `/metrics` is Prometheus text;
`/flight` emits the framed per-shard dump `tools/afx-flight` merges into a
TSC-ordered timeline.

**Derived, and the metric to watch:** `idle_ratio` per EM. It is the signal for
whether to add shards, and it must exist from day one rather than be inferred
later from CPU graphs.

**Stall detector (debug/opt-in):** if a single callback exceeds
`stall_threshold`, log the handler identity, the stage, and the duration. Most
"the framework is slow" reports are a user callback blocking the loop; make
that self-diagnosing.

**Logging:** asynchronous and per-thread by construction — the producer pushes
a binary record into an SPSC ring, formatting happens on a consumer thread.
Anything synchronous in a spinning loop is a latency bug. A `LogSink` interface
allows delegation to the application's existing logger.

### 21.1 Flight recorder

Counters tell you *that* something went wrong; the flight recorder tells you
*what happened in the 200 µs before it*. Each EM owns a fixed-size ring of
fixed-size records, written unconditionally on the hot path:

```cpp
struct FlightRecord {            // 32 bytes, cache-line aligned in pairs
    std::uint64_t tsc;
    EventKind     kind;          // Accept, Recv, Frame, Send, TimerFire,
                                 // StateChange, ItcPost, ItcRun, Wakeup,
                                 // Backpressure, Drop, DeadlineExpired, User
    std::uint32_t handle;        // ConnId.idx / TimerId.slot / mailbox id
    std::uint32_t a, b;          // kind-specific: bytes, error, state, depth
    TraceId::Short trace;        // 8 bytes, correlates with §7.4
};

em.recorder().record(EventKind::Frame, id.idx, len, type);   // ~5 ns
```

- **Always on in release builds** (`AFX_FLIGHT_RECORDER=ON` by default). The
  cost is a TSC read, a masked increment and a 32-byte store into a hot
  cache line; anything that must be disabled to be affordable will be disabled
  when it is needed most.
- **Dumped** on `SIGSEGV`/`SIGABRT` (via a pre-registered handler that only
  touches the ring and `write(2)`), on a stall-detector trip, on a close with
  an error reason, and on demand from the admin endpoint. Output is binary,
  decoded by `tools/afx-flight` into a per-EM timeline.
- **Correlated across shards** by `tsc` plus the trace id, so a request that
  crossed three EMs can be reassembled — which is the whole point, and is only
  possible because context propagation (§7.4) carries the id.
- Users can add their own records with `EventKind::User` and two 32-bit fields.

Why core rather than an extension: the useful records are emitted from exactly
the same call sites as the stats counters, deep inside the loop, the connection
state machine and the mailbox. Adding it with §21 is a handful of lines per
site; adding it afterwards is a diff through every one of them, and the
temptation then is to instrument only the paths you already suspect — which are
never the ones that fail.

---

## 22. Testing strategy

### 22.1 Levels

| Level | Scope | Tool |
|---|---|---|
| Unit | rings, wheel, buffers, framing, handle table | doctest |
| Property/fuzz | framer against arbitrary byte splits | libFuzzer |
| Virtual time | all timer semantics | virtual clock |
| Integration | loopback client↔server, real backends | doctest |
| Simulation | multi-EM scenarios, deterministic | `SimBackend` |
| Benchmarks | micro and macro, with CI baselines | nanobench |
| Sanitizers | ASan, UBSan, TSan builds in CI | CI matrix |

### 22.2 Virtual clock

`EventManager` is parameterised on a `Clock` policy. Tests advance time
instantly:

```cpp
TestEnv env;                         // VirtualClock + SimBackend
auto& em = env.event_manager();
int fired = 0;
em.every(1s, [&](TimerCtx) { ++fired; });
env.advance(10s);
CHECK(fired == 10);                  // no sleeping, no flakiness
```

Without this, the timer suite is `sleep()`-based, slow and permanently flaky.
It must exist **before** the timer wheel is written, not after — which is why
it is [ADR-0001](adr/0001-pluggable-clock.md).

### 22.3 Deterministic simulation

Because an EM's only inputs are (mailbox, timers, I/O completions), all of them
can be supplied deterministically: `SimBackend` plus `VirtualClock` plus a
seeded scheduler runs an entire N-thread application on one thread, with
reproducible interleavings.

```cpp
SimRuntime sim(seed);
sim.add_shards(4, my_app_setup);
sim.inject(FaultProfile{.packet_loss = 0.01,
                        .partition_chance = 0.005,
                        .partition_window = {50ms, 500ms},
                        .partial_reads = true, .slow_peer = 0.05});
sim.run_for(60s);
CHECK(sim.invariants_held());
```

As built (M10): shards are `BasicEventManager<VirtualClock, SimBackend>`
interleaved on one thread by a seeded scheduler; each `SimBackend` attaches
to a shared `SimNet` fabric (`afx/sim/net.hpp`) that turns submits into
timestamped events — connects resolve listeners by bound port, sends become
ordered per-direction deliveries (TCP stays a FIFO stream), and
`FaultProfile` draws loss/reset/partition/partial-read/slow-peer per send.
`sim.post(shard, fn, delay)` is the deterministic analogue of cross-thread
mailbox delivery. Every delivered event is appended to `sim.trace()`; a
seed's trace is byte-identical across runs, which is what makes a failing
seed a complete, replayable repro of a concurrency bug.

This is the strongest available answer to "why should I trust a threading
framework", and it is only achievable if the backend and clock are seams
from the start.

### 22.4 Invariant tests worth writing early

- `assert(em.is_current())` in every non-thread-safe method (debug builds).
- **No-allocation steady state:** poison `malloc` after init, run the echo
  server, assert zero allocations.
- **No blocking in an EM thread:** debug-build assertion on framework
  primitives.
- **Public header hygiene:** a test that fails if `detail::` types appear in
  public signatures.
- **Unspecified-behaviour randomisation** (§26.4) enabled in all debug tests.

---

## 23. Repository layout and build

```
AffiniX/
  CMakeLists.txt            # C++20, presets, install/export targets
  CMakePresets.json         # gcc/clang × debug/release/asan/tsan
  include/afx/
    afx.hpp                 # umbrella header
    core/   event_manager.hpp  timer_wheel.hpp  handle_table.hpp
            io_buffer.hpp      pool.hpp         stats.hpp
            context.hpp        deadline.hpp     flight_recorder.hpp
    itc/    mailbox.hpp  channel.hpp  fan.hpp  barrier.hpp  sharded.hpp
    net/    tcp_server.hpp  tcp_client.hpp  connection.hpp
            protocol.hpp    codec.hpp       udp_socket.hpp  endpoint.hpp
    backend/ backend.hpp  epoll.hpp  uring.hpp  kqueue.hpp  sim.hpp
    sys/    affinity.hpp  topology.hpp  numa.hpp  clock.hpp
            result.hpp    error.hpp     inline_fn.hpp
            annotations.hpp           # AFX_REQUIRES / AFX_GUARDED_BY (§6.3)
    coro/   task.hpp  awaiters.hpp  when_all.hpp  scope.hpp
  src/                      # non-inline implementation (backends, syscalls)
  examples/                 # echo_server, chat, ping_pong_itc, sharded_proxy,
                            # timer_zoo, custom_framing, latency_probe,
                            # deadline_chain
  bench/                    # micro (ring, wheel, framing) + macro (echo, rpc)
  test/                     # unit, sim, integration,
                            # affinity_negative/ (must-not-compile cases)
  docs/  DESIGN.md  adr/
  tools/  afx-flight (recorder decoder), afx-load (load generator),
          ci scripts, baseline comparison
```

- **Build:** CMake ≥ 3.24, `afx::afx` interface + compiled parts. Options:
  `AFX_WITH_URING`, `AFX_BACKEND_ONLY`, `AFX_WITH_TLS`, `AFX_BUILD_TESTS`,
  `AFX_BUILD_BENCH`, `AFX_SANITIZE`, `AFX_DEBUG_CHAOS` (§26.4),
  `AFX_FLIGHT_RECORDER` (default ON, §21.1), `AFX_THREAD_SAFETY_ANALYSIS`
  (Clang only, §6.3), `AFX_WITH_TIMESTAMPING` (§9.4).
- **Dependencies:** std first. `doctest` and `nanobench` for tests/benchmarks
  only. `liburing` via `FetchContent`, behind `AFX_WITH_URING`. `moodycamel`
  only if it beats the in-tree MPSC ring. `std::format` rather than `fmt`
  (GCC 13+/Clang 17+ have it). No dependency is exposed in a public header.
- **Compilers:** GCC 11+, Clang 14+ (dev box here: GCC 15.2, CMake 4.2,
  kernel 7.0 — io_uring fully available).
- **CI:** build matrix × sanitizers, unit + sim + integration, benchmark
  baselines with a regression threshold, clang-tidy, an ABI/header-hygiene
  check, a Clang `-Wthread-safety -Werror` job (§6.3), a must-not-compile suite
  for affinity violations, and an annotation-coverage script over public
  headers.

---

## 24. Performance targets

Targets, not promises; they exist so that regressions are detectable. Measured
on one modern x86-64 core, loopback or a 25 GbE NIC.

| Path | Target |
|---|---|
| ITC post → execute (SPSC, spinning) | < 200 ns p50, < 1 µs p99 |
| ITC post → execute (blocking consumer) | < 10 µs p99 |
| Timer arm + cancel | < 50 ns |
| Timer lateness (1 ms tick, spinning) | < 100 µs p99 |
| Echo round trip, loopback, 64 B frame | < 10 µs p50 |
| Frames/sec/core, 128 B, batched | > 3 M |
| Loop iteration cost, idle, spinning | < 500 ns |
| Allocations in steady state | 0 |
| `Connection` footprint | < 512 B + buffers |

---

## 25. Security considerations

- **Length fields are hostile.** `validate()` must bound the body size; the
  framework refuses to allocate an unbounded frame and the examples all show a
  `kMaxBody` check.
- **Bounded everything:** connections per server, write queue bytes, mailbox
  depth, accepts per iteration, frame size, header size. A remote peer must not
  be able to make the process grow without limit.
- **Slowloris-style defences:** idle read/write timeouts, handshake timeout,
  and per-connection rate limiting hooks.
- **No `reinterpret_cast` of network bytes** into a struct by the framework;
  alignment and padding decisions are the application's, made explicitly.
- **Fuzzing the framer** is part of CI, because it is the only code path
  directly driven by untrusted input.
- **TLS** (when added) delegates to a vetted library via BIO pairs; AffiniX
  implements no cryptography.

---

## 26. Evolvability policy

The decisions in §7–§14 are cheap to change now and expensive later. This
section is the policy for keeping them changeable, and is as much a part of the
design as the APIs.

### 26.1 Restrictive-first defaults

| Decision | Shipped as | Later loosening |
|---|---|---|
| Cross-thread refs | `ConnId` only | add a pinned ref, opt-in |
| Blocking ITC | forbidden | add `blocking_call()` if ever justified |
| Frame lifetime | valid during callback only | add an always-retained mode |
| Callback throwing | policy, unspecified default | specify it |
| Public surface | minimum that compiles the examples | expose more |

### 26.2 Rule of two

No abstraction is frozen with one implementation behind it. Before the
`IoBackend` concept is final there must be **two genuinely different**
implementations (`SimBackend` from the start, plus an io_uring spike written
*before* the concept is fixed — even if the spike is thrown away). Before the
`Protocol` concept is final, the *awkward* case (line-delimited, no fixed
header) is implemented first; the fixed-header case then falls out.

### 26.3 Minimal public surface

`afx::detail` for everything not needed by the examples; opaque handles instead
of pointers; PIMPL for cold, non-template classes (`Runtime`, backends,
resolver); no public data members outside frozen `Config` aggregates (which
grow only by appending defaulted fields); `inline namespace v1` around the
public API.

### 26.4 Randomise what is unspecified

Users come to depend on incidental behaviour, and then it cannot be changed
even though it was never promised. In debug builds (`AFX_DEBUG_CHAOS=ON`)
AffiniX actively jitters everything it declared unspecified: deferred-task
order, dispatch order across independent connections, `ConnId`/`TimerId`
starting generations, poll batch sizes, and the split of a byte stream across
`on_messages` batches. This turns "we will break them one day" into "their test
fails today".

### 26.5 Decisions are measurable

A benchmark per hard decision, in CI, with recorded baselines and a regression
threshold. If the handle indirection costs 3 %, that is known in week two, not
after 40 000 lines. Plus the no-allocation and no-blocking invariant tests,
which lock in properties that are brutal to recover once violated in fifty
places.

### 26.6 Escape hatches

`em.watch(fd, …)` under the net layer, `conn.send()` under the codec chain,
callbacks under coroutines. When an abstraction is wrong for a user, they route
around it and stay — and what they bypass tells us where it is wrong.

### 26.7 Decision records

`docs/adr/` holds one record per significant decision, each with an explicit
**tripwire**: the observation that would justify revisiting it. A decision
without a falsification condition becomes dogma nobody dares touch.

The four genuine one-way doors:

1. [ADR-0001](adr/0001-pluggable-clock.md) — clock is a policy (virtual time)
2. [ADR-0002](adr/0002-proactor-canonical-model.md) — proactor is canonical
3. [ADR-0003](adr/0003-generation-checked-handles.md) — handles, not pointers
4. [ADR-0004](adr/0004-no-blocking-itc.md) — no blocking cross-EM calls

Two more decisions are "seam now or never", and are recorded for the same
reason:

5. [ADR-0008](adr/0008-context-and-deadline-propagation.md) — context and
   deadline propagation (§7.4)
6. [ADR-0009](adr/0009-compile-time-affinity-annotations.md) — compile-time
   affinity annotations (§6.3)

Also recorded: [ADR-0005](adr/0005-callback-core-coroutine-layer.md),
[ADR-0006](adr/0006-compile-time-protocol.md),
[ADR-0007](adr/0007-shared-nothing-no-work-stealing.md).

---

## 27. Roadmap

Ordered so that each step is verifiable when it lands.

| # | Milestone | Contents |
|---|---|---|
| 1 | Foundations | `Result`/`Error`, `Clock` (real + virtual), topology, `InlineFn`, `Context`/`Deadline`, annotation macros |
| 2 | Loop skeleton | `EventManager` stages, `defer`, epoll backend, `poll_once`, context plumbing |
| 3 | ITC | `Mailbox` + arm/block protocol, SPSC/MPSC channels, context propagation → `ping_pong_itc` |
| 4 | Timers | wheel + heap + `TimerGroup`, all semantics against virtual time |
| 5 | Placement | `Runtime`, affinity, NUMA, placement logging |
| 6 | Networking | buffers/pools, `Acceptor`, `Connection`, `Protocol` → `echo_server` |
| 7 | Robustness | backpressure, close state machine, stats + histograms, flight recorder, stall detector |
| 8 | io_uring | second backend behind the same concept; timestamping; head-to-head benchmarks |
| 9 | Coroutines | `Task<T>`, awaiters, cancellation, `TaskScope` — core unchanged |
| 10 | Simulation | `SimBackend` + `SimRuntime` + fault injection |
| 11 | Breadth | UDP, async DNS, Unix sockets + fd passing, kqueue |
| 12 | Operability | admin/introspection endpoint, `afx-flight` cross-shard merge |
| 13 | Optional | TLS, file I/O, zero-copy, `WorkerPool`, hot restart, shm IPC |

Task-level breakdown, exit criteria per milestone, spike schedule and risk
register: [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md). A milestone 0
(project scaffolding, CI, hygiene scripts) precedes milestone 1 there, because
ADR-0009's guarantee depends on the enforcement existing before the first
header does.

Sequencing notes:

- Milestone 8 is late, but the io_uring **spike** happens during milestone 2
  (§26.2) — the spike informs the concept; the production backend comes later.
- `afx-load` lands in milestone 8, not 12: milestone 8's purpose is a
  defensible epoll-vs-io_uring comparison, which is only as trustworthy as the
  load generator producing it.
- The four seam-now additions land in milestones 1–8 deliberately: `Context`
  and the annotation macros in milestone 1 (they touch every signature written
  afterwards), `TimerGroup` with the wheel in milestone 4 (it fixes the node
  layout), the flight recorder alongside stats in milestone 7 (same call
  sites), and `Timestamps` in the `Completion` struct from milestone 2 even
  though only the io_uring and timestamping work in milestone 8 fills it in.
- Milestone 12 exists as its own step because operability tooling slips forever
  when it is a subtask of something else.

---

## 28. Extensions

Organised by the only axis that matters for sequencing: **does it need a design
seam now, or is it safely additive later?** An addition "needs a seam" when it
touches signatures, struct layouts or call sites that are spread across the
whole framework — such a thing is a paragraph today and a rewrite in a year.

### 28.1 Needs a seam now — promoted into the core

These were extensions in the first draft of this document and have been moved
into the design proper, because each one becomes a breaking change within weeks
of implementation starting.

| Addition | Where | Why it cannot wait |
|---|---|---|
| Context & deadline propagation | §7.4, ADR-0008 | Touches every timeout, ITC entry point and awaiter |
| Compile-time affinity annotations | §6.3, ADR-0009 | Partial coverage is worse than none; must be total from the first header |
| Flight recorder | §21.1 | Emitted from the same deep call sites as stats |
| Timestamps in `Completion` | §9.4 | A struct that would otherwise freeze without the field |
| `TimerGroup` | §10.1 | Fixes the wheel node layout |

### 28.2 Safely additive — do when there is a consumer

Ordered roughly by value per unit of effort.

- **Admin / introspection endpoint.** Per-EM stats, live histograms,
  connection table, actual placement, config dump, Prometheus text, flight
  recorder dump, and runtime toggles for the stall detector and chaos mode.
  Built *using* AffiniX on a `Block`-mode EM, so it dogfoods the framework.
  Highest value per line of anything here.
- **UDP** with `recvmmsg`/`sendmmsg`, multicast join/leave with source
  filtering — cheap given the backend seam, required for market data and
  discovery.
- **Async DNS.** A resolver on a small dedicated EM replying by mailbox.
  Blocking `getaddrinfo` in a loop thread is a production stall.
- **Load generator (`afx-load`).** Speaks the user's `Protocol`, open- and
  closed-loop modes, coordinated-omission-free latency accounting, HDR output.
  Needed to trust milestone 8's numbers, and it lets users reproduce published
  figures — which is how performance claims stay honest.
- **Client resilience primitives:** token-bucket rate limiter, circuit breaker,
  retry budget, hedged requests. Every user of a TCP client writes these, and
  usually writes them wrong. Pure logic over the timer wheel.
- **Structured async logging** (§21) and **idle/heartbeat management** wired to
  the wheel.
- **TLS** as an optional codec (BIO pairs driven by the reactor, never the
  library's own socket handling), plus kTLS offload where available.
- **Unix domain sockets and fd passing.**
- **Hot restart.** Pass listening fds (and optionally the shm ITC segment) to a
  new binary over a Unix socket and drain the old one: zero dropped connections
  on deploy. Builds directly on fd passing.
- **Structured concurrency scopes** (`TaskScope`) in the coroutine layer, so
  child tasks are guaranteed to complete or cancel before the scope exits.
  Without it, coroutine-per-connection code leaks orphaned tasks on close.
  Additive, but design it *with* cancellation in milestone 9, not after.
- **Pub/sub fan-out** with per-subscriber queues and per-subscriber overflow
  policy (drop-oldest for market data, disconnect for order flow). Must reuse
  §16's flow control rather than inventing a second backpressure mechanism.
- **Shared-memory IPC transport.** The Tier-2 ITC rings (§11.2) placed in a
  `shm` segment connect *processes*, not just threads, unlocking
  process-per-core deployment with fault isolation and independent restart.
  Keep the ring pointer-free (offsets only) now — a 20-minute decision that
  preserves the option.
- **`WorkerPool`** for CPU-bound offload — explicitly *not* part of the I/O
  loops, results returned by mailbox. This is where work stealing is
  acceptable, because those threads own no event loop.
- **File and disk I/O** via io_uring with a thread-pool fallback, so log
  writing and snapshotting never stall an EM.
- **Zero-copy / kernel-bypass path:** `MSG_ZEROCOPY`, `send_zc`, registered
  buffers, and a backend seam wide enough for AF_XDP or DPDK later.
- **`tpause`/`umwait` in the spin loop** — on capable CPUs, lower power and no
  starvation of the SMT sibling, with sub-microsecond wakeup. Small, localised,
  measurable.
- **Privilege drop and seccomp after init.** Bind privileged ports, set
  affinity and `SCHED_FIFO`, then drop capabilities and install a filter.
- **Config and hot reconfiguration** through `Barrier`.
- **Per-iteration `perf` counters** and a "why was this iteration slow" stage
  breakdown.
- **RPC skeleton / HTTP codec** as `L5` layers, strictly optional.

### 28.3 Defer the decision

- **Production record/replay.** Record all inputs (completions, mailbox
  messages, clock) and replay them offline through `SimBackend` — nearly free
  once simulation exists, since it is the same seam. Huge debugging payoff, but
  recording volume and PII handling are real problems. Wait for a demand.
- **Dynamic shard rebalancing / connection migration at accept time.** Only
  once §21's `idle_ratio` data shows hashing cannot fix the imbalance
  (ADR-0007's tripwire).
- **Distributed tracing propagation** (W3C `traceparent`) — trivial once §7.4
  exists, since it is the same context object; the open part is the exporter,
  which is environment-specific.
- **Serialisation integration** as codec layers for Cap'n Proto or FlatBuffers
  — adapters, never a new IDL.
- **Windows/IOCP** (§29, item 6).

### 28.4 Recommended against — recorded so it is not relitigated

- **Work stealing inside the I/O loops** — contradicts affinity (ADR-0007).
- **A universal actor abstraction** — obscures the cost model this framework
  exists to expose.
- **`std::execution` as the foundation** — revisit as an adapter after v1.
- **QUIC/HTTP3 implementation** — enormous, and it wants its own
  congestion-control and loop integration. Use an existing library over the
  UDP layer.
- **A custom IDL or codegen** — not this project's problem; ADR-0006 already
  lets users plug in existing ones.
- **Built-in service discovery / mesh integration** — environment-specific
  churn. Expose hooks, ship no policy.

---

## 29. Open questions

1. **MPSC ring:** hand-rolled Vyukov vs `moodycamel`. Decide by benchmark at
   milestone 3.
2. **Timer tick default:** 1 ms wheel granularity vs 256 µs. Depends on how
   much of the target workload needs sub-millisecond timeouts; a per-EM
   override exists either way.
3. **`Connection` read buffer strategy:** one buffer per connection (simple,
   memory-hungry at 100 k connections) vs a shared per-EM buffer with promotion
   on partial frames (complex, far cheaper). Suggest starting with per-
   connection and measuring at milestone 7.
4. **io_uring buffer rings** (`IORING_REGISTER_PBUF_RING`) change who owns the
   read buffer. The `IoBackend` concept must be checked against this during the
   milestone-2 spike, or it will not fit later.
5. **Coroutine frame allocation** from the EM arena requires care with
   `operator new` overload resolution and with frames outliving the arena.
   Needs a prototype before milestone 9 is committed.
6. **Windows (IOCP).** Natively a proactor, so the model fits — but the
   sockets layer and affinity code would need real work. Out of scope for v1;
   the backend seam should not actively preclude it.
7. **`Context` size and cost.** 40-odd bytes copied per hop is fine for
   request/response but may be measurable on a 3 M msg/s channel. Options if
   benchmarks say so: pass by pointer into an EM-owned context slab (handle
   again), or allow `Context` to be omitted for channels declared
   context-free. Decide at milestone 3 with a benchmark, not by taste.
8. **Flight recorder and TSC.** Cross-core `rdtsc` comparison requires
   invariant TSC and, strictly, `CLOCK_MONOTONIC_RAW` calibration per core to
   merge timelines. Needs a calibration step at startup and a documented
   accuracy claim; the alternative (a `clock_gettime` per record) is too
   expensive for an always-on recorder.
9. **Annotation coverage enforcement.** Whether the grep-based
   annotation-coverage script is sufficient, or whether a clang-tidy check /
   AST matcher is warranted. Start with grep; revisit if it produces false
   confidence.
10. **Deadline semantics across hosts.** Absolute deadlines are only
    transferable between processes with synchronised clocks; across a network
    hop a *remaining duration* must be sent instead. Affects the RPC layer
    (`L5`), not the core, but the wire representation should be decided before
    the first RPC user exists.

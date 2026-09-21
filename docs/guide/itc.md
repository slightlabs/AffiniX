# Inter-thread communication

Shared-nothing means cross-thread interaction is always an explicit message
(ADR-0004: no blocking cross-EM calls — two event loops can never deadlock
on each other). The ITC layer provides four tools of increasing structure:

| Tool | Shape | When |
|---|---|---|
| `Mailbox` | untyped task queue | general "run this on that EM" |
| `channel<T>` | typed SPSC/MPSC ring | high-rate value streams |
| `Fan<T>` | dispatch over mailbox sets | shard routing: round-robin / hash / broadcast |
| `Barrier`, `Sharded<T>` | coordination, per-shard state | reconfiguration, per-EM locals |

Everything is bounded: rings have fixed capacity, posts can fail, and a
dead EM reports `PostResult::Closed` rather than dangling.

## Mailbox

Every EM has one mailbox; `em.mailbox()` returns a copyable `Mailbox`
handle — share it freely across threads (it's a `shared_ptr` to the impl).

```cpp
Mailbox mb = em.mailbox();            // or rt.mailboxes()[i], g.mailboxes()

PostResult r = mb.post([] {
    // runs on the owning EM's thread, stage 1
});
// r: Ok | Full | Closed
```

- `post(fn)` — the ambient `Context` (trace id, deadline, stop token) is
  captured automatically; expired work is shed at drain time.
- `post_with_ctx(fn, ctx)` — explicit context override.
- `post_batch(span<Task>)` — push many; returns the count accepted.
- `size_approx()` — the ring's *capacity* (depth is EM-side).

### `post_and_reply` — request/response without blocking

```cpp
mb.post_and_reply(
    [] { return compute_answer(); },   // runs on mb's EM
    home,                              // reply lands here (a Mailbox)
    [](int answer) { use(answer); });  // runs on home's EM
```

The work item runs on the target EM and returns `R`; the reply hops back to
`reply_to` and `on_reply(R)` runs there. Both hops preserve the ambient
context — a deadline set on the caller's side follows the whole exchange.

### Overflow

`EventManagerConfig::mailbox_capacity` (default 4096, power of two) bounds
the ring; `mailbox_overflow` chooses the full-ring behaviour: `Fail`
(return `Full`), `SpinRetry` (retry until space or `Closed`), `Abort`
(process dies — for fail-fast shops). `stats().mailbox_full` counts
overflows; `stats().mailbox_pushes/pops` count traffic.

## Typed channels

`channel<T>` is the fast path for typed values: a bounded ring with batch
delivery and a single in-flight drain wakeup — producers that arrive while
a drain is queued just fill the ring.

```cpp
auto [tx, rx] = channel<Msg>(1024);             // SPSC by default
auto [tx, rx] = channel<Msg>(4096, ChannelKind::MPSC);

// Producer(s) — try_push is lock-free; returns false when full.
tx.try_push(Msg{...});
tx.try_push_bulk(std::span<const Msg>(batch));  // returns count moved

// Consumer — attach once; cb runs on the EM's thread per drain.
rx.attach(em, [](std::span<const Msg> batch) {
    for (auto& m : batch) consume(m);
});
```

- **SPSC**: one producer thread, one consumer. `drain_contiguous` hands the
  consumer contiguous spans (up to 1024 per call).
- **MPSC**: many producers; delivery in chunks of up to 64.
- One in-flight drain post per channel — the mailbox sees one task no
  matter how many producers pushed.
- `rx.poll()` drains without an EM (tests, non-EM consumers).

## Fan — routing over a mailbox set

```cpp
auto mbs = g.mailboxes();

// Round-robin
Fan<Req> rr(mbs, FanMode::RoundRobin);
rr.dispatch(Req{...}, [](Req r) { handle(r); });   // runs on the picked shard

// Hash-by-key: same key → same shard (shard-affine routing)
Fan<Req> by_key(mbs, HashBy<Req, std::uint64_t>{&Req::tenant});
by_key.dispatch(req, [](Req r) { handle(r); });

// Broadcast: every shard gets a copy
rr.broadcast(Req{...}, [](Req r) { handle(r); });
```

`dispatch` returns the `PostResult` of the chosen mailbox. Hash routing uses
`std::hash<K>` over the named member — keys must be hashable.

## Barrier — coordinated reconfiguration

```cpp
Barrier::arrive_all(mbs,
    [] { /* per_shard: runs on every shard's thread */ },
    [] { /* done: runs once, on the last shard to arrive */ });
```

Each shard runs `per_shard`, then exactly once `done` — the idiom for
"everyone flipped the config, now swap the pointer".

## `Sharded<T>` — per-EM state

The intended replacement for a mutex-guarded global: one `T` per shard,
constructed on its own thread, touched only by its own thread.

```cpp
Sharded<Stats> s(mbs, [](std::size_t shard_idx) {
    return Stats{/* per-shard init */};
});

// fn(T&) runs on shard i's thread
s.invoke_on(0, [](Stats& st) { st.tally++; });

// map every shard → R, fold with reduce, deliver once to done(R)
s.map_reduce(
    [](Stats& st) { return st.tally; },          // per-shard
    [](std::uint64_t acc, std::uint64_t r) { return acc + r; },
    [](std::uint64_t total) { report(total); }); // last shard to report
```

`map_reduce`'s `done` runs on whichever shard reports last; the accumulator
is a CAS-folded atomic — keep `R` trivially copyable.

## Wake machinery (notes)

- Mailbox drain happens in stage 1 of every iteration — posted work always
  wins over waiting on I/O.
- The arm/block protocol (DESIGN.md §8.1) guarantees no lost wakeups:
  producers check a `Blocked` flag under seq_cst and signal the backend
  (eventfd write) when set.
- On io_uring↔io_uring pairs, posts between EMs prefer `MSG_RING` — a
  ring-to-ring message with zero syscalls on the sender side; a failed
  send falls back to the eventfd path, so a wake is never dropped.

# Spike 0002: coroutine frame allocation from the EM arena (M9-01)

**Question (open question 5):** can coroutine frames be arena-allocated
soundly? What happens when a frame outlives the arena?

## Constraint recap

DESIGN.md §17 wants `promise_type::operator new(std::size_t, EventManager&)`
so coroutine-per-connection never hits the general allocator. The EM arena
(`core/arena.hpp`) is a bump region: no per-object free. Naively carving
frames out of it is **unsound**: a server under connection churn leaks one
frame per session forever, because a bump allocator cannot reclaim an
individual frame when its coroutine ends.

## Answers

### 1. Yes — via a size-bucketed frame cache, never raw bump

A coroutine's frame size is fixed per promise type + capture set; in
practice a server runs a handful of distinct frame sizes. The promise's
`operator new(size, alloc)` routes through `FrameCache`, a per-EM array of
free lists bucketed by size class (power-of-two buckets, 64 B granularity):

- `operator new` pops a cached block of the right class; on miss it bumps
  one 4 KiB chunk from the arena and slices it into blocks.
- `operator delete` pushes the block back onto its class's list.
- Arena exhaustion falls back to `::operator new` (counted on `Stats` —
  `coro_heap_frames`), exactly the Pool contract: visible, never silent.

Arena footprint then equals the *peak live frame working set*, not the
cumulative number of coroutines — churn recycles blocks. This is the only
form of arena allocation considered sound; a pure bump carve-out is
rejected.

### 2. A frame that outlives its arena is a contract violation, made visible

Frames are freed by `coroutine_handle::destroy()`. The arena dies with its
EM, so the soundness condition is: **no live frame may outlive the owning
EM**. Enforced structurally, not by hope:

- A `Task` only ever runs via `em.spawn(t)` or `co_await` inside another
  spawned task — both paths register the frame's home EM.
- `BasicEventManager` keeps `coro_live_frames` (debug: an atomic-ish debug
  counter on the EM). Its destructor requires the count to be zero: a task
  abandoned across EM teardown `terminate`s in debug builds and is a
  documented contract violation in release (same class of rule as
  §13's deferred reclamation).
- `Task` is non-copyable; its destructor runs `destroy()` on the owning
  EM's thread. Destroying a `Task` off-thread posts the destroy to the EM
  mailbox — frames never cross threads, matching the shared-nothing rule.
- A `Task` created but never spawned/awaited owns a heap frame allocated
  without an EM (global `operator new`) — safe to destroy anywhere.

### Alternatives rejected

- **Raw bump allocation** — leaks per session; rejected above.
- **Heap frames only** — correct but forfeits the locality/TLB win the
  arena exists for; kept as the fallback path, not the default.
- **Reference-counted arena sub-regions** — needless machinery; the
  freelist achieves reuse with O(1) operations and no ownership tracking.

## Verdict

**Adopt**: `operator new(size, FrameAlloc&)` where the coroutine's first
parameter carries the EM (the `ConnRef`/`EM&` first-argument convention in
§17). FrameCache sits inside `BasicEventManager`, backed by `arena_`,
fallback heap, `coro_heap_frames` counter on `Stats`. Contract: every
spawned task is joined/cancelled before its EM dies — asserted in debug.

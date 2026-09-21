# Memory and allocation

No `std::function`, no hidden heap churn (DESIGN.md §2.2, §12). Each
EventManager owns a bump `Arena` allocated once at construction; `Pool<T>`
sits on top for object recycling; coroutine frames come from the
`FrameCache`; callables use `InlineFn` with inline storage. Every fallback
to the heap is **counted and visible** — never silent.

## `Arena` — per-EM bump region

```cpp
Arena arena(8u << 20, numa::AllocOpts{
    .node = numa::current_node(),   // MPOL_BIND to this NUMA node
    .hugepages = true,              // MAP_HUGETLB → MADV_HUGEPAGE → plain
    .prefault = true,               // touch every page at init, not at peak
});

void* p = arena.alloc(n, align);   // bump; nullptr when exhausted
arena.used(); arena.remaining(); arena.owns(p); arena.hugepages();
```

EMs build theirs from `EventManagerConfig::memory`:

```cpp
cfg.memory.arena_bytes = 32u << 20;   // default 8 MiB; 0 disables the arena
cfg.memory.read_buffer_size = 64u << 10;  // per-connection read buf (64 KiB)
cfg.memory.numa = NumaPolicy::LocalAlloc; // Default | LocalAlloc | Interleave
cfg.memory.hugepages = true;
cfg.memory.prefault = true;           // default on — pay the fault cost at init
```

- `LocalAlloc` binds the arena to the node the EM thread currently runs on
  (`MPOL_BIND`); `Interleave` spreads pages across all nodes
  (`MPOL_INTERLEAVE`); `Default` leaves policy untouched.
- Binding happens **before** population — `numa::alloc` mbinds the mapping
  first, then prefaults, so pages charge to the right node.
- `arena_bytes == 0` creates an empty arena whose `alloc()` always returns
  `nullptr` — pools then fall back to heap (counted).
- NUMA support degrades gracefully: `numa::available()` false → plain
  mapping, never an error. No libnuma dependency — raw `getcpu`/`mbind`/
  `get_mempolicy` syscalls.

## `Pool<T>` — slab recycling

```cpp
Pool<Session> pool(&arena, /*objs_per_chunk=*/64);

Session* s = pool.construct(args...);  // nullptr only on real OOM
s->use();
pool.destroy(s);                        // ~T() + free-list push

pool.allocated();  // live objects
pool.capacity();   // total slots across all chunks
pool.heap_chunks();// chunks that fell back to operator new — watch this
```

Chunks carve from the arena first; on exhaustion they come from
`operator new` and `heap_chunks()` records it. The pool frees **memory** on
destruction — live objects are the owner's business (deferred reclamation,
§13), so `destroy()` everything you `construct()`.

## Coroutine frames — `FrameCache`

A `CoroTask` whose first parameter carries the EM (`session(ConnRef)`,
`worker(EventManager&)`) allocates its frame from the owning EM's
`FrameCache`: size-bucketed blocks carved from the arena. Recycling matters
— a bump arena alone would leak one frame per accepted session.

Exhaustion and oversized frames fall back to `operator new`, counted at
`em.coro_heap_frames()` and `stats().coro_heap_frames` — a steadily growing
counter means the arena is undersized or frames are capturing too much.

## `InlineFn<Sig, N>` — callable without `std::function`

```cpp
InlineFn<void(int)> f = [n = 5](int x) { return x + n; };   // inline storage
InlineFn<void(int)> g = [big = BigState{}](int) { /*...*/ }; // heap fallback
f.heap_allocated();  // true when the callable didn't fit
```

- `N` bytes of inline storage (default 48); callables that fit *and* are
  nothrow-movable stay inline — posted `Task`s and timer callbacks don't
  hit the heap.
- Oversized or throwing-move callables go to the heap; `heap_allocated()`
  makes the cost observable rather than surprising.
- Move-only, like the rest of the framework's task types.

## `IoBuffer` — the connection read buffer

`Connection` owns one `IoBuffer` (`memory.read_buffer_size`, default
64 KiB) that it fills on each read completion. Message bodies you receive
are spans **into this buffer** — valid only until the next read lands on
that connection.

Layout is `[ headroom | readable | writable ]`: `consume(n)` advances the
read cursor, `writable(at_least)` compacts or grows to guarantee tail room,
and `prepend(n)` reserves space *in front* of the readable region — so a
protocol can serialise a body first and stamp its header in front of it
without a copy. `compact()` slides unconsumed bytes back to the headroom
boundary; the buffer grows geometrically when a single frame exceeds it.

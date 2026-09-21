# Testing

The suite is a pyramid (M10, M13): cheap deterministic layers at the bottom,
real-socket integration at the top. 223 cases and counting.

## Layers

| Directory | Kind | What it proves |
|---|---|---|
| `test/unit/` | unit | Ring arithmetic, timer wheel, framer, channels, mailboxes, `Result`, `IoBuffer`, `InlineFn`, pools/arenas — against `VirtualClock`/`SimBackend` where time or I/O matter |
| `test/model/` | model | Rings and timer wheel against an exhaustive small-model checker |
| `test/sim/` | simulation | `SimRuntime` multi-shard scenarios: seeded interleavings, fault profiles, replay traces |
| `test/invariant/` | invariants | `close_exactly_once`, `no_alloc_steady_state`, `no_blocking_in_em` — properties, not cases |
| `test/fuzz/` | fuzz | `test_framer_fuzz` — random byte streams through the framing seam |
| `test/stress/` | stress | `test_lost_wakeup` — hammering the producer/blocked-protocol race |
| `test/integration/` | integration | Real sockets: echo, client/server, shutdown drain, UDP, Unix, DNS, admin |
| `test/affinity_negative/` | compile-fail | Clang thread-safety annotations must reject these; verified by `check_annotations.py` |

## Running

```sh
cmake --preset gcc-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug --output-on-failure
```

Presets worth knowing: `gcc-release`, `clang-debug`, `clang-release`,
`clang-tsa` (thread-safety analysis), `asan`, `ubsan`, `tsan`. For changes
under `include/afx/net` or `include/afx/core`, run the asan preset too.

Hygiene gates (they run in CI; cheap to run locally):

```sh
python3 tools/check_header_hygiene.py    # header self-containment/dep rules
python3 tools/check_annotations.py       # affinity annotations present+used
python3 tools/test_hygiene_scripts.py    # the checkers' own tests
```

## Writing tests

- **No sleeps.** `VirtualClock` + `SimBackend` (or `SimRuntime` for
  multi-shard) — drive time explicitly, feed bytes in, inspect `sent(fd)`.
  If a test sleeps, it's an integration test or it's wrong.
- **Determinism.** Same seed → same `sim.trace()`. Don't assert on
  incidental timing; assert on the trace and on `invariants_held()`.
- **Real sockets only in `test/integration/`.** Everything else runs
  kernel-free.

## Thread-affinity annotations

With Clang, the `clang-tsa` preset (`AFX_THREAD_SAFETY_ANALYSIS=ON`)
compiles `-Wthread-safety`-style checks that prove EM-affine objects can't
escape their thread — `test/affinity_negative/` holds the cases that must
*fail* to compile, driven by `tools/check_annotations.py`.

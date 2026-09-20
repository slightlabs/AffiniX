# Benchmark environment & methodology

Per `docs/IMPLEMENTATION_PLAN.md` §6: every committed baseline in
`bench/baselines/` is tied to a recorded environment. Numbers taken on a
different machine or configuration are *not* comparable — record a new
environment block, don't edit an old one.

## Recorded environment

| field | value |
|---|---|
| CPU | Intel Core i5-4300U @ 1.90 GHz (2C/4T, Haswell) |
| kernel | Linux 7.0.0-31-generic |
| governor | `schedutil` (800 MHz–2.9 GHz scaling) |
| NUMA | 1 node (`numa_balancing=0`) |
| hugepages | 2048 kB + 1 GiB pools configured (`/sys/kernel/mm/hugepages`) |
| compiler | g++ 15.2.0 (Ubuntu) |
| machine class | shared dev host — **not** an isolated benchmark machine |

This is a laptop-class dev host with frequency scaling on. Absolute numbers
are soft; treat baselines produced here as smoke-level references. The CI
benchmark job and any real comparison (e.g. the M8-10 epoll-vs-io_uring
bake-off) must run on a pinned host with `governor=performance`, IRQ affinity
set, and turbo state recorded — and those facts go in the JSON's `env` block.

## How to run

```sh
cmake -S . -B build-bench -GNinja -DCMAKE_BUILD_TYPE=Release -DAFX_BUILD_BENCH=ON
cmake --build build-bench

# Real measurement run (writes a committable baseline):
AFX_BENCH_JSON=bench/baselines/itc_pingpong.json ./build-bench/bench/bench_itc_pingpong

# CI smoke run (proves it builds and runs; numbers meaningless):
AFX_BENCH_SMOKE=1 ./build-bench/bench/bench_timer_ops

# M8-10 head-to-head (in-process server+load on loopback):
./build-bench/bench/bench_echo --server epoll --conns 8 --duration 4 \
    --outstanding 8 --json bench/baselines/echo_closed_epoll.json
./build-bench/bench/bench_echo --server uring ...   # same workload
./build-bench/bench/bench_echo --server raw   ...   # hand-written epoll loop

# Point afx-load at any live echo_server instead of the in-process driver:
./build-bench/tools/afx-load 127.0.0.1 9000 --conns 8 --duration 10 \
    --mode open --rate 120000 --shards 2 --json run.json
```

## Baseline format

`bench/baselines/<benchmark>.json` is nanobench's JSON render
(`AFX_BENCH_JSON` env var drives `bench_env.hpp::flush_json()`): a
`results` array of `{name, unit, batch, epochs, "median(elapsed)", ...}`
where `median(elapsed)` is seconds per operation.
For non-nanobench drivers (`bench_driver`, `tools/afx-load`) the file is a
flat object: `{"benchmark", "env": {...}, "metrics": {...}}`.

## Regression policy

- A benchmark regresses when its median moves **>10%** against the committed
  baseline on a comparable environment.
- Baselines update in the same commit as the change that moves them, with
  the environment block refreshed.
- Never commit baselines from a `schedutil`/powersave machine when the
  comparison being claimed (M8-10 bake-off) requires `performance`.

# Benchmarking

`bench/` holds the micro/macro benchmarks plus committed baselines; the
discipline lives in [`bench/ENVIRONMENT.md`](https://github.com/slightlabs/AffiniX/blob/main/bench/ENVIRONMENT.md)
— every baseline is tied to a recorded environment, and numbers from a
different machine or governor are *not* comparable.

## Running

```sh
cmake -S . -B build-bench -GNinja -DCMAKE_BUILD_TYPE=Release -DAFX_BUILD_BENCH=ON
cmake --build build-bench
```

| Benchmark | Measures |
|---|---|
| `bench_itc_pingpong` | mailbox post→run round-trip latency |
| `bench_channel_throughput` | typed channel push/drain rate |
| `bench_timer_ops` | timer wheel arm/fire/cancel ops |
| `bench_echo` | in-process server+load over loopback — closed/open loop, `--server epoll|uring|raw` |
| `bench_echo_coro` | same echo workload through `make_coro_server` |
| `bench_udp_batch` | batched datagram send/recv |
| `bench_admin_impact` | dataplane cost of a running admin endpoint |
| `bench_driver` | non-nanobench driver harness |

```sh
# Committable baseline (nanobench JSON → AFX_BENCH_JSON):
AFX_BENCH_JSON=bench/baselines/itc_pingpong.json \
    ./build-bench/bench/bench_itc_pingpong

# CI smoke run (builds and runs; numbers meaningless):
AFX_BENCH_SMOKE=1 ./build-bench/bench/bench_timer_ops

# M8-10-style head-to-head, same workload across backends:
./build-bench/bench/bench_echo --server epoll --conns 8 --duration 4 \
    --outstanding 8 --json bench/baselines/echo_closed_epoll.json
./build-bench/bench/bench_echo --server uring ...   # same flags
./build-bench/bench/bench_echo --server raw   ...   # hand-written epoll loop
```

## `afx-load` — external load generator

Point it at any live server (it's a real networked client, not a ctest):

```sh
./build-bench/tools/afx-load 127.0.0.1 9000 \
    --conns 8 --duration 10 --warmup 2 \
    --mode open --rate 120000 --shards 2 \
    --payload 32 --outstanding 8 \
    --backend auto --json run.json
```

`--mode closed` keeps `--outstanding` requests in flight per connection;
`--mode open` fires at a fixed `--rate` regardless of replies — the honest
way to find the saturation point. `--json` writes a flat
`{benchmark, env, metrics}` object including the log-scale latency
histogram buckets.

## Rules

- A benchmark regresses when its median moves **>10%** against the
  committed baseline *on a comparable environment*.
- Update baselines in the same commit as the change that moves them, and
  refresh the environment block.
- Never commit baselines from a `schedutil`/powersave host for comparisons
  that require `governor=performance` — record CPU, kernel, governor, NUMA,
  hugepages, compiler, and machine class in the JSON `env` block.

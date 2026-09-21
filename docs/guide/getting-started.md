# Getting started

AffiniX is a C++20 framework for building thread-affine, event-driven
applications. This page covers requirements, building, and a minimal working
server. The other guide pages go subsystem by subsystem.

## Requirements

| Requirement | Notes |
|---|---|
| C++20 compiler | GCC 11+ or Clang 14+. Clang additionally enables compile-time affinity checking (`-Wthread-safety`); on GCC the annotations expand to nothing. |
| CMake | 3.24+ (presets require `version: 6` support, i.e. CMake 3.25+ recommended) |
| Ninja | used by all presets |
| Linux | epoll backend is the baseline; io_uring is probed at runtime (kernel ≥ 5.18 for MSG_RING; SQPOLL needs `io_uring_setup` privilege) |
| macOS / BSD | kqueue backend (`AFX_HAVE_KQUEUE`); io_uring-specific features degrade to the readiness model |
| doctest | fetched automatically when `AFX_BUILD_TESTS=ON` |

There are no third-party runtime dependencies: the io_uring backend is
implemented directly on the syscall ABI — no liburing.

## Build

```sh
cmake --preset gcc-debug                  # or clang-debug / gcc-release / ...
cmake --build --preset gcc-debug -j"$(nproc)"
ctest --preset gcc-debug --output-on-failure -j"$(nproc)"
```

Available presets: `gcc-debug`, `gcc-release`, `clang-debug`,
`clang-release`, `clang-tsa` (thread-safety analysis), `asan`, `ubsan`,
`tsan`. Each builds the library, tests, examples, and tools into
`build/<preset>/`.

### CMake options

| Option | Default | Effect |
|---|---|---|
| `AFX_BUILD_TESTS` | ON | doctest suite under `test/` |
| `AFX_BUILD_BENCH` | OFF (ON in presets) | benchmarks under `bench/` |
| `AFX_BUILD_EXAMPLES` | ON | `examples/` binaries |
| `AFX_BUILD_TOOLS` | ON | `afx-load` load generator |
| `AFX_WITH_URING` | ON (Linux) | io_uring backend; define `AFX_WITH_URING=1` |
| `AFX_WITH_TIMESTAMPING` | OFF | SO_TIMESTAMPING receive stamps |
| `AFX_FLIGHT_RECORDER` | ON | always-on per-EM flight recorder |
| `AFX_DEBUG_CHAOS` | OFF | randomise unspecified ordering in debug builds |
| `AFX_THREAD_SAFETY_ANALYSIS` | OFF | Clang `-Wthread-safety` affinity checks |
| `AFX_SANITIZE` | — | `asan`, `ubsan`, or `tsan` |

To embed AffiniX in another CMake project, add the repository as a
subdirectory (or install it) and link `afx::afx`:

```cmake
add_subdirectory(AffiniX)
target_link_libraries(my_app PRIVATE afx::afx)
```

The public umbrella header is `#include "afx/afx.hpp"`; every layer is also
includable on its own (`afx/core/event_manager.hpp`, `afx/net/...`, ...).

## A first server

The essential shape — a framed echo server, one shard per physical core
(condensed from `examples/echo_server.cpp`):

```cpp
#include <array>
#include <csignal>
#include <cstring>
#include "afx/afx.hpp"

using namespace afx;

// Wire format: [magic:1][len:2 BE][type:1] + body.
struct __attribute__((packed)) EchoHeader {
    std::uint8_t magic;
    std::uint16_t len_be;
    std::uint8_t type;
};
static_assert(sizeof(EchoHeader) == 4);

struct EchoMsg {
    EchoHeader header;
    ByteSpan body;
};

struct EchoProto {
    using Header = EchoHeader;
    using Message = EchoMsg;
    static constexpr std::size_t kHeaderSize = sizeof(EchoHeader);

    Result<void> validate(const EchoHeader& h) const {
        if (h.magic != 0xA5)
            return make_error(ErrorCategory::Frame, Err::BadMagic);
        return {};
    }
    Result<std::size_t> body_size(const EchoHeader& h) const {
        return std::size_t(be16(h.len_be));   // bound this in real protocols!
    }
};

int main() {
    Topology topo = Topology::detect();
    std::size_t n = std::max<std::size_t>(1, topo.physical_cores().size());

    Runtime rt(std::move(topo));
    auto g = rt.spawn_group("echo", n, ThreadConfig{});

    g.each([](EventManager& em) {
        Handlers<EchoProto> h;
        h.on_messages = [&](ConnId id, std::span<const EchoMsg> batch) {
            auto* c = Connection<EchoProto, EventManager>::resolve(em, id);
            if (!c) return;
            for (auto& m : batch) {
                std::array<std::byte, sizeof(EchoHeader)> hdr;
                std::memcpy(hdr.data(), &m.header, sizeof(hdr));
                std::array<ByteSpan, 2> parts{ByteSpan(hdr.data(), hdr.size()),
                                              m.body};
                c->send_scatter(parts);   // header + body, one sendmsg
            }
        };
        ServerConfig cfg;
        cfg.bind = SockAddr::any(9000);
        cfg.reuse_port = true;          // every shard binds :9000
        (void)em.make_server<EchoProto>(cfg, std::move(h));
    });

    rt.on_signal({SIGINT, SIGTERM}, [&] { rt.shutdown(5s); });
    rt.start();
    rt.join();
}
```

Build it with the `gcc-debug` preset and run
`./build/gcc-debug/examples/echo_server`. To drive real traffic, use the
bundled load generator:
`./build/gcc-debug/tools/afx-load 127.0.0.1 9000 --conns 4 --duration 5`.

## The programming model in one paragraph

Each `EventManager` (EM) owns one thread and everything running on it:
connections, timers, coroutines, deferred work. Callbacks run on that thread
only — no locks needed for EM-local state. Cross-thread work is sent, never
shared: `Mailbox::post()` moves a closure onto the target EM, and handles
(`ConnId`, `TimerId`, `IoId`) are generation-checked so a stale reference
fails instead of corrupting a new object. All I/O is proactor-shaped: you
submit an operation, the backend completes it, you get the result. The whole
stack — TCP, UDP, Unix sockets, DNS, timers, coroutines, ITC — runs on the
same loop, and the same code runs on a fully deterministic simulator for
testing.

## Examples map

| Example | Shows |
|---|---|
| `echo_server` | Runtime + spawn_group + `make_server` + `FixedHeaderProtocol` + `send_scatter` |
| `custom_framing` | The general `Protocol` concept (line-delimited parse) |
| `itc_pingpong` | `Mailbox::post_and_reply` between shards |
| `timer_zoo` | `after` / `every` / `at`, `TimerGroup`, cancel-in-callback |
| `session_coro` | `make_coro_server`, `ConnRef` recv/send awaiters, `with_timeout` |
| `observable_server` | The admin endpoint (`admin::AdminServer`) alongside a dataplane |

Each example takes an optional port argument; see the header comment of each
file for the exact invocation.

## Where next

- [EventManager](event-manager.md) — the loop itself: lifecycle, stages,
  wait strategies, posting work, the raw-fd escape hatch
- [Protocols and framing](protocols.md) — define your own wire format
- [TCP servers and clients](tcp.md) — connections, flow control, reconnect
- [Runtime and placement](runtime.md) — sharding, CPU pinning, NUMA, signals
- [Inter-thread communication](itc.md) — mailboxes, channels, fan, sharded state
- [Coroutines](coroutines.md) — `CoroTask`, `ConnRef`, combinators
- [Observability](observability.md) — stats, metrics, flight recorder, admin endpoint
- [Simulation and testing](simulation.md) — virtual clock, deterministic net
- [Errors and results](errors.md) — `Result<T>`, `Error`, `AFX_TRY`

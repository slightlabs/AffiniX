# TCP servers and clients

Any number of TCP servers and clients coexist on one EventManager, each with
its own protocol. Everything is owned by the EM; user code sees generation-
checked `ConnId` handles and resolves them to `Connection*` only on the EM
thread.

```cpp
auto srv = em.make_server<MyProto>(ServerConfig{...}, std::move(handlers));
auto cli = em.make_client<MyProto>(ClientConfig{...}, std::move(handlers));
```

Both return `Result<T*>` — the object is EM-owned and lives until the EM
dies (there is no public delete; tear down by closing the EM).

## Handlers

`Handlers<P>` carries the callbacks. Only `on_messages` is required;
everything else is optional (`InlineFn`, assignable from any callable ≤ the
inline capacity — larger callables fall back to the heap):

```cpp
Handlers<MyProto> h;
h.proto       = MyProto{/* options */};              // protocol instance
h.on_messages = [](ConnId id, std::span<const Msg> batch) {};
h.on_open     = [](ConnId id, Peer peer) {};
h.on_close    = [](ConnId id, CloseReason r) {};
h.on_error    = [](ConnId id, Error e) {};
h.on_writable = [](ConnId id) {};   // write queue drained below low watermark
h.on_shutdown = [](ConnId id) {};   // §20 drain: last chance to write
h.on_fds      = [](ConnId id, std::span<const int> fds) {};  // SCM_RIGHTS
```

- `on_messages` gets a **batch** — up to 64 parsed messages per call. Message
  bodies are views into the read buffer: copy before the callback returns
  (see [Protocols](protocols.md)).
- `on_writable` fires once when a backpressured connection drains below its
  low watermark — resume producing there.
- `on_shutdown` fires once per connection when shutdown begins, before the
  drain wait — flush final responses there.
- `on_fds` receives SCM_RIGHTS descriptors on Unix connections; the handler
  **takes ownership** of every fd (without a handler the framework closes
  them — unclaimed rights never leak).

## Connections

Inside a callback, resolve the `ConnId` to the `Connection`:

```cpp
auto* c = Connection<MyProto, EventManager>::resolve(em, id);
if (!c) return;   // stale handle — the conn is gone
```

| Member | Meaning |
|---|---|
| `id()` | Its `ConnId` — `{idx, gen}`. |
| `state()` | `Connecting / Established / ShutdownWrite / Closing / Closed`. |
| `peer()` | `Peer{SockAddr}` — remote address. |
| `fd()` | The raw socket (read-only use). |
| `open_ns()` | Nanoseconds since Established. |
| `queued_write_bytes()` | Bytes sitting in the write queue. |
| `send(bytes)` | Queue bytes → `SendResult` (see below). |
| `send_scatter(parts)` | `std::span<const ByteSpan>` gather — header+body in one sendv. |
| `send_fds(payload, fds)` | SCM_RIGHTS on Unix conns — see below. |
| `shutdown_write()` | Close-after-drain: FIN follows queued bytes. |
| `close(reason)` | Begin teardown; `on_close` fires after. |
| `handle()` | `Connection::Handle` — a cross-thread handle. |

`resolve` returning `nullptr` for a dead connection is the design: stale
handles fail closed, never use-after-free.

### Sending

`send()` and `send_scatter()` queue into the connection's write buffer; the
loop emits one sendv per dirty connection in stage 6 (write coalescing —
many small `send()` calls in one callback become one syscall).

```cpp
SendResult r = c->send(ByteSpan(p, n));
// Queued        — accepted (with DropOldest, the oldest bytes were evicted to fit)
// Backpressured — StopReading policy: queued, reads paused until drain
// Dropped       — DropNewest policy refused these bytes
// Closed        — not Established, or Disconnect policy just fired
```

### The cross-thread `Handle`

`c->handle()` returns `{Mailbox, ConnId, EM*}` — a small value you can share
with other threads. `handle.send(bytes)` copies the bytes and posts a send
task to the owning EM; `handle.close()` posts a close. Both return
`PostResult` (`Full`/`Closed` possible). This is the only supported way to
write to a connection from another thread.

## Servers

```cpp
struct ServerConfig {
    SockAddr bind = SockAddr::any(0);  // any/port, loopback, parse, unix_domain
    int backlog = 1024;
    bool reuse_port = true;            // per-shard listeners on one port
    bool reuse_addr = true;
    bool defer_accept = false;
    std::size_t max_connections = 64 * 1024;
    SocketOptions sock{};              // nodelay, rcvbuf, keepalive, ...
    FlowControl flow{};                // backpressure — below
    Duration idle_read_timeout{};      // 0 = off
    Duration idle_write_timeout{};     // 0 = off
    std::size_t accepts_per_iteration = 32;
    bool fd_passing = false;           // SCM_RIGHTS harvest (AF_UNIX only)
};
```

```cpp
auto srv = em.make_server<MyProto>(
    ServerConfig{.bind = SockAddr::any(9000), .idle_read_timeout = 30s},
    std::move(h));
if (!srv) { /* bind failed — srv.error() */ }
TcpServer<MyProto, EventManager>* s = *srv;
s->bound_addr();          // actual bound address (port 0 → assigned port)
s->connection_count();
s->conn(id);              // Connection* by ConnId
s->close_conn(id, CloseReason::LocalClose);
```

- `reuse_port` is the shared-nothing scale-out: every shard binds the same
  port and the kernel hashes new connections across listeners. A connection
  lives its whole life on the shard that accepted it.
- `idle_read_timeout` / `idle_write_timeout` close silent connections with
  `CloseReason::IdleTimeout`.
- `accepts_per_iteration` bounds accept bursts per loop pass.
- Over `max_connections`, new fds are closed immediately.
- Unix listeners own their path: a stale socket file is unlinked before
  bind and again at close.

## Flow control and backpressure

```cpp
struct FlowControl {
    std::size_t write_high_watermark = 1u << 20;   // 1 MiB
    std::size_t write_low_watermark  = 256u << 10; // 256 KiB
    WriteOverflow on_overflow = WriteOverflow::Disconnect;
    bool auto_pause_reads = true;
};
```

`send()` when the queue is above the high watermark applies `on_overflow`:

| Policy | Effect |
|---|---|
| `Disconnect` | Close the connection (`CloseReason::WriteOverflow`). Default — a peer that can't drain is usually a peer you can't help. |
| `DropNewest` | Refuse the bytes being sent now (`SendResult::Dropped`). |
| `DropOldest` | Evict the oldest queued bytes. |
| `StopReading` | Leave reads paused until the queue drains below low watermark (proxy behaviour). |

With `auto_pause_reads` the connection stops reading while above the high
watermark regardless of policy. `stats().write_hwm_hits` and
`stats().write_drops` count both events; `on_writable` is the resume signal.

## Clients

```cpp
struct ClientConfig {
    Endpoint target{};                 // {"host", port} — name or numeric IP
    Duration connect_timeout = 5s;
    Backoff reconnect{};               // {initial=100ms, max=30s, jitter=0.2}
    bool auto_reconnect = true;
    bool happy_eyeballs = true;        // RFC 8305 staggered dual-stack
    Duration he_stagger = 250ms;
    Resolver* resolver = nullptr;      // async DNS — see dns.md
    std::optional<SockAddr> unix_target;
    bool fd_passing = false;           // receive SCM_RIGHTS (unix conns)
    std::optional<SockAddr> bind_local;
    SocketOptions sock{};
    FlowControl flow{};
};
```

```cpp
auto cli = em.make_client<MyProto>(ClientConfig{
    .target = {"example.com", 443},
    .resolver = &resolver,             // required for real names — see dns.md
}, std::move(h));
(*cli)->on_state_change([](ClientState s) {
    // Disconnected / Connecting / Connected / ReconnectWait
});
```

- `start()` is implicit in `make_client` (calling it again is a safe no-op).
- Numeric targets resolve synchronously; DNS names need `resolver` — without
  one the lookup falls back to blocking `getaddrinfo` in `start()` (a
  one-time stall on the EM thread).
- `connect_timeout` bounds the whole attempt; the ambient `Context` deadline
  tightens it further when set.
- On failure the client walks the resolved address list, then — with
  `auto_reconnect` — schedules a retry after jittered exponential backoff
  (`initial`, doubling to `max`, ±`jitter`). The backoff resets on each
  successful connect.
- **Happy Eyeballs**: with multiple resolved addresses, attempts launch
  staggered by `he_stagger`, IPv6 first, alternating families; the first to
  connect wins, losers are torn down silently. `auto_reconnect` only sees
  the winner's fate.
- `cli->conn()` is the live `Connection*` (or nullptr between attempts);
  `cli->state()` the `ClientState`.

## Unix-domain sockets

`SockAddr::unix_domain(path)` makes an AF_UNIX address (filesystem paths;
no abstract namespace):

```cpp
ServerConfig sc;
sc.bind = *SockAddr::unix_domain("/tmp/afx.sock");

ClientConfig cc;
cc.unix_target = *SockAddr::unix_domain("/tmp/afx.sock");  // bypasses DNS/HE
```

## SCM_RIGHTS — descriptor passing

On Unix connections, file descriptors ride `sendmsg` ancillary data — the
foundation for hot-restart hand-offs.

Receiving: set `fd_passing = true` on the server or client config and
provide `on_fds`. Descriptors past the 8-fd inbox are closed and counted as
truncation (`CompletionFlag::FdTrunc` → `on_error` with `Err::Truncated`).

Sending: `c->send_fds(payload, fds)` — ≤8 descriptors, attached to exactly
this send's bytes. Requirements: the connection's write queue must be empty
(the rights ride the first byte — a queued write would let plain bytes
overtake them); a short write still delivered the rights, and the remaining
payload is queued as plain bytes.

```cpp
// server side
ServerConfig sc;
sc.bind = *SockAddr::unix_domain(path);
sc.fd_passing = true;
Handlers<P> sh;
sh.on_fds = [](ConnId, std::span<const int> fds) {
    for (int fd : fds) take_ownership(fd);   // you own them now
};
sh.on_messages = [](ConnId, std::span<const Msg>) {};  // drain the byte

// client side
cc.unix_target = *SockAddr::unix_domain(path);
cc.fd_passing = true;
ch.on_open = [&](ConnId id, Peer) {
    auto* c = Connection<P, EventManager>::resolve(em, id);
    const char mark[] = "x";
    int file_fd = ::open("/path", O_RDONLY);
    (void)c->send_fds(ByteSpan(reinterpret_cast<const std::byte*>(mark), 1),
                      std::span<const int>(&file_fd, 1));
};
```

`close()` on a connection that still holds harvested-but-undelivered fds
closes them — the kernel delivered them into the process, so the framework
never leaks them.

## Close reasons

`on_close(id, reason)` — `PeerFin` (orderly close), `LocalClose`,
`FrameError`, `IdleTimeout`, `WriteOverflow`, `Error`, `ConnectFailed`,
`Shutdown` (EM teardown), `HeldOverflow` (coroutine session stopped
consuming — see [Coroutines](coroutines.md)).

## Examples

- `examples/echo_server.cpp` — reuse_port sharded echo.
- `examples/custom_framing.cpp` — general `Protocol` + `on_error`.
- `test/integration/test_client_server.cpp` — client lifecycle and
  reconnect behaviour.
- `test/integration/test_unix.cpp` — Unix sockets and `send_fds`/`on_fds`.

# UDP sockets and multicast

`UdpSocket<P>` is the datagram endpoint. UDP rides the `em.watch()` escape
hatch rather than the proactor submit seam (which carries no source
address): the socket is watched for `Interest::Readable`, and when the fd
reports ready the socket itself performs batched `recvmmsg`/`sendmmsg`. The
code is identical on epoll, io_uring, kqueue, and `SimBackend`.

```cpp
auto sock = em.make_udp<MyProto>(UdpConfig{...}, std::move(handlers));
// Result<UdpSocket<MyProto, EventManager>*>
```

## Configuration

```cpp
struct UdpConfig {
    SockAddr bind = SockAddr::any(0);   // port 0 → kernel-assigned
    SocketOptions sock{};               // nonblock/rcvbuf/sndbuf/timestamping
    bool reuse_addr = true;
    bool reuse_port = false;            // SO_REUSEPORT fan-out across shards
    std::size_t recv_batch = 32;        // datagrams per readiness event
    std::size_t max_datagram = 8192;    // per-datagram scratch size
};
```

`recv_batch × max_datagram` is the scratch arena for one drain — a larger
batch or MTU grows both linearly.

## Delivery: parsed messages or raw datagrams

Exactly one delivery hook is required:

```cpp
UdpHandlers<MyProto> h;
h.proto = MyProto{};

// (a) parsed: each datagram is run through a FRESH framer
h.on_messages = [](SockAddr from, std::span<const Msg> msgs) {};

// (b) raw: the datagram's bytes as received
h.on_datagram = [](SockAddr from, ByteSpan bytes) {};

h.on_error = [](Error e) {};
```

Datagram boundaries are message boundaries: a packet may carry zero or more
complete messages, and framer state never leaks across packets. A trailing
partial message is a truncation error — `on_error` gets
`Err::Truncated` (Frame category); the datagram's complete messages still
deliver.

## Sending

```cpp
// Single datagram — straight to the kernel, no queue.
auto to = SockAddr::parse("239.1.2.3", 5000);   // Result<SockAddr>
if (to) sock->send_to(*to, ByteSpan(p, n));

// Batched — sendmmsg on Linux; returns the count sent (may be < msgs.size()
// on EAGAIN).
std::array<UdpTx, 3> out{{
    {SockAddr::loopback(5001), ByteSpan(a, na)},
    {SockAddr::loopback(5002), ByteSpan(b, nb)},
    {SockAddr::loopback(5003), ByteSpan(c, nc)},
}};
Result<int> sent = sock->send_batch(out);
```

UDP has no stream to queue into — `ENOBUFS`/`EMSGSIZE` come back to the
caller as an `Error`. There is no write-path flow control; send pacing is
the application's job.

```cpp
sock->bound_addr();      // actual bound address (after port 0)
sock->fd();
sock->drains();          // readiness drains that produced ≥1 datagram
sock->datagrams_in();    // total datagrams — datagrams_in/drains ≈ batch factor
```

## Multicast

```cpp
auto grp = SockAddr::parse("239.1.2.3", 0);
sock->join_group(*grp);                    // default interface
sock->join_group(*grp, SockAddr::parse("10.0.0.5", 0).value_or(SockAddr::any(0)));
sock->leave_group(*grp);

// Source filtering (SFM) — IPv4 only; on a v6 group → Err::Unsupported.
sock->join_source(*grp, *SockAddr::parse("10.0.0.9", 0));
sock->leave_source(*grp, *SockAddr::parse("10.0.0.9", 0));

// Sender-side knobs
sock->set_multicast_ttl(4);
sock->set_multicast_loop(false);           // don't loop back to self
sock->set_multicast_if(*SockAddr::parse("10.0.0.5", 0));
```

The free functions live in `afx::sock` too (`join_group`, `leave_group`,
`join_source`, `leave_source`, `set_multicast_ttl`, `set_multicast_loop`,
`set_multicast_if`) for fds managed outside `UdpSocket`.

## Graceful shutdown

UDP has no write queue to drain — closing the fd is the whole story, so a
`UdpSocket` participates in the §20 drain sequence trivially (it closes at
stage 1).

## Notes

- On io_uring the watch is a multishot `POLL_ADD`; on readiness backends it's
  edge-triggered notification — `UdpSocket` drains until EAGAIN either way.
- `bench/bench_udp_batch` measures the batch factor: `--batch 1` vs `--batch
  32` shows the recvmmsg drain effect on loopback flood.

# I/O backends

The backend seam is a concept, not a vtable: `IoBackend` in
`include/afx/backend/backend.hpp`. The model is proactor-canonical
(ADR-0002) — submissions in, `Completion`s out — and readiness-based
backends emulate it by performing the syscall themselves once the fd
reports ready.

| Backend | Platform | Mechanism |
|---|---|---|
| `UringBackend` | Linux (runtime-probed) | True submissions/completions, MSG_RING wakes, `kProactor = true` |
| `EpollBackend` | Linux | readiness emulated as completions; eventfd wakes |
| `KqueueBackend` | macOS / BSD (`AFX_HAVE_KQUEUE`) | readiness emulated as completions; EVFILT_USER wakes |
| `SimBackend` | portable (in-process) | virtual time + scripted events — see [Simulation](simulation.md) |

## Selection

```cpp
EventManagerConfig cfg;                  // BackendKind::Auto by default
cfg.backend = BackendKind::Uring;        // force io_uring
cfg.backend = BackendKind::Epoll;        // force epoll
cfg.backend = BackendKind::Kqueue;       // force kqueue (where supported)
cfg.backend = BackendKind::Sim;          // deterministic tests
```

`Auto` tries io_uring first where it was compiled in
(`AFX_WITH_URING=ON`, the default) and falls back to the platform default —
epoll on Linux, kqueue on macOS/BSD — if probing or setup fails. The backend
is **per-EM**, not per-runtime; a group of io_uring EMs gets MSG_RING fast
posts between themselves automatically (`wake_ring_fd()` + ring-to-ring
messages, zero syscalls on the sender, eventfd fallback so a wake is never
lost).

When `wait == WaitStrategy::Spin`, the auto backend also enables io_uring
SQPOLL (M8-08): submissions go through the kernel's own poll thread, which
removes the `io_uring_enter` syscall from the submit path on a busy loop.

## The interface

```cpp
b.attach(fd, interest, user);   // register fd + completion tag
b.modify(fd, interest, user);   // change interest/tag
b.detach(fd);

b.submit_recv(user, fd, buf);   // completion: bytes or -errno
b.submit_send(user, fd, bytes);
b.submit_sendv(user, fd, iov);
b.submit_accept(user, fd);
b.submit_connect(user, fd, addr);
b.cancel(user);                  // cancel a pending op by tag

b.set_timestamping(fd, on);     // SO_TIMESTAMPING stamps (below)
b.set_fd_inbox(fd, inbox);      // SCM_RIGHTS landing pad (below)
b.wake_ring_fd();               // io_uring fd for MSG_RING, -1 otherwise

int n = b.wait(out, timeout);   // drain ≤ out.size() completions
b.wake();                       // cross-thread wakeup (eventfd/EVFILT_USER)
```

Every submission carries a `UserData` tag — `kind | slot | generation`
packed into 64 bits — which comes back unchanged on the completion. That
tag is what makes `IoId`/`ConnId` generation checks work: a stale tag's
generation never matches the live slot.

## Emulation vs. true proactor

On `EpollBackend`/`KqueueBackend` (`kProactor == false`), a `submit_recv`
translates to readiness interest; when the fd reports ready the backend
performs the read and reports the byte count — the same `Completion` shape
io_uring produces natively. Write backpressure is edge-triggered
(`EPOLLET`): partial sends stay submitted until the kernel accepts all
bytes.

`wait(out, timeout)` caps each drain at `max_io_events` (256 by default);
the timeout arrives as an `IORING_OP_TIMEOUT` on io_uring and as
`epoll_wait`'s timeout on readiness backends — same semantics either way.

## Optional per-fd features

**Kernel/hardware timestamping** (`AFX_WITH_TIMESTAMPING=ON`):
`set_timestamping(fd, true)` attaches `SO_TIMESTAMPING`; recv completions
then carry `Timestamps{hw_ns, sw_ns, tsc}` — NIC hardware time, kernel
software time, and the TSC at dequeue — flagged via
`CompletionFlag::HasHwStamp`/`HasSwStamp`.

**SCM_RIGHTS** (`M11-06`): `set_fd_inbox(fd, {buf, cap, count})` registers a
caller-owned landing pad; recv completions fill it from `recvmsg` control
messages and set `CompletionFlag::HasFds` (or `FdTrunc` on `MSG_CTRUNC` —
excess fds are closed and lost). `FdInbox{}` disables. See the
[SCM_RIGHTS](tcp.md#scm_rights-descriptor-passing) section of the TCP guide.

## Escape hatch

`em.watch(fd)` registers a raw fd with `IoEvent` callbacks; `em.modify(id)`
and `em.unwatch(id)` re-drive it through the generation-checked `IoId`.
Watch completions report a readiness mask
(`ReadyRead`/`ReadyWrite`/`ReadyErr`/`ReadyHangup`) in `result` instead of
a byte count — same plumbing as sockets, user-owned semantics.

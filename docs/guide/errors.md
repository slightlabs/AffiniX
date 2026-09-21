# Errors, results, and context

No exceptions on the data path, no raw `errno` in user code (DESIGN.md
§18). Fallible APIs return `Result<T>`; the error payload is a compact
four-byte `Error` — a category plus a code.

## `Error`

```cpp
struct Error {
    std::uint16_t code;              // Err value, or errno for Sys
    ErrorCategory category;          // Ok | Sys | Net | Frame | Config |
};                                   //   Itc | Internal | Cancelled
```

- `Sys`-category errors carry the errno value as `code` — built with
  `errno_error(e)` / `last_errno()`.
- Everything else carries an `Err` value: `Full`, `Closed`, `Stopped`,
  `Expired`, `NotFound`, `Invalid`, `WouldBlock`, `Overflow`, `BadMagic`,
  `UnsupportedVersion`, `FrameTooLarge`, `ConnectFailed`, `ResolveFailed`,
  `NoResources`, `PermissionDenied`, `Unsupported`, `Truncated`.
- `err.message()` — a printable `string_view` for logs.
- `make_error(category, Err::X)` constructs one; `err.ok()` is true only
  for `category == Ok`.

## `Result<T>`

A C++20 stand-in for `std::expected` with the API surface the framework
needs:

```cpp
Result<SockAddr> r = SockAddr::parse("127.0.0.1", 9000);
if (r) {                     // explicit operator bool
    use(*r);                 // value access: *r, r->, r.value()
} else {
    log(r.error().message());
}
SockAddr a = r.value_or(SockAddr::any(0));   // fallback
```

`Result<void>` stores just the `Error`; `has_value()` means `err.ok()`.

### `AFX_TRY`

Early-return propagation, `try`-style, without exceptions:

```cpp
Result<void> setup(EventManager& em) {
    AFX_TRY(auto srv, em.make_server<P>(cfg, std::move(h)));
    // srv is the unwrapped TcpServer<P, EM>* — EM-owned, not yours to delete
    return {};
}
```

`AFX_TRY(decl, expr)` evaluates `expr` (a `Result<T>`); on failure it
returns `expr.error()` from the enclosing function, otherwise `decl`
receives the moved value.

## `Context` — ambient per-work-item data

Every dispatched work item runs under a `Context` (§7.4, ADR-0008) that
follows it across every hop — mailbox posts, channel pushes, fan dispatch,
`post_and_reply` both directions, coroutine combinators.

```cpp
struct Context {
    Deadline deadline;      // absolute; unset = none
    TraceId trace;          // 16-byte trace + 8-byte span ids
    StopToken stop;         // cooperative cancellation
    std::uint32_t priority; // advisory; used by Fan / WorkerPool
};
```

The EM sets the ambient context around every dispatched item, so
`mb.post(fn)`, `tx.try_push`, and `Fan::dispatch` capture it
automatically — `post_with_ctx` overrides it explicitly.

### `Deadline`

```cpp
Context c;
c.deadline = Deadline::in(500ms);         // absolute deadline, now + d
c.deadline = Deadline::at(timepoint);
```

Deadlines **tighten only**: a hop merges `earliest_of(mine, theirs)`, so a
child can narrow a deadline but never relax its parent's. Expired work is
shed at drain time — a message that arrives too late never runs.

### `StopSource` / `StopToken`

```cpp
StopSource src;
Context c; c.stop = src.token();

src.request();                    // cooperative cancel, any thread
// c.stop.stop_requested() == true in every work item that captured it
```

Timer groups, `when_any` losers, `TaskScope::request_stop`, and coroutine
`sleep`/`recv` waits all honour the same token — one mechanism for the
whole framework.

### `TraceId`

`TraceId{hi, lo, span}` — the 128-bit W3C-style trace id plus span.
`valid()` when either half is nonzero; `short_id()` (`hi ^ lo`) is the
compact form the flight recorder stamps on every record, so a trace id
typed once at accept time can be followed through the crash dump.

## Callback exceptions

`on_callback_error` picks the policy for a handler that throws:
`RouteToHandler` (default — delivered to `on_error` and counted),
`CloseConnection`, or `Terminate`.

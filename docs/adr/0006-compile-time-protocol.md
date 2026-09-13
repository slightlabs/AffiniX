# ADR-0006: Protocol/framing is a compile-time concept

**Status:** accepted
**Date:** 2026-09-13
**One-way door:** no

## Context

"The TCP message header must be customisable by the application" is a core
requirement. Framing runs once per message — millions of times per second per
core — so the mechanism's per-call cost is part of the framework's headline
performance.

## Decision

Two concepts (DESIGN.md §14):

- `Protocol` — general: `parse(ByteSpan) -> ParseResult<Message>` returning
  message / need-more / error. Covers fixed headers, TLV, varint prefixes,
  line-delimited and self-describing formats.
- `FixedHeaderProtocol` — convenience: `kHeaderSize`, `validate(Header)`,
  `body_size(Header)`, adapted onto `Protocol` by `FixedHeaderFramer<P>`.

The protocol is a template parameter of `TcpServer<P>` / `Connection<P>`, so
the framer inlines into the read loop. Messages are *views* into the read
buffer, valid for the duration of the callback, with an explicit `retain()` for
longer lifetimes. Dispatch is batched (`on_messages`).

Endianness, packing and validation are the application's responsibility; the
framework supplies `le16/le32/be16/be32` helpers and never reinterprets network
bytes as a struct on the user's behalf.

## Alternatives rejected

- **Virtual `Framer` interface.** One indirect call per message plus no
  inlining of the length computation; also invites heap-allocated message
  objects. Runtime protocol selection, its only real advantage, is achievable by
  instantiating different `TcpServer<P>` objects on the same EM.
- **Built-in framing options** (length-prefixed with a choice of width). Covers
  80 % of cases and leaves the remaining 20 % with no path at all, which is the
  wrong trade for a library whose users have existing wire formats.
- **Framework-owned message objects.** Requires allocation or pooling per
  message and hides the buffer lifetime, which is exactly the thing users need
  to understand to write correct handlers.

## Consequences

- Framing errors are compile errors, and the read loop is monomorphic per
  protocol.
- Binary size grows with the number of distinct protocols in a program
  (acceptable: typically one to three).
- The dangling-view risk is real, so it is made loud in the API and the docs,
  and debug builds poison retired buffer regions to catch it immediately.
- Per DESIGN.md §26.2, the *awkward* case (line-delimited, no fixed header) is
  implemented first so the concept is not accidentally shaped around
  length-prefixed framing alone.

## Tripwire

Revisit if a user needs to choose a protocol per *connection* at runtime on a
single listener (protocol sniffing) — solvable with a small type-erased
adapter over the same concept, added without changing the core.

# Protocols and framing

The application owns its wire format. A protocol is a compile-time concept
(ADR-0006): the framer is inlined into the connection's read loop, so
parsing costs what your `parse()` costs — no virtual dispatch, no
intermediate buffers, and the framework never allocates for a frame.

A `Message` is a **view into the connection's read buffer**. It is valid for
the duration of `on_messages` only — copy what you keep.

## Two shapes

### `FixedHeaderProtocol` — the common case

A fixed-size header declares the body length. Three members are required:

```cpp
struct MyProto {
    struct __attribute__((packed)) Header {
        std::uint32_t magic_be;
        std::uint16_t type_be;
        std::uint16_t len_be;
    };
    using Message = FrameView<Header>;          // {Header header; ByteSpan body;}
    static constexpr std::size_t kHeaderSize = sizeof(Header);

    Result<void> validate(const Header& h) const {
        if (be32(h.magic_be) != 0xA11CE)
            return make_error(ErrorCategory::Frame, Err::BadMagic);
        return {};
    }
    Result<std::size_t> body_size(const Header& h) const {
        std::size_t n = be16(h.len_be);
        if (n > (1u << 20))                     // ALWAYS bound the body —
            return make_error(ErrorCategory::Frame, Err::FrameTooLarge);
        return n;                               // the read buffer is finite
    }
};
static_assert(sizeof(MyProto::Header) == 8);
```

`FixedHeaderFramer` adapts this to the general protocol shape
automatically — `FramerForT<MyProto>` selects it. The parse sequence:
buffer ≥ header size → `memcpy` the header out → `validate` → `body_size` →
wait for `header + body` bytes → deliver `{header, body}`.

The wire bytes are `memcpy`-ed, never reinterpreted (DESIGN.md §25) — packed
structs and unaligned reads are safe.

### `Protocol` — the general form

For formats that aren't fixed-header — TLV, varint, line-delimited,
self-describing — implement `parse` directly:

```cpp
struct LineMsg { ByteSpan text; };              // view into the read buffer

struct LineProto {
    using Message = LineMsg;

    ParseResult<LineMsg> parse(ByteSpan in) const {
        for (std::size_t i = 0; i < in.size(); ++i)
            if (in[i] == std::byte('\n'))
                return ParseResult<LineMsg>::message_result(
                    LineMsg{in.subspan(0, i)}, i + 1);
        if (in.size() >= 4096)                  // bound it, or a peer can
            return ParseResult<LineMsg>::error_result(  // grow the buffer
                make_error(ErrorCategory::Frame, Err::FrameTooLarge));
        return ParseResult<LineMsg>::need_more(1);
    }
};
```

`parse(ByteSpan in)` returns one of:

| Constructor | Meaning |
|---|---|
| `ParseResult::message_result(msg, consumed)` | One message decoded; `consumed` bytes are retired from the read buffer. |
| `ParseResult::need_more(at_least)` | Incomplete — `at_least` hints the next read size (larger hints reduce reads). |
| `ParseResult::error_result(err)` | Frame error: `on_error` fires, connection closes with `CloseReason::FrameError`. |

The loop calls `parse` repeatedly until `NeedMore`, delivering messages in
batches of up to 64 to `on_messages`. A protocol instance may carry state —
`Handlers::proto` holds it — but a stream-framer that buffers across calls
must remember: **only** bytes the framework still holds are re-presented;
what you report `consumed` is gone.

```cpp
Handlers<LineProto> h;
h.proto.max_line = 8192;                        // stateful protocol instance
```

## Endianness

The wire byte order is the application's choice; helpers are
`be16/be32/be64` and `le16/le32/le64` (constexpr, `std::endian`-aware), plus
generic `byteswap`/`be`/`le`. Always convert explicitly in `validate` and
`body_size` — the header you receive is raw wire bytes.

## Message lifetime

`on_messages(ConnId, std::span<const Message>)` — the span and every body it
points into die when the callback returns. Responding within the callback
(the common case) needs no copies: `send()` queues the bytes before the
buffer is recycled. Holding a span past the callback is a use-after-free;
copy instead.

## Errors and bounds

- `validate`/`body_size` errors → `on_error(id, err)` + close with
  `FrameError`; counted in `stats().frame_errors`.
- `body_size` is trusted to bound memory: an unbounded return lets a peer
  grow the read buffer at will. Always clamp to a protocol maximum.
- The scratch batch is capped at 64 messages per `on_messages` call — a
  single `recv` can produce several calls.

## Complete examples

- `examples/echo_server.cpp` — fixed-header binary protocol (`EchoProto`).
- `examples/custom_framing.cpp` — general `Protocol`, line-delimited.
- `src/admin/http.cpp` — `HttpProto`, the general-form parser the admin
  endpoint dogfoods through `make_server`.

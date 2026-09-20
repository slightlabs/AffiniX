#pragma once

// Protocol and framing — DESIGN.md §14. The application owns its wire format;
// AffiniX inlines the framer into the read loop (compile-time polymorphism,
// ADR-0006). The framework never allocates for a frame: Message is a view
// into the connection's read buffer, valid only for the callback's duration.

#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

#include "afx/sys/result.hpp"
#include "afx/sys/types.hpp"

namespace afx {

template <class M>
struct ParseResult {
    enum class Kind : std::uint8_t { Message, NeedMore, Error };

    Kind kind = Kind::NeedMore;
    M message{};
    std::size_t consumed = 0;  // Kind::Message: bytes to retire
    std::size_t need = 0;      // Kind::NeedMore: at least this many more
    Error error{};

    static ParseResult message_result(M m, std::size_t consumed) {
        ParseResult r;
        r.kind = Kind::Message;
        r.message = std::move(m);
        r.consumed = consumed;
        return r;
    }
    static ParseResult need_more(std::size_t at_least = 1) {
        ParseResult r;
        r.kind = Kind::NeedMore;
        r.need = at_least;
        return r;
    }
    static ParseResult error_result(Error e) {
        ParseResult r;
        r.kind = Kind::Error;
        r.error = e;
        return r;
    }
};

// General form (§14.1): covers TLV, varint, line-delimited, self-describing.
template <class P>
concept Protocol = requires(const P& p, ByteSpan in) {
    typename P::Message;
    { p.parse(in) } -> std::same_as<ParseResult<typename P::Message>>;
};

// Convenience form: a fixed-size header states the body length.
template <class P>
concept FixedHeaderProtocol =
    requires(const P& p, const typename P::Header& h) {
        typename P::Header;
        typename P::Message;
        { P::kHeaderSize } -> std::convertible_to<std::size_t>;
        { p.validate(h) } -> std::same_as<Result<void>>;
        { p.body_size(h) } -> std::same_as<Result<std::size_t>>;
    };

// A parsed frame: header by value (§25 — the framework never reinterprets
// wire bytes as a struct), body a view into the read buffer.
template <class H>
struct FrameView {
    H header{};
    ByteSpan body{};
};

// FixedHeaderFramer adapts a FixedHeaderProtocol to the Protocol concept, so
// there is exactly one read path (§14.1).
template <FixedHeaderProtocol P>
struct FixedHeaderFramer {
    using Header = typename P::Header;
    using Message = typename P::Message;

    P proto{};

    ParseResult<Message> parse(ByteSpan in) const {
        if (in.size() < P::kHeaderSize)
            return ParseResult<Message>::need_more(P::kHeaderSize - in.size());
        Header h{};
        std::memcpy(&h, in.data(), P::kHeaderSize);
        if (auto r = proto.validate(h); !r)
            return ParseResult<Message>::error_result(r.error());
        auto bs = proto.body_size(h);
        if (!bs) return ParseResult<Message>::error_result(bs.error());
        std::size_t total = P::kHeaderSize + *bs;
        if (in.size() < total)
            return ParseResult<Message>::need_more(total - in.size());
        ByteSpan body = in.subspan(P::kHeaderSize, *bs);
        if constexpr (std::is_constructible_v<Message, Header, ByteSpan>) {
            return ParseResult<Message>::message_result(Message{h, body},
                                                        total);
        } else {
            return ParseResult<Message>::message_result(Message{h, body},
                                                        total);
        }
    }
};

// Message batch delivered to on_messages (§14.3). Points into the
// connection's reusable scratch vector; valid for the callback only.
template <class P>
using MessageBatch = std::span<const typename P::Message>;

// Pick the framer for a user protocol type: a Protocol is used directly, a
// FixedHeaderProtocol is wrapped.
template <class P>
struct FramerFor {
    using type = FixedHeaderFramer<P>;
};
template <Protocol P>
struct FramerFor<P> {
    using type = P;
};
template <class P>
using FramerForT = typename FramerFor<P>::type;

// Message type the read loop produces for a user protocol P.
template <class P>
using MessageForT = typename FramerForT<P>::Message;

}  // namespace afx

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "afx/net/protocol.hpp"
#include "afx/sys/endian.hpp"

using namespace afx;

// Wire format for tests: [magic:1][len:2 BE][type:1] + body
struct TestHeader {
    std::uint8_t  magic;
    std::uint16_t len_be;
    std::uint8_t  type;
};
struct TestMsg {
    TestHeader header;
    ByteSpan   body;
};

struct TestProto {
    using Header  = TestHeader;
    using Message = TestMsg;
    static constexpr std::size_t kHeaderSize = sizeof(TestHeader);
    static constexpr std::uint8_t kMagic = 0xA5;

    Result<void> validate(const TestHeader& h) const {
        if (h.magic != kMagic)
            return make_error(ErrorCategory::Frame, Err::BadMagic);
        return {};
    }
    Result<std::size_t> body_size(const TestHeader& h) const {
        return std::size_t(be16(h.len_be));
    }
};
static_assert(FixedHeaderProtocol<TestProto>);
static_assert(Protocol<FixedHeaderFramer<TestProto>>);

static std::vector<std::byte> frame(std::string_view body, std::uint8_t type = 1) {
    std::vector<std::byte> out(TestProto::kHeaderSize + body.size());
    TestHeader h{TestProto::kMagic, be16(std::uint16_t(body.size())), type};
    std::memcpy(out.data(), &h, sizeof(h));
    std::memcpy(out.data() + sizeof(h), body.data(), body.size());
    return out;
}

TEST_CASE("FixedHeaderFramer parses a complete frame") {
    FixedHeaderFramer<TestProto> f;
    auto bytes = frame("hello", 7);
    auto pr = f.parse(ByteSpan(bytes.data(), bytes.size()));
    REQUIRE(pr.kind == decltype(pr)::Kind::Message);
    CHECK(pr.consumed == bytes.size());
    CHECK(pr.message.header.type == 7);
    CHECK(pr.message.body.size() == 5);
    CHECK(std::memcmp(pr.message.body.data(), "hello", 5) == 0);
}

TEST_CASE("FixedHeaderFramer asks for more on partial input") {
    FixedHeaderFramer<TestProto> f;
    auto bytes = frame("hello");
    auto pr = f.parse(ByteSpan(bytes.data(), 2));       // partial header
    CHECK(pr.kind == decltype(pr)::Kind::NeedMore);
    CHECK(pr.need == TestProto::kHeaderSize - 2);

    pr = f.parse(ByteSpan(bytes.data(), bytes.size() - 1));  // partial body
    CHECK(pr.kind == decltype(pr)::Kind::NeedMore);
    CHECK(pr.need == 1);
}

TEST_CASE("FixedHeaderFramer rejects bad magic") {
    FixedHeaderFramer<TestProto> f;
    auto bytes = frame("x");
    bytes[0] = std::byte{0x00};                          // corrupt magic
    auto pr = f.parse(ByteSpan(bytes.data(), bytes.size()));
    CHECK(pr.kind == decltype(pr)::Kind::Error);
    CHECK(pr.error.category == ErrorCategory::Frame);
}

TEST_CASE("FixedHeaderFramer handles back-to-back frames") {
    FixedHeaderFramer<TestProto> f;
    auto a = frame("ab");
    auto b = frame("cde");
    std::vector<std::byte> both(a.begin(), a.end());
    both.insert(both.end(), b.begin(), b.end());
    ByteSpan in(both.data(), both.size());
    auto p1 = f.parse(in);
    REQUIRE(p1.kind == decltype(p1)::Kind::Message);
    in = in.subspan(p1.consumed);
    auto p2 = f.parse(in);
    REQUIRE(p2.kind == decltype(p2)::Kind::Message);
    CHECK(p2.message.body.size() == 3);
}

// Line-delimited protocol exercises the general Protocol path (no framer).
struct LineProto {
    struct Message { ByteSpan line; };
    ParseResult<Message> parse(ByteSpan in) const {
        for (std::size_t i = 0; i < in.size(); ++i)
            if (in[i] == std::byte{'\n'})
                return ParseResult<Message>::message_result(
                    Message{in.first(i)}, i + 1);
        return ParseResult<Message>::need_more(1);
    }
};
static_assert(Protocol<LineProto>);

TEST_CASE("LineProto splits on newlines across arbitrary boundaries") {
    LineProto p;
    std::string data = "one\ntwo\nthree";
    ByteSpan in(reinterpret_cast<const std::byte*>(data.data()), data.size());
    auto r1 = p.parse(in);
    CHECK(r1.message.line.size() == 3);
    auto r2 = p.parse(in.subspan(r1.consumed));
    CHECK(r2.message.line.size() == 3);
    auto r3 = p.parse(in.subspan(r1.consumed + r2.consumed));
    CHECK(r3.kind == decltype(r3)::Kind::NeedMore);
}

TEST_CASE("FramerFor picks Protocol directly, wraps FixedHeaderProtocol") {
    static_assert(std::is_same_v<FramerForT<LineProto>, LineProto>);
    static_assert(std::is_same_v<FramerForT<TestProto>,
                                 FixedHeaderFramer<TestProto>>);
    CHECK(true);
}

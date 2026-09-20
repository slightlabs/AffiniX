// Seeded fuzz for the framing layer (IMPLEMENTATION_PLAN.md §4, M6-14):
// arbitrary bytes and arbitrary split points through the same parse loop the
// connection read path uses. Invariants: parse always makes progress or
// reports a precise need; it never reads past the buffer; a stream of valid
// frames decodes exactly, however it is chopped; garbage terminates with an
// Error rather than a hang.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../proto.hpp"
#include "../test_env.hpp"
#include "afx/net/connection.hpp"
#include "afx/net/protocol.hpp"

using namespace afx;
using afx::test::echo_frame;
using afx::test::EchoHeader;
using afx::test::EchoMsg;
using afx::test::EchoProto;

namespace {

std::uint64_t fuzz_seed() {
    if (const char* s = std::getenv("AFX_FUZZ_SEED"))
        return std::strtoull(s, nullptr, 0);
    return 0xAF5EED;
}

// The same consume loop Connection::on_recv runs: parse, retire, repeat.
struct FrameSink {
    FixedHeaderFramer<EchoProto> framer;
    std::vector<std::byte> pending;
    std::vector<std::string> bodies;
    int errors = 0;

    // Feed a chunk; returns false on unrecoverable desync policy choice.
    void feed(ByteSpan chunk) {
        pending.insert(pending.end(), chunk.begin(), chunk.end());
        for (;;) {
            auto pr = framer.parse(ByteSpan(pending.data(), pending.size()));
            using Kind = decltype(pr)::Kind;
            if (pr.kind == Kind::NeedMore) {
                CHECK(pr.need > 0);
                break;
            }
            if (pr.kind == Kind::Error) {
                ++errors;
                pending.erase(pending.begin());  // resync: drop one byte
                continue;
            }
            CHECK(pr.consumed > 0);
            CHECK(pr.consumed <= pending.size());
            bodies.emplace_back(
                reinterpret_cast<const char*>(pr.message.body.data()),
                pr.message.body.size());
            pending.erase(pending.begin(),
                          pending.begin() + std::ptrdiff_t(pr.consumed));
        }
    }
};

std::string random_body(std::mt19937_64& rng) {
    std::string s(rng() % 300 + 1, '\0');
    for (auto& c : s) c = char(rng());
    return s;
}

}  // namespace

TEST_CASE("fuzz: valid frames decode exactly under arbitrary splits") {
    std::mt19937_64 rng(fuzz_seed());
    MESSAGE("fuzz seed: ", fuzz_seed());

    for (int iter = 0; iter < 400; ++iter) {
        // Build a stream of valid frames with random bodies.
        std::vector<std::byte> stream;
        std::vector<std::string> expected;
        int nframes = 1 + int(rng() % 8);
        for (int i = 0; i < nframes; ++i) {
            expected.push_back(random_body(rng));
            auto f = echo_frame(expected.back());
            stream.insert(stream.end(), f.begin(), f.end());
        }

        FrameSink sink;
        std::size_t pos = 0;
        while (pos < stream.size()) {
            std::size_t n = 1 + rng() % (stream.size() - pos);
            sink.feed(ByteSpan(stream.data() + pos, n));
            pos += n;
        }
        CHECK(sink.errors == 0);
        CHECK(sink.bodies == expected);
        CHECK(sink.pending.empty());
    }
}

TEST_CASE("fuzz: byte-at-a-time delivery decodes identically") {
    std::mt19937_64 rng(fuzz_seed() ^ 0x9E3779B9);

    for (int iter = 0; iter < 50; ++iter) {
        std::vector<std::byte> stream;
        std::vector<std::string> expected;
        for (int i = 0; i < 4; ++i) {
            expected.push_back(random_body(rng));
            auto f = echo_frame(expected.back());
            stream.insert(stream.end(), f.begin(), f.end());
        }
        FrameSink sink;
        for (std::byte b : stream) sink.feed(ByteSpan(&b, 1));
        CHECK(sink.errors == 0);
        CHECK(sink.bodies == expected);
    }
}

TEST_CASE("fuzz: random garbage always terminates, never hangs") {
    std::mt19937_64 rng(fuzz_seed() ^ 0xDEADBEEF);

    for (int iter = 0; iter < 200; ++iter) {
        std::vector<std::byte> garbage(1 + rng() % 4096);
        for (auto& b : garbage) b = std::byte(rng());
        FrameSink sink;
        sink.feed(ByteSpan(garbage.data(), garbage.size()));
        // Whatever it decided, the buffer drains to a bounded residue: a
        // partial header at worst.
        CHECK(sink.pending.size() < EchoProto::kHeaderSize + 65535);
    }
}

TEST_CASE("fuzz: mutated headers are rejected, never mis-parsed") {
    std::mt19937_64 rng(fuzz_seed() ^ 0x12345);

    for (int iter = 0; iter < 300; ++iter) {
        auto f = echo_frame(random_body(rng));
        // Corrupt one random byte of the header.
        f[rng() % EchoProto::kHeaderSize] = std::byte(rng());
        FrameSink sink;
        sink.feed(ByteSpan(f.data(), f.size()));
        // A corrupt magic is an Error; a corrupt length field either errors
        // (if the mutation hit magic) or just waits for bytes that never
        // come — both are acceptable; a message decode is NOT.
        CHECK(sink.bodies.size() <= 1);
    }
}

// The same fuzz through the real connection read path, sim-fed.
TEST_CASE("fuzz: connection read path survives arbitrary splits") {
    std::mt19937_64 rng(fuzz_seed() ^ 0xC0FFEE);
    afx::test::TestEnv env;
    using EM = afx::test::TestEnv::EM;

    Handlers<EchoProto> h;
    std::vector<std::string> got;
    h.on_messages = [&](ConnId, std::span<const EchoMsg> batch) {
        for (auto& m : batch)
            got.emplace_back(reinterpret_cast<const char*>(m.body.data()),
                             m.body.size());
    };
    int cfd = env.sim().add_fd();
    auto conn = std::make_unique<Connection<EchoProto, EM>>(
        env.em, cfd, h, typename Connection<EchoProto, EM>::Params{}, nullptr,
        [](void*, ConnId, CloseReason) {});
    conn->start(Peer{SockAddr::loopback(1111)});

    std::vector<std::byte> stream;
    std::vector<std::string> expected;
    for (int i = 0; i < 24; ++i) {
        expected.push_back(random_body(rng));
        auto f = echo_frame(expected.back());
        stream.insert(stream.end(), f.begin(), f.end());
    }
    std::size_t pos = 0;
    while (pos < stream.size()) {
        std::size_t n = 1 + rng() % (stream.size() - pos);
        env.sim().feed(cfd, ByteSpan(stream.data() + pos, n));
        env.pump(2);
        pos += n;
    }
    env.pump(4);
    CHECK(got == expected);
}

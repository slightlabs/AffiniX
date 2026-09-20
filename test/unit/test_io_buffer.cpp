#include <doctest/doctest.h>

#include <cstring>

#include "afx/core/io_buffer.hpp"

using namespace afx;

static void write_bytes(IoBuffer& b, std::string_view s) {
    auto w = b.writable(s.size());
    std::memcpy(w.data(), s.data(), s.size());
    b.commit(s.size());
}
static std::string read_str(IoBuffer& b) {
    auto r = b.readable();
    return {reinterpret_cast<const char*>(r.data()), r.size()};
}

TEST_CASE("IoBuffer basic write/read/consume") {
    IoBuffer b;
    write_bytes(b, "hello");
    CHECK(read_str(b) == "hello");
    b.consume(2);
    CHECK(read_str(b) == "llo");
    write_bytes(b, " world");
    CHECK(read_str(b) == "llo world");
    b.consume(9);
    CHECK(b.empty());
}

TEST_CASE("IoBuffer prepend writes a header in front, no copy") {
    IoBuffer b;
    write_bytes(b, "body");
    auto hdr = b.prepend(4);
    std::memcpy(hdr.data(), "HDR!", 4);
    CHECK(read_str(b) == "HDR!body");
    CHECK(b.size() == 8);
}

TEST_CASE("IoBuffer prepend beyond headroom shifts data") {
    IoBuffer b(256, 4);  // only 4 bytes of initial headroom
    write_bytes(b, "payload");
    auto big = b.prepend(16);
    std::memset(big.data(), 0xAB, 16);
    CHECK(b.size() == 23);
    CHECK(read_str(b).substr(16) == "payload");
}

TEST_CASE("IoBuffer grows on demand") {
    IoBuffer b(128);
    std::string big(1000, 'x');
    write_bytes(b, big);
    CHECK(b.size() == 1000);
    write_bytes(b, "tail");
    CHECK(read_str(b) == big + "tail");
}

TEST_CASE("IoBuffer consume-then-write reclaims space via compact") {
    IoBuffer b(64, 0);
    std::string s(48, 'a');
    write_bytes(b, s);
    b.consume(40);
    // writable must satisfy at_least by compacting rather than growing.
    auto w = b.writable(50);
    CHECK(w.size() >= 50);
}

#pragma once

// Shared test wire protocol: [magic:1][len:2 BE][type:1] + body.

#include <cstring>
#include <vector>

#include "afx/net/protocol.hpp"
#include "afx/sys/endian.hpp"

namespace afx::test {

struct __attribute__((packed)) EchoHeader {
    std::uint8_t magic;
    std::uint16_t len_be;
    std::uint8_t type;
};
static_assert(sizeof(EchoHeader) == 4, "wire header must be padding-free");
struct EchoMsg {
    EchoHeader header;
    ByteSpan body;
};

struct EchoProto {
    using Header = EchoHeader;
    using Message = EchoMsg;
    static constexpr std::size_t kHeaderSize = sizeof(EchoHeader);
    static constexpr std::uint8_t kMagic = 0xA5;

    Result<void> validate(const EchoHeader& h) const {
        if (h.magic != kMagic)
            return make_error(ErrorCategory::Frame, Err::BadMagic);
        return {};
    }
    Result<std::size_t> body_size(const EchoHeader& h) const {
        return std::size_t(be16(h.len_be));
    }
};

inline std::vector<std::byte> echo_frame(std::string_view body,
                                         std::uint8_t type = 1) {
    std::vector<std::byte> out(EchoProto::kHeaderSize + body.size());
    EchoHeader h{EchoProto::kMagic, be16(std::uint16_t(body.size())), type};
    std::memcpy(out.data(), &h, sizeof(h));
    std::memcpy(out.data() + sizeof(h), body.data(), body.size());
    return out;
}

}  // namespace afx::test

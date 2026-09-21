#pragma once

// Minimal HTTP/1.1 for the admin endpoint (M12-01) — dogfooded through
// make_server, so it proves the custom-framing seam on a real protocol.
// Scope: request head up to CRLFCRLF, Content-Length bodies, no chunked
// encoding, no keep-alive (every response carries `Connection: close`).
// Anything beyond that is an HTTP framework — explicitly not our job (§103).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "afx/net/protocol.hpp"
#include "afx/sys/types.hpp"

namespace afx::admin {

// Views into the connection read buffer — valid until the next parse.
struct HttpRequest {
    std::string_view method;   // GET / POST
    std::string_view path;     // /stats (query stripped into `query`)
    std::string_view query;    // raw a=b&c=d tail
    std::string_view version;  // HTTP/1.1
    ByteSpan body;
    std::size_t content_length = 0;
};

struct HttpProto {
    using Message = HttpRequest;
    std::size_t max_head = 16 * 1024;
    std::size_t max_body = 1 << 20;

    ParseResult<HttpRequest> parse(ByteSpan in) const;
};

static_assert(Protocol<HttpProto>);

// Serialize a response head + body. `Connection: close` is unconditional.
inline std::string http_response(int status, std::string_view reason,
                                 std::string_view content_type,
                                 std::string_view body) {
    std::string out;
    out.reserve(96 + body.size());
    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += reason;
    out += "\r\nContent-Type: ";
    out += content_type;
    out += "\r\nContent-Length: ";
    out += std::to_string(body.size());
    out += "\r\nConnection: close\r\n\r\n";
    out += body;
    return out;
}

}  // namespace afx::admin

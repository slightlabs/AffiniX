#include "afx/admin/http.hpp"

#include <cstdlib>
#include <cstring>

namespace afx::admin {

namespace {

Error bad_request() {
    return make_error(ErrorCategory::Frame, Err::Invalid);
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if ((a[i] | 0x20) != (b[i] | 0x20)) return false;
    return true;
}

// RFC 9112 field value → size; trailing/leading OWS tolerated. Returns false
// on malformed or absurdly large values.
bool parse_content_length(std::string_view v, std::size_t& out) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
        v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
        v.remove_suffix(1);
    if (v.empty() || v.size() > 10) return false;
    std::size_t n = 0;
    for (char c : v) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + std::size_t(c - '0');
    }
    out = n;
    return true;
}

}  // namespace

ParseResult<HttpRequest> HttpProto::parse(ByteSpan in) const {
    // ---- head boundary
    const auto* base = reinterpret_cast<const char*>(in.data());
    const std::size_t scan = std::min(in.size(), max_head);
    std::size_t head_end = std::string_view::npos;
    for (std::size_t i = 0; i + 3 < scan; ++i) {
        if (base[i] == '\r' && base[i + 1] == '\n' && base[i + 2] == '\r' &&
            base[i + 3] == '\n') {
            head_end = i + 4;
            break;
        }
    }
    if (head_end == std::string_view::npos) {
        if (in.size() >= max_head)
            return ParseResult<HttpRequest>::error_result(
                make_error(ErrorCategory::Frame, Err::FrameTooLarge));
        return ParseResult<HttpRequest>::need_more();
    }
    std::string_view head(base, head_end);

    // ---- request line: METHOD SP target SP HTTP/1.x CRLF
    auto eol = head.find("\r\n");
    if (eol == std::string_view::npos)
        return ParseResult<HttpRequest>::error_result(bad_request());
    std::string_view line = head.substr(0, eol);
    auto sp1 = line.find(' ');
    auto sp2 = line.rfind(' ');
    if (sp1 == std::string_view::npos || sp2 == sp1)
        return ParseResult<HttpRequest>::error_result(bad_request());

    HttpRequest req;
    req.method = line.substr(0, sp1);
    std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    req.version = line.substr(sp2 + 1);
    if (auto q = target.find('?'); q != std::string_view::npos) {
        req.path = target.substr(0, q);
        req.query = target.substr(q + 1);
    } else {
        req.path = target;
    }

    // ---- headers: only Content-Length matters to us; everything else is
    // skipped line-wise (admin endpoints take no negotiation).
    std::size_t content_length = 0;
    for (std::size_t pos = eol + 2; pos + 2 < head.size();) {
        auto nl = head.find("\r\n", pos);
        if (nl == std::string_view::npos) break;
        std::string_view h = head.substr(pos, nl - pos);
        pos = nl + 2;
        auto colon = h.find(':');
        if (colon == std::string_view::npos) continue;
        // case-insensitive compare, no allocation
        std::string_view name = h.substr(0, colon);
        if (iequals(name, "content-length")) {
            if (!parse_content_length(h.substr(colon + 1), content_length))
                return ParseResult<HttpRequest>::error_result(bad_request());
        }
    }
    if (content_length > max_body)
        return ParseResult<HttpRequest>::error_result(
            make_error(ErrorCategory::Frame, Err::FrameTooLarge));

    const std::size_t total = head_end + content_length;
    if (in.size() < total)
        return ParseResult<HttpRequest>::need_more(total - in.size());
    req.content_length = content_length;
    req.body = in.subspan(head_end, content_length);
    return ParseResult<HttpRequest>::message_result(req, total);
}

}  // namespace afx::admin

// Custom framing (M6-16): a user-defined wire format via the general
// Protocol concept (DESIGN.md §14.1, ADR-0006). Line-delimited text —
// no fixed header, parse() scans for '\n' directly. The framer is
// inlined into the read loop; Message is a view into the read buffer.
//
// Protocol: request  = "<text>\n"   (max 4 KiB)
//           response = "<TEXT>\n"   (uppercased echo)
//
// Run: ./custom_framing [port]     then:  nc localhost 9001

#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "afx/afx.hpp"

using namespace afx;

namespace {

constexpr std::size_t kMaxLine = 4096;

struct LineMsg {
    ByteSpan text;  // without the trailing '\n'; view into the read buffer
};

// The general Protocol concept: parse() returns Message / NeedMore / Error.
struct LineProto {
    using Message = LineMsg;

    ParseResult<LineMsg> parse(ByteSpan in) const {
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (in[i] == std::byte('\n'))
                return ParseResult<LineMsg>::message_result(
                    LineMsg{in.subspan(0, i)}, i + 1);
        }
        // Unterminated input beyond the cap is a frame error, not a wait.
        if (in.size() >= kMaxLine)
            return ParseResult<LineMsg>::error_result(
                make_error(ErrorCategory::Frame, Err::FrameTooLarge));
        return ParseResult<LineMsg>::need_more(1);
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = argc > 1 ? std::uint16_t(std::atoi(argv[1])) : 9001;

    Topology topo = Topology::detect();
    std::size_t n = std::max<std::size_t>(1, topo.physical_cores().size());

    Runtime rt(std::move(topo));
    ThreadConfig cfg;
    cfg.placement = Placement::OnePerPhysicalCore;
    auto g = rt.spawn_group("lines", n, cfg);

    g.each([port](EventManager& em) {
        Handlers<LineProto> h;
        h.on_messages = [&](ConnId id, std::span<const LineMsg> batch) {
            auto* c = Connection<LineProto, EventManager>::resolve(em, id);
            if (!c) return;
            std::byte buf[kMaxLine + 1];
            for (auto& m : batch) {
                std::size_t n = std::min(m.text.size(), kMaxLine);
                for (std::size_t i = 0; i < n; ++i)
                    buf[i] = std::byte(
                        std::toupper(static_cast<unsigned char>(m.text[i])));
                buf[n] = std::byte('\n');
                (void)c->send(ByteSpan(buf, n + 1));
            }
        };
        h.on_error = [](ConnId, Error e) {
            std::fprintf(stderr, "frame error: %.*s\n", int(e.message().size()),
                         e.message().data());
        };
        ServerConfig sc;
        sc.bind = SockAddr::any(port);
        sc.reuse_port = true;
        auto srv = em.make_server<LineProto>(sc, std::move(h));
        if (!srv)
            std::fprintf(stderr, "bind failed: %.*s\n",
                         int(srv.error().message().size()),
                         srv.error().message().data());
    });

    rt.on_signal({SIGINT, SIGTERM}, [&] { rt.shutdown(5s); });
    rt.start();
    std::printf("line server on :%d, %zu shard(s) — lines echo uppercased\n",
                port, g.size());
    rt.join();
    return 0;
}

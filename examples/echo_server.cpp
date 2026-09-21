// Echo server: one EM per physical core, SO_REUSE_PORT, fixed-header
// protocol. DESIGN.md §24 example. Run: ./echo_server [port]

#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "afx/afx.hpp"

using namespace afx;

// Protocol: [magic:1][len:2 BE][type:1] + body — echoes every frame.
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
    Result<void> validate(const EchoHeader& h) const {
        if (h.magic != 0xA5)
            return make_error(ErrorCategory::Frame, Err::BadMagic);
        return {};
    }
    Result<std::size_t> body_size(const EchoHeader& h) const {
        return std::size_t(be16(h.len_be));
    }
};

int main(int argc, char** argv) {
    std::uint16_t port = argc > 1 ? std::uint16_t(std::atoi(argv[1])) : 9000;

    Topology topo = Topology::detect();
    std::size_t n = std::max<std::size_t>(1, topo.physical_cores().size());

    Runtime rt(std::move(topo));
    ThreadConfig cfg;
    cfg.placement = Placement::OnePerPhysicalCore;
    auto g = rt.spawn_group("echo", n, cfg);

    g.each([port](EventManager& em) {
        Handlers<EchoProto> h;
        h.on_messages = [&](ConnId id, std::span<const EchoMsg> batch) {
            auto* c = Connection<EchoProto, EventManager>::resolve(em, id);
            if (!c) return;
            for (auto& m : batch) {
                std::array<std::byte, sizeof(EchoHeader)> hdr;
                std::memcpy(hdr.data(), &m.header, sizeof(hdr));
                std::array<ByteSpan, 2> parts{ByteSpan(hdr.data(), hdr.size()),
                                              m.body};
                c->send_scatter(parts);
            }
        };
        ServerConfig cfg;
        cfg.bind = SockAddr::any(port);
        cfg.reuse_port = true;
        auto srv = em.make_server<EchoProto>(cfg, std::move(h));
        if (!srv)
            std::fprintf(stderr, "bind failed: %.*s\n",
                         int(srv.error().message().size()),
                         srv.error().message().data());
    });

    rt.on_signal({SIGINT, SIGTERM}, [&] { rt.shutdown(5s); });
    rt.start();
    std::printf("echo server on :%d, %zu shard(s)\n", port, g.size());
    rt.join();
    return 0;
}

// Observable echo server: the §24 echo workload with the M12 admin plane
// attached. One EM per physical core serves echo traffic on `port`; a
// dedicated Block-mode admin EM (own thread, no dataplane CPU) answers:
//
//   GET /healthz /version /stats /metrics /conns /placement /config /flight
//   POST /admin/stall_threshold?ns=N[&shard=i]
//   POST /admin/chaos?on=0|1[&shard=i]
//   POST /admin/log_level?level=debug|info|warn|error
//
// Run: ./observable_server [port] [admin_port]   (defaults 9000 / 9100)

#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "afx/afx.hpp"

using namespace afx;

struct EchoHeader {
    std::uint8_t magic;
    std::uint16_t len_be;
    std::uint8_t type;
};
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
    std::uint16_t aport = argc > 2 ? std::uint16_t(std::atoi(argv[2])) : 9100;

    Topology topo = Topology::detect();
    std::size_t n = std::max<std::size_t>(1, topo.physical_cores().size());

    Runtime rt(std::move(topo));
    ThreadConfig cfg;
    cfg.placement = Placement::OnePerPhysicalCore;
    cfg.em.profile_stages = true;  // afx_stage_ns{stage=...} on /metrics
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
                (void)c->send_scatter(parts);
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

    // The admin EM blocks between requests — zero dataplane cost while idle.
    admin::AdminServer admin(rt,
                             admin::AdminConfig{.bind = SockAddr::any(aport)});
    auto bound = admin.start();
    if (bound)
        std::printf("echo on :%d (%zu shards) — admin on :%d\n", port, g.size(),
                    bound->port());
    else
        std::fprintf(stderr, "admin bind failed; dataplane unaffected\n");

    rt.join();
    admin.stop();
    return 0;
}

// Session server, coroutine style (M9, ADR-0005). Same wire protocol and
// service as echo_server — but each connection runs as a lazy coroutine:
// sequential code instead of a callback state machine.
//
// Demonstrates: make_coro_server, ConnRef recv/send awaiters, with_timeout
// as a per-recv idle deadline, and arena-backed coroutine frames.
//
// Run: ./session_coro [port]     (default 9001; idle timeout 30s)

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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

using Conn = ConnRef<EchoProto, EventManager>;

// recv() is an awaiter; with_timeout composes Tasks, so wrap one batch in a
// task. This is also the shape for recv-side deadlines generally.
CoroTask<Result<Conn::Batch>> recv_once(Conn c) {
    co_return co_await c.recv();
}

CoroTask<void> session(Conn c) {
    // Greet the peer, then echo frames until it leaves or idles for 30s.
    static constexpr char kHello[] = "session_coro: hello\n";
    auto sent = co_await c.send(ByteSpan(
        reinterpret_cast<const std::byte*>(kHello), sizeof(kHello) - 1));
    if (sent == SendResult::Closed) co_return;

    // `frame` lives in the coroutine frame: stable across send suspensions
    // and reused per message (batch bodies point into the conn read buffer,
    // so payload is copied out before the next recv()).
    std::vector<std::byte> frame;
    for (;;) {
        auto rr = co_await coro::with_timeout(30s, recv_once(c));
        if (!rr) co_return;     // idle deadline / stopped
        auto& batch = *rr;      // Result<Batch>
        if (!batch) co_return;  // peer closed or conn error
        for (auto& m : *batch) {
            frame.resize(sizeof(EchoHeader) + m.body.size());
            std::memcpy(frame.data(), &m.header, sizeof(EchoHeader));
            std::memcpy(frame.data() + sizeof(EchoHeader), m.body.data(),
                        m.body.size());
            if (co_await c.send(ByteSpan(frame.data(), frame.size())) ==
                SendResult::Closed)
                co_return;
        }
    }
}

int main(int argc, char** argv) {
    std::uint16_t port = argc > 1 ? std::uint16_t(std::atoi(argv[1])) : 9001;

    Topology topo = Topology::detect();
    std::size_t n = std::max<std::size_t>(1, topo.physical_cores().size());

    Runtime rt(std::move(topo));
    ThreadConfig cfg;
    cfg.placement = Placement::OnePerPhysicalCore;
    auto g = rt.spawn_group("coro-echo", n, cfg);

    g.each([port](EventManager& em) {
        ServerConfig cfg;
        cfg.bind = SockAddr::any(port);
        cfg.reuse_port = true;
        auto srv = em.make_coro_server<EchoProto>(cfg, &session);
        if (!srv)
            std::fprintf(stderr, "bind failed: %.*s\n",
                         int(srv.error().message().size()),
                         srv.error().message().data());
    });

    rt.on_signal({SIGINT, SIGTERM}, [&] { rt.shutdown(5s); });
    rt.start();
    std::printf("coro session server on :%d, %zu shard(s)\n", port, g.size());
    rt.join();
    return 0;
}

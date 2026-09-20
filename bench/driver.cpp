// bench/driver — minimal closed-loop load driver (M6-17; grows into
// tools/afx-load in M8-09). Speaks the echo_server wire format
// ([magic:1][len:2 BE][type:1] + body), one outstanding request per
// connection, prints one JSON result line for baselines.
//
// Usage: bench_driver <host> <port> <conns> <seconds> [payload_bytes]

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "afx/afx.hpp"

using namespace afx;

namespace {

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

std::atomic<std::uint64_t> g_replies{0};
std::atomic<std::uint64_t> g_opened{0};
std::atomic<bool> g_stop{false};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr,
                     "usage: %s <host> <port> <conns> <seconds> "
                     "[payload_bytes]\n",
                     argv[0]);
        return 2;
    }
    const char* host = argv[1];
    std::uint16_t port = std::uint16_t(std::atoi(argv[2]));
    int conns = std::atoi(argv[3]);
    int seconds = std::atoi(argv[4]);
    std::size_t payload = argc > 5 ? std::size_t(std::atoi(argv[5])) : 64;

    EventManager em(EventManagerConfig{.name = "driver"});

    std::vector<std::byte> request(sizeof(EchoHeader) + payload);
    EchoHeader hdr{0xA5, be16(std::uint16_t(payload)), 0x01};
    std::memcpy(request.data(), &hdr, sizeof(hdr));

    // Closed loop: one outstanding request per connection — a reply kicks
    // the next send (M8-09 will add open-loop rate control and HDR output).
    for (int i = 0; i < conns; ++i) {
        Handlers<EchoProto> h;
        h.on_open = [&em, &request](ConnId id, Peer) {
            ++g_opened;
            if (auto* c = Connection<EchoProto, EventManager>::resolve(em, id))
                (void)c->send(ByteSpan(request.data(), request.size()));
        };
        h.on_messages = [&em, &request](ConnId id,
                                        std::span<const EchoMsg> batch) {
            g_replies += batch.size();
            if (g_stop.load(std::memory_order_relaxed)) return;
            if (auto* c = Connection<EchoProto, EventManager>::resolve(em, id))
                (void)c->send(ByteSpan(request.data(), request.size()));
        };
        ClientConfig cc;
        cc.target = Endpoint{host, port};
        cc.connect_timeout = 5s;
        cc.auto_reconnect = false;
        auto cli = em.make_client<EchoProto>(cc, std::move(h));
        if (!cli) {
            std::fprintf(stderr, "client %d failed\n", i);
            return 1;
        }
    }

    std::thread loop([&] { em.run(); });
    TimePoint t0 = SteadyClock::now();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    g_stop.store(true);
    em.stop();
    loop.join();
    double secs = Nanos(SteadyClock::now() - t0).count() / 1e9;
    std::printf(
        "{\"benchmark\":\"driver\",\"conns\":%d,\"payload\":%zu,"
        "\"replies\":%llu,\"opened\":%llu,\"seconds\":%.3f,"
        "\"replies_per_sec\":%.0f}\n",
        conns, payload, (unsigned long long)g_replies.load(),
        (unsigned long long)g_opened.load(), secs,
        secs > 0 ? g_replies.load() / secs : 0.0);
    return 0;
}

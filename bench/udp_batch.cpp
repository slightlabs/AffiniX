// bench/udp_batch — M11-01 exit criterion: UDP batch receive throughput.
//
// One EM holds a sender and a receiver UdpSocket on loopback; the sender
// floods fixed-size datagrams via send_batch for `--seconds`, the receiver
// drains them with `--batch N` slots per readiness wake. Reported: rx
// datagrams/sec, mean datagrams per drain (the batch factor — 1.0 without
// batching), tx rate, and loss. `recv_batch=1` vs `=32` shows the
// recvmmsg win directly.
//
// Usage: bench_udp_batch [--seconds 2] [--size 64] [--batch 32]
//                        [--rcvbuf-mb 8] [--backend auto|epoll|uring]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "afx/afx.hpp"

using namespace afx;

namespace {

// A Protocol whose "frame" is the whole datagram — the bench arms
// on_datagram so this never actually parses, but FramerForT<P> must resolve.
struct RawProto {
    using Message = ByteSpan;
    ParseResult<ByteSpan> parse(ByteSpan in) const {
        return ParseResult<ByteSpan>::message_result(in, in.size());
    }
};

}  // namespace

int main(int argc, char** argv) {
    double seconds = 2.0;
    std::size_t size = 64;
    std::size_t batch = 32;
    int rcvbuf_mb = 8;
    BackendKind bk = BackendKind::Auto;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](const char* d) -> std::string {
            auto eq = a.find('=');
            if (eq != std::string::npos) return a.substr(eq + 1);
            if (i + 1 < argc) return argv[++i];
            return d;
        };
        if (a.find("--seconds") == 0)
            seconds = std::stod(val("2"));
        else if (a.find("--size") == 0)
            size = std::stoul(val("64"));
        else if (a.find("--batch") == 0)
            batch = std::stoul(val("32"));
        else if (a.find("--rcvbuf-mb") == 0)
            rcvbuf_mb = std::stoi(val("8"));
        else if (a.find("--backend") == 0) {
            auto v = val("auto");
            bk = v == "epoll"    ? BackendKind::Epoll
                 : v == "uring"  ? BackendKind::Uring
                 : v == "kqueue" ? BackendKind::Kqueue
                                 : BackendKind::Auto;
        }
    }

    EventManager em{EventManagerConfig{
        .name = "udp-batch", .wait = WaitStrategy::Spin, .backend = bk}};

    std::atomic<std::uint64_t> rx_n{0};
    std::atomic<std::uint64_t> tx_n{0};
    std::atomic<std::uint64_t> tx_err{0};

    UdpConfig rc;
    rc.bind = SockAddr::loopback(0);
    rc.recv_batch = batch;
    rc.sock.rcvbuf = rcvbuf_mb << 20;
    UdpHandlers<RawProto> rh;
    rh.on_datagram = [&](SockAddr, ByteSpan) { ++rx_n; };
    auto rs = em.make_udp<RawProto>(rc, std::move(rh));
    if (!rs) {
        std::fprintf(stderr, "receiver: %s\n", "open failed");
        return 1;
    }
    auto* recv = *rs;

    UdpConfig sc;
    sc.bind = SockAddr::loopback(0);
    auto ss = em.make_udp<RawProto>(sc, UdpHandlers<RawProto>{});
    if (!ss) return 1;
    auto* send = *ss;
    SockAddr dst = recv->bound_addr();

    std::vector<std::byte> payload(size, std::byte{0xAB});
    constexpr std::size_t kBurst = 64;
    std::vector<UdpTx> burst(kBurst, UdpTx{});
    for (auto& t : burst)
        t = UdpTx{dst, ByteSpan(payload.data(), payload.size())};

    auto t0 = std::chrono::steady_clock::now();
    auto deadline = t0 + std::chrono::duration<double>(seconds);
    // Flood pump: on_idle runs once per loop iteration; each call pushes a
    // burst until the deadline. ENOBUFS tails count as tx_err.
    em.on_idle([&] {
        if (std::chrono::steady_clock::now() >= deadline) {
            em.stop();
            return;
        }
        auto n = send->send_batch(burst);
        if (n && *n > 0)
            tx_n += std::uint64_t(*n);
        else
            tx_err += kBurst;
    });
    em.run();

    double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    auto rx = rx_n.load(), tx = tx_n.load();
    double rx_rate = rx / elapsed;
    double tx_rate = tx / elapsed;
    double loss = tx ? 100.0 * (1.0 - double(rx) / double(tx)) : 0.0;
    double factor = recv->drains() ? double(rx) / double(recv->drains()) : 0.0;
    std::printf(
        "{\"benchmark\":\"udp_batch\",\"batch\":%zu,\"size\":%zu,"
        "\"seconds\":%.1f,\"tx\":%llu,\"rx\":%llu,\"drains\":%llu,"
        "\"batch_factor\":%.2f,\"tx_rate\":%.0f,\"rx_rate\":%.0f,"
        "\"loss_pct\":%.2f}\n",
        batch, size, elapsed, (unsigned long long)tx, (unsigned long long)rx,
        (unsigned long long)recv->drains(), factor, tx_rate, rx_rate, loss);
    return 0;
}

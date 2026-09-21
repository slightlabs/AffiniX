// bench/admin_impact (M12 exit): does a Block-mode admin EM measurably slow
// a spinning dataplane shard?
//
// A Spin-wait shard runs a self-reposting defer() chain — each repost is one
// unit of pure shard-side work (no kernel IO), so work/sec measures the
// shard's compute capacity directly. Phase 1 is baseline; phase 2 starts
// AdminServer and hammers the gather endpoints (/stats /conns /metrics
// /config), each of which posts one task onto the shard's mailbox.
//
// Usage: bench_admin_impact [seconds_per_phase=5] [admin_threads=4]
// Output: one JSON line {phase, work_per_sec, ...} per phase.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "afx/afx.hpp"

using namespace afx;
using namespace std::chrono;

namespace {

std::atomic<std::uint64_t> g_work{0};
std::atomic<std::uint64_t> g_replies{0};
std::atomic<bool> g_hammer{false};

// Minimal blocking GET; the admin endpoint closes after one response.
void hammer_once(std::uint16_t port, const char* path) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    timeval tv{1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        ::close(fd);
        return;
    }
    char req[128];
    int n = std::snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nhost: x\r\n\r\n",
                          path);
    if (::send(fd, req, std::size_t(n), 0) != n) {
        ::close(fd);
        return;
    }
    char buf[8192];
    while (::recv(fd, buf, sizeof buf, 0) > 0) g_replies.fetch_add(1);
    ::close(fd);
}

double measure(int secs, EventManager* em) {
    g_work = 0;
    Stats s0 = em->stats();
    auto t0 = steady_clock::now();
    std::this_thread::sleep_for(seconds(secs));
    double el = duration<double>(steady_clock::now() - t0).count();
    Stats d = em->stats();
    std::fprintf(stderr, "  [shard] iters/s=%.0f mailbox_pops/s=%.0f\n",
                 (d.iterations - s0.iterations) / el,
                 (d.mailbox_pops - s0.mailbox_pops) / el);
    return double(g_work.load()) / el;
}

}  // namespace

int main(int argc, char** argv) {
    int secs = argc > 1 ? std::atoi(argv[1]) : 5;
    int threads = argc > 2 ? std::atoi(argv[2]) : 4;

    auto topo = Topology::detect();
    Runtime rt(topo);
    ThreadConfig cfg;
    // The design claim is about a pinned shard-per-core deployment: the
    // shard owns its core; the admin EM thread and its clients float on the
    // remaining cores and must not steal shard cycles. Pin to the last
    // physical core — core 0 soaks default IRQ/softirq work (incl. the
    // loopback traffic the hammer generates), which would pollute the
    // measurement on a small box.
    auto phys = topo.physical_cores();
    cfg.placement = Placement::Explicit;
    cfg.cores = CoreSet::of({phys.empty() ? 0 : phys.back()});
    cfg.em.wait = WaitStrategy::Spin;  // the thing we must not slow down
    auto g = rt.spawn_group("spin", 1, cfg);

    std::atomic<bool> ready{false};
    g.each([&](EventManager& em) {
        // Self-reposting defer chain: each execution is one unit of shard
        // work. Deferred tasks run at end of iteration, so throughput tracks
        // the shard's loop rate minus whatever else competes for the thread.
        auto tick = std::make_shared<std::function<void()>>();
        *tick = [&em, tick] {
            g_work.fetch_add(1, std::memory_order_relaxed);
            em.defer([tick] { (*tick)(); });
        };
        (*tick)();
        ready = true;
    });
    rt.start();
    while (!ready.load()) std::this_thread::yield();
    std::this_thread::sleep_for(200ms);  // warm the loop

    double base = measure(secs, rt.em(0));
    std::printf("{\"phase\":\"baseline\",\"work_per_sec\":%.0f}\n", base);

    admin::AdminServer admin(rt);
    auto bound = admin.start();
    if (!bound) {
        std::fprintf(stderr, "admin start failed\n");
        rt.shutdown(5s);
        rt.join();
        return 1;
    }
    std::uint16_t aport = bound->port();

    // Phase 2: admin EM exists (its own Block thread) but serves nothing —
    // isolates thread-existence cost from request-serving cost.
    double idle = measure(secs, rt.em(0));
    std::printf(
        "{\"phase\":\"admin_idle\",\"work_per_sec\":%.0f,"
        "\"delta_pct\":%.2f}\n",
        idle, (idle - base) / base * 100.0);

    auto hammer_phase = [&](const char* name, const char* const* paths,
                            std::size_t np) {
        g_hammer = true;
        g_replies = 0;
        std::vector<std::thread> hs;
        for (int i = 0; i < threads; ++i)
            hs.emplace_back([aport, i, paths, np] {
                for (int k = 0; g_hammer.load(); ++k)
                    hammer_once(aport, paths[k % np]);
            });
        double v = measure(secs, rt.em(0));
        g_hammer = false;
        for (auto& t : hs) t.join();
        std::printf(
            "{\"phase\":\"%s\",\"work_per_sec\":%.0f,"
            "\"delta_pct\":%.2f,\"admin_threads\":%d,\"reply_chunks\":%llu}\n",
            name, v, (v - base) / base * 100.0, threads,
            (unsigned long long)g_replies.load());
        return v;
    };

    // Phase 3: hammer /healthz — admin-local, no shard mailbox post. The
    // delta vs baseline is pure CPU/scheduler contention from the admin
    // plane's own threads on a shared box.
    static const char* kLocal[] = {"/healthz"};
    hammer_phase("hammer_local", kLocal, 1);

    // Phase 4: hammer the gather endpoints — each request posts one task to
    // the shard mailbox. Delta vs hammer_local is the true per-request
    // shard-side cost of cross-shard introspection.
    static const char* kGather[] = {"/stats", "/conns", "/metrics", "/config"};
    hammer_phase("hammer_gather", kGather, 4);

    admin.stop();
    rt.shutdown(5s);
    rt.join();
    return 0;
}

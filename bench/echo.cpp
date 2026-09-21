// bench/echo — M8-10 head-to-head: the same echo workload against the epoll
// backend, the io_uring backend, and a hand-written epoll loop (--server
// raw-epoll) so ADR-0002's tripwire can be measured directly: is the
// framework's proactor-over-readiness emulation within 5 % of raw epoll?
//
// In-process: one server EM + load shards, all on loopback. Both sides share
// the host CPU, so absolute numbers reflect server+load together — the point
// is the RELATIVE comparison under an identical workload.
//
// Usage: bench_echo [--server epoll|uring|raw] [--load epoll|uring|auto]
//                   [--conns N] [--duration S] [--warmup S]
//                   [--mode closed|open] [--rate RPS] [--outstanding N]
//                   [--payload B] [--shards N] [--json PATH]
//                   [--listen-port P]

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#ifdef __linux__
#include <sys/epoll.h>
#endif
#include <sys/socket.h>
#include <unistd.h>

#include "afx/afx.hpp"
#include "afx/net/socket.hpp"

using namespace afx;

namespace {

// ---- echo protocol (same wire format as echo_server / afx-load) ------------
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

struct Opts {
    BackendKind server = BackendKind::Auto;  // RawEpoll selects raw mode
    BackendKind load = BackendKind::Auto;
    bool raw_server = false;
    int conns = 8;
    double duration_s = 10.0;
    double warmup_s = 1.0;
    bool open_loop = false;
    double rate = 0.0;
    int outstanding = 1;
    std::size_t payload = 64;
    int shards = 1;
    std::uint16_t port = 0;
    const char* json = nullptr;
};

// ---- raw epoll echo server (ADR-0002 tripwire baseline) --------------------
// Minimal hand-written readiness loop: nonblocking accept, EPOLLIN read,
// buffer, write-back. No framing knowledge — echoes bytes verbatim.
// Linux-only by construction — the whole point is measuring epoll itself.
#ifdef __linux__
void raw_epoll_server(std::uint16_t port, std::atomic<bool>& stop) {
    // SOCK_NONBLOCK: the drain loop relies on accept4 returning EAGAIN.
    int lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0 ||
        ::listen(lfd, 256) < 0) {
        std::fprintf(stderr, "raw server bind failed: %s\n",
                     std::strerror(errno));
        return;
    }
    int ep = ::epoll_create1(0);
    epoll_event ev{.events = EPOLLIN, .data = {.fd = lfd}};
    ::epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);

    std::vector<epoll_event> evs(256);
    std::vector<std::byte> buf(64 << 10);
    while (!stop.load(std::memory_order_relaxed)) {
        int n = ::epoll_wait(ep, evs.data(), int(evs.size()), 100);
        for (int i = 0; i < n; ++i) {
            int fd = evs[i].data.fd;
            if (fd == lfd) {
                for (;;) {
                    int c = ::accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK);
                    if (c < 0) break;
                    epoll_event ce{.events = EPOLLIN, .data = {.fd = c}};
                    ::epoll_ctl(ep, EPOLL_CTL_ADD, c, &ce);
                }
                continue;
            }
            if (evs[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
                continue;
            }
            ssize_t r = ::recv(fd, buf.data(), buf.size(), 0);
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                continue;  // spurious wakeup — keep the connection
            if (r <= 0) {
                ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
                continue;
            }
            // Blocking write-back of what we just read: loopback buffers are
            // deep enough for the driver rates used here; the tripwire cares
            // about syscall/count overhead, not write-path sophistication.
            ssize_t off = 0;
            while (off < r) {
                ssize_t w = ::send(fd, buf.data() + off, std::size_t(r - off),
                                   MSG_NOSIGNAL);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                off += w;
            }
        }
    }
    ::close(lfd);
    ::close(ep);
}
#endif  // __linux__

// ---- framework echo server -------------------------------------------------
// bench_echo_coro compiles this same file with AFX_ECHO_CORO_SERVER: the
// load side and CLI are identical, only the server session shape changes
// (callback batch handler → one coroutine per connection).
#ifdef AFX_ECHO_CORO_SERVER
void framework_server(EventManager& em, std::uint16_t port) {
    ServerConfig cfg;
    cfg.bind = SockAddr::any(port);
    cfg.reuse_port = true;
    auto srv = em.make_coro_server<EchoProto>(
        cfg, [](ConnRef<EchoProto, EventManager> c) -> CoroTask<void> {
            // Frame buffer lives in the coroutine frame: valid across the
            // send suspension, and reused per message (recv batches point
            // into the conn's read buffer — copy out before the next recv).
            std::vector<std::byte> buf;
            while (auto batch = co_await c.recv()) {
                for (auto& m : *batch) {
                    buf.resize(sizeof(EchoHeader) + m.body.size());
                    std::memcpy(buf.data(), &m.header, sizeof(EchoHeader));
                    std::memcpy(buf.data() + sizeof(EchoHeader), m.body.data(),
                                m.body.size());
                    if (co_await c.send(ByteSpan(buf.data(), buf.size())) ==
                        SendResult::Closed)
                        co_return;
                }
            }
        });
    if (!srv) std::fprintf(stderr, "bench_echo: bind failed\n");
}
#else
void framework_server(EventManager& em, std::uint16_t port) {
    Handlers<EchoProto> h;
    h.on_messages = [&](ConnId id, std::span<const EchoMsg> batch) {
        auto* c = Connection<EchoProto, EventManager>::resolve(em, id);
        if (!c) return;
        for (auto& m : batch) {
            ByteSpan parts[2]{
                ByteSpan(reinterpret_cast<const std::byte*>(&m.header),
                         sizeof(m.header)),
                m.body};
            (void)c->send_scatter(std::span<const ByteSpan>(parts, 2));
        }
    };
    ServerConfig cfg;
    cfg.bind = SockAddr::any(port);
    cfg.reuse_port = true;
    auto srv = em.make_server<EchoProto>(cfg, std::move(h));
    if (!srv) std::fprintf(stderr, "bench_echo: bind failed\n");
}
#endif  // AFX_ECHO_CORO_SERVER

// ---- load side (afx-load logic, shared shape) ------------------------------
struct PerConn {
    ConnId id{};
    bool open = false;
    TimerId pacing{};
};

struct Shard {
    EventManager em;
    std::vector<PerConn> conns;
    std::vector<std::byte> req;
    Histogram lat;
    std::uint64_t sends = 0, replies = 0, errors = 0;
    std::uint64_t warmup_end_rt = 0;
    Shard(EventManagerConfig c) : em(std::move(c)) {}
};

std::atomic<bool> g_stop{false};

void put_u64(std::byte* p, std::uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) p[i] = std::byte(v >> (i * 8));
}
std::uint64_t get_u64(const std::byte* p) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= std::uint64_t(p[i]) << (i * 8);
    return v;
}

void send_req(Shard& s, PerConn& pc, std::uint64_t sched_rt) {
    put_u64(s.req.data() + sizeof(EchoHeader), sched_rt);
    if (auto* c = Connection<EchoProto, EventManager>::resolve(s.em, pc.id)) {
        if (c->send(ByteSpan(s.req.data(), s.req.size())) ==
            SendResult::Queued) {
            ++s.sends;
            return;
        }
    }
    ++s.errors;
}

void record_lat(Shard& s, const EchoMsg& m) {
    if (m.body.size() < 8) return;
    std::uint64_t rt = realtime_ns();
    ++s.replies;
    if (rt < s.warmup_end_rt) return;
    std::uint64_t sched = get_u64(m.body.data());
    if (rt > sched) s.lat.record(rt - sched);
}

void arm_pacing(Shard& s, PerConn& pc, Nanos interval) {
    pc.pacing = s.em.every(interval, [&s, &pc, interval](TimerCtx tc) {
        if (g_stop.load(std::memory_order_relaxed) || !pc.open) return;
        std::uint64_t sched =
            realtime_ns() + std::uint64_t(Nanos(tc.scheduled - tc.now).count());
        for (std::uint32_t j = tc.missed + 1; j-- > 0;)
            send_req(s, pc, sched - std::uint64_t(j) * interval.count());
    });
}

void make_client(Shard& s, PerConn& pc, const Opts& o, Nanos interval) {
    Handlers<EchoProto> h;
    h.on_open = [&s, &pc, &o, interval](ConnId id, Peer) {
        pc.id = id;
        pc.open = true;
        if (o.open_loop)
            arm_pacing(s, pc, interval);
        else
            for (int i = 0; i < o.outstanding; ++i)
                send_req(s, pc, realtime_ns());
    };
    h.on_messages = [&s, &pc, &o](ConnId, std::span<const EchoMsg> batch) {
        for (const EchoMsg& m : batch) record_lat(s, m);
        if (g_stop.load(std::memory_order_relaxed) || o.open_loop) return;
        for (std::size_t i = 0; i < batch.size(); ++i)
            send_req(s, pc, realtime_ns());
    };
    h.on_close = [&pc](ConnId, CloseReason) { pc.open = false; };
    h.on_error = [&s](ConnId, Error) { ++s.errors; };

    ClientConfig cc;
    cc.target = Endpoint{"127.0.0.1", o.port};
    cc.connect_timeout = 5s;
    cc.auto_reconnect = false;
    if (!s.em.make_client<EchoProto>(cc, std::move(h)))
        std::fprintf(stderr, "bench_echo: make_client failed\n");
}

const char* kind_name(BackendKind k) {
    switch (k) {
        case BackendKind::Epoll:
            return "epoll";
        case BackendKind::Uring:
            return "uring";
        case BackendKind::Sim:
            return "sim";
        case BackendKind::Kqueue:
            return "kqueue";
        default:
            return "auto";
    }
}

[[noreturn]] void usage(const char* a0) {
    std::fprintf(stderr,
                 "usage: %s [--server epoll|uring|raw] [--load epoll|uring|"
                 "auto] [--conns N] [--duration S] [--warmup S]\n"
                 "       [--mode closed|open] [--rate RPS] [--outstanding N]\n"
                 "       [--payload B] [--shards N] [--listen-port P]\n"
                 "       [--json PATH]\n",
                 a0);
    std::exit(2);
}

Opts parse(int argc, char** argv) {
    Opts o;
    auto backend_of = [&](std::string_view b, bool allow_raw) -> BackendKind {
        if (b == "epoll") return BackendKind::Epoll;
        if (b == "uring") return BackendKind::Uring;
        if (b == "auto") return BackendKind::Auto;
        if (allow_raw && b == "raw") return BackendKind::Kqueue;  // sentinel
        usage(argv[0]);
    };
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        const char* inline_v = nullptr;
        if (auto eq = arg.find('='); eq != std::string_view::npos) {
            inline_v = argv[i] + eq + 1;
            arg = arg.substr(0, eq);
        }
        auto val = [&]() -> const char* {
            if (inline_v) return inline_v;
            if (++i < argc) return argv[i];
            usage(argv[0]);
        };
        std::string_view a = arg;
        if (a == "--server") {
            o.server = backend_of(val(), true);
            o.raw_server = (o.server == BackendKind::Kqueue);
        } else if (a == "--load")
            o.load = backend_of(val(), false);
        else if (a == "--conns")
            o.conns = std::atoi(val());
        else if (a == "--duration")
            o.duration_s = std::atof(val());
        else if (a == "--warmup")
            o.warmup_s = std::atof(val());
        else if (a == "--mode") {
            std::string_view m = val();
            o.open_loop = (m == "open");
            if (!o.open_loop && m != "closed") usage(argv[0]);
        } else if (a == "--rate")
            o.rate = std::atof(val());
        else if (a == "--outstanding")
            o.outstanding = std::atoi(val());
        else if (a == "--payload")
            o.payload = std::size_t(std::atol(val()));
        else if (a == "--shards")
            o.shards = std::atoi(val());
        else if (a == "--listen-port")
            o.port = std::uint16_t(std::atoi(val()));
        else if (a == "--json")
            o.json = val();
        else
            usage(argv[0]);
    }
    if (o.conns < 1 || o.shards < 1 || o.duration_s <= 0 || o.payload < 8 ||
        (o.open_loop && o.rate <= 0))
        usage(argv[0]);
    return o;
}

std::uint16_t pick_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t n = sizeof(a);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &n);
    std::uint16_t p = ntohs(a.sin_port);
    ::close(fd);
    return p;
}

}  // namespace

int main(int argc, char** argv) {
    Opts o = parse(argc, argv);
    if (!o.port) o.port = pick_port();

    // ---- server side
    std::atomic<bool> raw_stop{false};
    std::thread srv_thread;
    std::unique_ptr<EventManager> srv_em;
    if (o.raw_server) {
#ifdef __linux__
        srv_thread = std::thread([&] { raw_epoll_server(o.port, raw_stop); });
#else
        std::fprintf(stderr, "--server raw requires epoll (Linux-only)\n");
        return 2;
#endif
    } else {
        EventManagerConfig sc;
        sc.name = "echo-server";
        sc.backend = o.server;
        srv_em = std::make_unique<EventManager>(std::move(sc));
        framework_server(*srv_em, o.port);
        srv_thread = std::thread([&] { srv_em->run(); });
    }
    std::this_thread::sleep_for(200ms);  // let the listener come up

    // ---- load side
    std::vector<std::unique_ptr<Shard>> shards;
    for (int i = 0; i < o.shards; ++i) {
        EventManagerConfig cfg;
        cfg.name = "echo-load-" + std::to_string(i);
        cfg.backend = o.load;
        shards.push_back(std::make_unique<Shard>(std::move(cfg)));
    }
    Nanos interval(0);
    if (o.open_loop) interval = Nanos(std::int64_t(1e9 * o.conns / o.rate));
    int per_shard = (o.conns + o.shards - 1) / o.shards;
    int made = 0;
    for (auto& sp : shards) {
        int n = std::min(per_shard, o.conns - made);
        sp->conns.resize(std::size_t(n));
        sp->req.resize(sizeof(EchoHeader) + o.payload);
        EchoHeader hdr{0xA5, be16(std::uint16_t(o.payload)), 0x01};
        std::memcpy(sp->req.data(), &hdr, sizeof(hdr));
        for (auto& pc : sp->conns) make_client(*sp, pc, o, interval);
        made += n;
    }
    std::uint64_t warmup_end = realtime_ns() + std::uint64_t(o.warmup_s * 1e9);
    for (auto& sp : shards) sp->warmup_end_rt = warmup_end;

    std::vector<std::thread> threads;
    for (auto& sp : shards) threads.emplace_back([&] { sp->em.run(); });

    TimePoint t0 = SteadyClock::now();
    std::this_thread::sleep_for(
        std::chrono::nanoseconds(std::int64_t(o.duration_s * 1e9)));
    g_stop.store(true);
    for (auto& sp : shards) sp->em.stop();
    for (auto& t : threads) t.join();
    double secs = Nanos(SteadyClock::now() - t0).count() / 1e9;

    if (o.raw_server) raw_stop.store(true);
    if (srv_em) srv_em->stop();
    srv_thread.join();

    Histogram lat;
    std::uint64_t sends = 0, replies = 0, errors = 0;
    for (auto& sp : shards) {
        lat.merge(sp->lat);
        sends += sp->sends;
        replies += sp->replies;
        errors += sp->errors;
    }

    const char* srv_name = o.raw_server ? "raw-epoll" : kind_name(o.server);
    std::printf(
        "bench_echo: server=%s load=%s %d conns %s 2x%.1fs — %llu replies "
        "(%.0f rps) errors=%llu\n",
        srv_name, kind_name(o.load), o.conns, o.open_loop ? "open" : "closed",
        secs, (unsigned long long)replies, replies / secs,
        (unsigned long long)errors);
    std::printf(
        "latency ns: count=%llu min=%llu p50=%llu p90=%llu p99=%llu "
        "p99.9=%llu max=%llu mean=%.0f\n",
        (unsigned long long)lat.count(), (unsigned long long)lat.min(),
        (unsigned long long)lat.percentile(0.50),
        (unsigned long long)lat.percentile(0.90),
        (unsigned long long)lat.percentile(0.99),
        (unsigned long long)lat.percentile(0.999),
        (unsigned long long)lat.max(), lat.mean());

    char head[1024];
    std::snprintf(head, sizeof(head),
                  "{\"benchmark\":\"echo\",\"server_backend\":\"%s\","
                  "\"load_backend\":\"%s\",\"mode\":\"%s\",\"conns\":%d,"
                  "\"payload\":%zu,\"duration_s\":%.3f,\"sends\":%llu,"
                  "\"replies\":%llu,\"errors\":%llu,\"throughput_rps\":%.0f,"
                  "\"latency_ns\":{\"count\":%llu,\"min\":%llu,\"p50\":%llu,"
                  "\"p90\":%llu,\"p99\":%llu,\"p999\":%llu,\"max\":%llu,"
                  "\"mean\":%.0f},\"buckets\":[",
                  srv_name, kind_name(o.load), o.open_loop ? "open" : "closed",
                  o.conns, o.payload, secs, (unsigned long long)sends,
                  (unsigned long long)replies, (unsigned long long)errors,
                  replies / secs, (unsigned long long)lat.count(),
                  (unsigned long long)lat.min(),
                  (unsigned long long)lat.percentile(0.50),
                  (unsigned long long)lat.percentile(0.90),
                  (unsigned long long)lat.percentile(0.99),
                  (unsigned long long)lat.percentile(0.999),
                  (unsigned long long)lat.max(), lat.mean());
    std::string js = head;
    auto& b = lat.buckets();
    for (unsigned i = 0; i < b.size(); ++i) {
        if (!b[i]) continue;
        char e[96];
        std::snprintf(e, sizeof(e), "%s{\"floor_ns\":%llu,\"count\":%llu}",
                      js.back() == '[' ? "" : ",",
                      (unsigned long long)(1ull << i),
                      (unsigned long long)b[i]);
        js += e;
    }
    js += "]}\n";
    std::fputs(js.c_str(), stdout);
    if (o.json) {
        std::ofstream f(o.json);
        if (f) f << js;
    }
    return 0;
}

// tools/afx-load — protocol-aware load generator (M8-09). Speaks the
// echo_server wire format ([magic:1][len:2 BE][type:1] + body); the first 8
// body bytes carry the request's *scheduled* realtime send stamp, echoed back
// verbatim, so latency is measured against intent — coordinated-omission-free
// in open-loop mode.
//
// Usage:
//   afx-load <host> <port> [--conns N] [--duration S] [--warmup S]
//            [--mode closed|open] [--rate RPS] [--outstanding N]
//            [--payload B] [--shards N] [--backend auto|epoll|uring]
//            [--json PATH]
//
// Modes:
//   closed  --outstanding requests in flight per connection (default 1)
//   open    fixed aggregate rate paced by per-conn FixedRate timers; the
//           scheduled instant is derived from the timer's intended expiry so
//           EM-side queueing shows up in the measured latency
//
// Latency uses CLOCK_REALTIME — meaningful cross-machine only with synced
// clocks (ptp/chrony); exact on loopback.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

struct Opts {
    const char* host = "127.0.0.1";
    std::uint16_t port = 0;
    int conns = 1;
    double duration_s = 10.0;
    double warmup_s = 1.0;
    bool open_loop = false;
    double rate = 0.0;        // aggregate req/s (open mode)
    int outstanding = 1;      // closed-mode window per conn
    std::size_t payload = 64; // body bytes; >= 8 for the stamp
    int shards = 1;
    BackendKind backend = BackendKind::Auto;
    const char* json = nullptr;
};

struct PerConn {
    ConnId id{};
    bool open = false;
    std::uint32_t outstanding = 0;
    TimerId pacing{};
};

struct Shard {
    EventManager em;
    std::vector<PerConn> conns;
    std::vector<std::byte> req;  // header + body scratch, ts patched per send
    // Hot-path stats — touched only on this shard's EM thread.
    Histogram lat;
    std::uint64_t sends = 0;
    std::uint64_t replies = 0;
    std::uint64_t errors = 0;
    std::uint64_t opened = 0;
    std::uint64_t closed = 0;
    std::uint64_t warmup_end_rt = 0;  // realtime ns; samples before are dropped

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

// One send: stamp body[0..8) with the scheduled realtime instant.
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
    if (rt < s.warmup_end_rt) {  // warmup window — count but don't record
        ++s.replies;
        return;
    }
    ++s.replies;
    std::uint64_t sched = get_u64(m.body.data());
    if (rt > sched) s.lat.record(rt - sched);
}

// Open-loop pacing: the timer's intended expiry is the scheduled send
// instant. A late EM fire pushes sched into the past — the queueing delay is
// charged to latency instead of being omitted. Coalesced overruns
// (interval < timer_tick) emit their skipped sends batched, each stamped at
// its own intended instant, so the offered rate is preserved even when the
// wheel quantizes the schedule.
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
        ++s.opened;
        if (o.open_loop) {
            arm_pacing(s, pc, interval);
        } else {
            for (int i = 0; i < o.outstanding; ++i) {
                send_req(s, pc, realtime_ns());
                ++pc.outstanding;
            }
        }
    };
    h.on_messages = [&s, &pc, &o](ConnId, std::span<const EchoMsg> batch) {
        for (const EchoMsg& m : batch) record_lat(s, m);
        if (g_stop.load(std::memory_order_relaxed)) return;
        if (o.open_loop) return;  // pacing timer owns the send schedule
        // Closed loop: each reply refills the window.
        pc.outstanding = batch.size() >= pc.outstanding
                             ? 0
                             : pc.outstanding - std::uint32_t(batch.size());
        for (std::size_t i = 0; i < batch.size(); ++i) {
            send_req(s, pc, realtime_ns());
            ++pc.outstanding;
        }
    };
    h.on_close = [&s, &pc](ConnId, CloseReason) {
        if (pc.open) ++s.closed;
        pc.open = false;
        pc.outstanding = 0;
    };
    h.on_error = [&s](ConnId, Error) { ++s.errors; };

    ClientConfig cc;
    cc.target = Endpoint{o.host, o.port};
    cc.connect_timeout = 5s;
    cc.auto_reconnect = false;
    auto cli = s.em.make_client<EchoProto>(cc, std::move(h));
    if (!cli) std::fprintf(stderr, "afx-load: make_client failed\n");
}

[[noreturn]] void usage(const char* arg0) {
    std::fprintf(
        stderr,
        "usage: %s <host> <port> [--conns N] [--duration S] [--warmup S]\n"
        "       [--mode closed|open] [--rate RPS] [--outstanding N]\n"
        "       [--payload B] [--shards N] [--backend auto|epoll|uring]\n"
        "       [--json PATH]\n",
        arg0);
    std::exit(2);
}

Opts parse(int argc, char** argv) {
    Opts o;
    if (argc < 3) usage(argv[0]);
    o.host = argv[1];
    o.port = std::uint16_t(std::atoi(argv[2]));
    for (int i = 3; i < argc; ++i) {
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
        if (a == "--conns") o.conns = std::atoi(val());
        else if (a == "--duration") o.duration_s = std::atof(val());
        else if (a == "--warmup") o.warmup_s = std::atof(val());
        else if (a == "--mode") {
            std::string_view m = val();
            o.open_loop = (m == "open");
            if (!o.open_loop && m != "closed") usage(argv[0]);
        } else if (a == "--rate") o.rate = std::atof(val());
        else if (a == "--outstanding") o.outstanding = std::atoi(val());
        else if (a == "--payload") o.payload = std::size_t(std::atol(val()));
        else if (a == "--shards") o.shards = std::atoi(val());
        else if (a == "--backend") {
            std::string_view b = val();
            if (b == "epoll") o.backend = BackendKind::Epoll;
            else if (b == "uring") o.backend = BackendKind::Uring;
            else if (b != "auto") usage(argv[0]);
        } else if (a == "--json") o.json = val();
        else usage(argv[0]);
    }
    if (!o.port || o.conns < 1 || o.shards < 1 || o.duration_s <= 0 ||
        o.payload < 8 || (o.open_loop && o.rate <= 0))
        usage(argv[0]);
    return o;
}

std::string to_json(const Opts& o, const Histogram& lat,
                    std::uint64_t sends, std::uint64_t replies, double secs) {
    char head[1024];
    std::snprintf(head, sizeof(head),
                  "{\"tool\":\"afx-load\",\"mode\":\"%s\",\"conns\":%d,"
                  "\"shards\":%d,\"payload\":%zu,\"duration_s\":%.3f,"
                  "\"sends\":%llu,\"replies\":%llu,"
                  "\"throughput_rps\":%.0f,"
                  "\"latency_ns\":{\"count\":%llu,\"min\":%llu,\"p50\":%llu,"
                  "\"p90\":%llu,\"p99\":%llu,\"p999\":%llu,\"max\":%llu,"
                  "\"mean\":%.0f},\"buckets\":[",
                  o.open_loop ? "open" : "closed", o.conns, o.shards,
                  o.payload, secs, (unsigned long long)sends,
                  (unsigned long long)replies,
                  secs > 0 ? replies / secs : 0.0,
                  (unsigned long long)lat.count(),
                  (unsigned long long)lat.min(),
                  (unsigned long long)lat.percentile(0.50),
                  (unsigned long long)lat.percentile(0.90),
                  (unsigned long long)lat.percentile(0.99),
                  (unsigned long long)lat.percentile(0.999),
                  (unsigned long long)lat.max(), lat.mean());
    std::string out = head;
    auto& b = lat.buckets();
    for (unsigned i = 0; i < b.size(); ++i) {
        if (!b[i]) continue;
        char e[96];
        std::snprintf(e, sizeof(e), "%s{\"floor_ns\":%llu,\"count\":%llu}",
                      out.back() == '[' ? "" : ",",
                      (unsigned long long)(1ull << i),
                      (unsigned long long)b[i]);
        out += e;
    }
    out += "]}\n";
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    Opts o = parse(argc, argv);

    std::vector<std::unique_ptr<Shard>> shards;
    shards.reserve(std::size_t(o.shards));
    for (int i = 0; i < o.shards; ++i) {
        EventManagerConfig cfg;
        cfg.name = "afx-load-" + std::to_string(i);
        cfg.backend = o.backend;
        shards.push_back(std::make_unique<Shard>(std::move(cfg)));
    }

    // Per-conn pacing interval: the aggregate rate is spread evenly.
    Nanos interval(0);
    if (o.open_loop)
        interval = Nanos(std::int64_t(1e9 * o.conns / o.rate));

    // Open-loop needs per-conn pacing, so PerConn must stay put: size the
    // vector up front and never reallocate while handlers hold pointers.
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

    std::uint64_t warmup_end =
        realtime_ns() + std::uint64_t(o.warmup_s * 1e9);
    for (auto& sp : shards) sp->warmup_end_rt = warmup_end;

    std::vector<std::thread> threads;
    threads.reserve(shards.size());
    for (auto& sp : shards) threads.emplace_back([&] { sp->em.run(); });

    TimePoint t0 = SteadyClock::now();
    std::this_thread::sleep_for(
        std::chrono::nanoseconds(std::int64_t(o.duration_s * 1e9)));
    g_stop.store(true);
    for (auto& sp : shards) sp->em.stop();
    for (auto& t : threads) t.join();
    double secs = Nanos(SteadyClock::now() - t0).count() / 1e9;

    Histogram lat;
    std::uint64_t sends = 0, replies = 0, errors = 0, opened = 0, closed = 0;
    for (auto& sp : shards) {
        lat.merge(sp->lat);
        sends += sp->sends;
        replies += sp->replies;
        errors += sp->errors;
        opened += sp->opened;
        closed += sp->closed;
    }

    std::printf(
        "afx-load: %d conns x %d shards, %s mode, %.1fs — %llu replies "
        "(%.0f rps), opened=%llu closed=%llu errors=%llu\n",
        o.conns, o.shards, o.open_loop ? "open" : "closed", secs,
        (unsigned long long)replies, secs > 0 ? replies / secs : 0.0,
        (unsigned long long)opened, (unsigned long long)closed,
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

    std::string js = to_json(o, lat, sends, replies, secs);
    std::fputs(js.c_str(), stdout);
    if (o.json) {
        std::ofstream f(o.json);
        if (f)
            f << js;
        else
            std::fprintf(stderr, "afx-load: cannot open %s\n", o.json);
    }
    return 0;
}

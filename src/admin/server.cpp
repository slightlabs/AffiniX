#include "afx/admin/server.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "afx/core/event_manager.hpp"
#include "afx/core/flight_recorder.hpp"
#include "afx/core/timer_wheel.hpp"
#include "afx/net/connection.hpp"
#include "afx/net/tcp_server.hpp"
#include "afx/sys/log.hpp"

#ifndef AFX_VERSION
#define AFX_VERSION "dev"
#endif

namespace afx::admin {
namespace {

using AdminEM = EventManager;
using Msg = HttpRequest;

// --------------------------------------------------------------- queries

std::string_view qget(std::string_view query, std::string_view key) {
    for (std::size_t pos = 0; pos < query.size();) {
        auto amp = query.find('&', pos);
        auto kv = query.substr(pos, amp == std::string_view::npos
                                        ? std::string_view::npos
                                        : amp - pos);
        if (auto eq = kv.find('=');
            eq != std::string_view::npos && kv.substr(0, eq) == key)
            return kv.substr(eq + 1);
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return {};
}

long long qnum(std::string_view q, std::string_view k, long long dflt = 0) {
    auto v = qget(q, k);
    return v.empty() ? dflt : std::atoll(std::string(v).c_str());
}

// ------------------------------------------------------------ formatting

void jesc(std::string& out, std::string_view s) {
    for (char c : s) {
        if (c == '"' || c == '\\' || c == '\n') out += '\\';
        out += c == '\n' ? 'n' : c;
    }
}

const char* conn_state_name(std::uint8_t s) {
    switch (ConnState(s)) {
        case ConnState::Connecting:
            return "connecting";
        case ConnState::Established:
            return "established";
        case ConnState::ShutdownWrite:
            return "shutdown_write";
        case ConnState::Closing:
            return "closing";
        case ConnState::Closed:
            return "closed";
    }
    return "?";
}

// -------------------------------------------------------------- gathering

// One admin request may need data from every shard. A Gather fans out one
// mailbox task per shard; each runs on its own EM thread, fills its slot,
// then posts back to the admin EM. A deadline on the admin EM bounds the
// wait so a wedged shard can't stall the endpoint (§20: the admin plane
// never blocks the dataplane — and never hangs on it either).
enum class GatherKind { Stats, Metrics, Conns, Flight, Config };

struct ShardSnap {
    std::string name;
    Stats stats;
    LatencyMetrics lat;
    std::vector<ConnInfo> conns;
    std::vector<FlightRecord> flight;
    std::string cfg;  // Config kind: rendered EventManagerConfig
    bool ok = false;
};

struct Gather {
    GatherKind kind;
    ConnId reply_conn;
    std::atomic<int> arrived{0};
    std::vector<ShardSnap> snaps;
    TimerId deadline{};
};

void respond_text(AdminEM& em, ConnId id, const std::string& status,
                  const std::string& ctype, std::string_view body) {
    auto* c = Connection<HttpProto, AdminEM>::resolve(em, id);
    if (!c) return;
    std::string r = "HTTP/1.1 " + status + "\r\ncontent-type: " + ctype +
                    "\r\ncontent-length: " + std::to_string(body.size()) +
                    "\r\nconnection: close\r\n\r\n";
    r.append(body);
    (void)c->send(std::as_bytes(std::span{std::string_view(r)}));
    c->shutdown_write();
}

std::string stats_json(const ShardSnap& s) {
    std::string o = "{\"name\":\"";
    jesc(o, s.name);
    char b[1024];
    std::snprintf(
        b, sizeof b,
        "\",\"ok\":%s,\"iterations\":%llu,\"idle_iterations\":%llu,"
        "\"idle_ratio\":%.4f,\"spins\":%llu,\"blocks\":%llu,"
        "\"wakeups\":%llu,\"accepts\":%llu,\"conns_opened\":%llu,"
        "\"conns_closed\":%llu,\"conns_live\":%llu,\"bytes_in\":%llu,"
        "\"bytes_out\":%llu,\"msgs_in\":%llu,\"msgs_out\":%llu,"
        "\"frame_errors\":%llu,\"write_hwm_hits\":%llu,"
        "\"write_drops\":%llu,\"mailbox_pushes\":%llu,"
        "\"mailbox_pops\":%llu,\"mailbox_full\":%llu,"
        "\"timers_armed\":%llu,\"timers_fired\":%llu,"
        "\"timers_cancelled\":%llu,\"timer_coalesced\":%llu,"
        "\"deadline_expired_before_start\":%llu,"
        "\"deadline_expired_in_flight\":%llu,\"callback_errors\":%llu,"
        "\"internal_errors\":%llu,\"coro_spawned\":%llu,"
        "\"coro_completed\":%llu,\"coro_heap_frames\":%llu}",
        s.ok ? "true" : "false", (unsigned long long)s.stats.iterations,
        (unsigned long long)s.stats.idle_iterations, s.stats.idle_ratio(),
        (unsigned long long)s.stats.spins, (unsigned long long)s.stats.blocks,
        (unsigned long long)s.stats.wakeups,
        (unsigned long long)s.stats.accepts,
        (unsigned long long)s.stats.conns_opened,
        (unsigned long long)s.stats.conns_closed,
        (unsigned long long)(s.stats.conns_opened - s.stats.conns_closed),
        (unsigned long long)s.stats.bytes_in,
        (unsigned long long)s.stats.bytes_out,
        (unsigned long long)s.stats.msgs_in,
        (unsigned long long)s.stats.msgs_out,
        (unsigned long long)s.stats.frame_errors,
        (unsigned long long)s.stats.write_hwm_hits,
        (unsigned long long)s.stats.write_drops,
        (unsigned long long)s.stats.mailbox_pushes,
        (unsigned long long)s.stats.mailbox_pops,
        (unsigned long long)s.stats.mailbox_full,
        (unsigned long long)s.stats.timers_armed,
        (unsigned long long)s.stats.timers_fired,
        (unsigned long long)s.stats.timers_cancelled,
        (unsigned long long)s.stats.timer_coalesced,
        (unsigned long long)s.stats.deadline_expired_before_start,
        (unsigned long long)s.stats.deadline_expired_in_flight,
        (unsigned long long)s.stats.callback_errors,
        (unsigned long long)s.stats.internal_errors,
        (unsigned long long)s.stats.coro_spawned,
        (unsigned long long)s.stats.coro_completed,
        (unsigned long long)s.stats.coro_heap_frames);
    o += b;
    return o;
}

// Histogram → cumulative Prometheus buckets (le = upper bound in ns; the
// log2 buckets map to le=2^b; a final +Inf bucket carries count()).
void histo_prom(std::string& o, const char* name, const char* labels,
                const Histogram& h) {
    std::uint64_t cum = 0;
    for (unsigned bkt = 0; bkt < h.buckets().size(); ++bkt) {
        cum += h.buckets()[bkt];
        if (!h.buckets()[bkt]) continue;
        char l[256];
        std::snprintf(l, sizeof l, "%s{%s,le=\"%llu\"} %llu\n", name, labels,
                      (unsigned long long)(std::uint64_t(2) << bkt),
                      (unsigned long long)cum);
        o += l;
    }
    char l[256];
    std::snprintf(l, sizeof l, "%s{%s,le=\"+Inf\"} %llu\n", name, labels,
                  (unsigned long long)h.count());
    o += l;
    std::snprintf(l, sizeof l, "%s_sum{%s} %.0f\n%s_count{%s} %llu\n", name,
                  labels, h.mean() * double(h.count()), name, labels,
                  (unsigned long long)h.count());
    o += l;
}

std::string metrics_prom(const ShardSnap& s, int shard) {
    char lbl[32];
    std::snprintf(lbl, sizeof lbl, "shard=\"%d\"", shard);
    char b[3072];
    std::snprintf(
        b, sizeof b,
        "afx_iterations_total{%s} %llu\nafx_idle_iterations_total{%s} %llu\n"
        "afx_spins_total{%s} %llu\nafx_blocks_total{%s} %llu\n"
        "afx_wakeups_total{%s} %llu\nafx_accepts_total{%s} %llu\n"
        "afx_conns_opened_total{%s} %llu\nafx_conns_closed_total{%s} %llu\n"
        "afx_conns_live{%s} %llu\nafx_bytes_in_total{%s} %llu\n"
        "afx_bytes_out_total{%s} %llu\nafx_msgs_in_total{%s} %llu\n"
        "afx_msgs_out_total{%s} %llu\nafx_frame_errors_total{%s} %llu\n"
        "afx_write_hwm_hits_total{%s} %llu\nafx_write_drops_total{%s} %llu\n"
        "afx_mailbox_pushes_total{%s} %llu\nafx_mailbox_pops_total{%s} %llu\n"
        "afx_mailbox_full_total{%s} %llu\nafx_timers_armed_total{%s} %llu\n"
        "afx_timers_fired_total{%s} %llu\n"
        "afx_timers_cancelled_total{%s} %llu\n"
        "afx_timer_coalesced_total{%s} %llu\n"
        "afx_groups_cancelled_total{%s} %llu\n"
        "afx_deadline_expired_before_start_total{%s} %llu\n"
        "afx_deadline_expired_in_flight_total{%s} %llu\n"
        "afx_idle_ratio{%s} %.4f\n"
        "afx_callback_errors_total{%s} %llu\n"
        "afx_internal_errors_total{%s} %llu\n"
        "afx_coro_spawned_total{%s} %llu\n"
        "afx_coro_completed_total{%s} %llu\n"
        "afx_coro_heap_frames_total{%s} %llu\n",
        lbl, (unsigned long long)s.stats.iterations, lbl,
        (unsigned long long)s.stats.idle_iterations, lbl,
        (unsigned long long)s.stats.spins, lbl,
        (unsigned long long)s.stats.blocks, lbl,
        (unsigned long long)s.stats.wakeups, lbl,
        (unsigned long long)s.stats.accepts, lbl,
        (unsigned long long)s.stats.conns_opened, lbl,
        (unsigned long long)s.stats.conns_closed, lbl,
        (unsigned long long)(s.stats.conns_opened - s.stats.conns_closed), lbl,
        (unsigned long long)s.stats.bytes_in, lbl,
        (unsigned long long)s.stats.bytes_out, lbl,
        (unsigned long long)s.stats.msgs_in, lbl,
        (unsigned long long)s.stats.msgs_out, lbl,
        (unsigned long long)s.stats.frame_errors, lbl,
        (unsigned long long)s.stats.write_hwm_hits, lbl,
        (unsigned long long)s.stats.write_drops, lbl,
        (unsigned long long)s.stats.mailbox_pushes, lbl,
        (unsigned long long)s.stats.mailbox_pops, lbl,
        (unsigned long long)s.stats.mailbox_full, lbl,
        (unsigned long long)s.stats.timers_armed, lbl,
        (unsigned long long)s.stats.timers_fired, lbl,
        (unsigned long long)s.stats.timers_cancelled, lbl,
        (unsigned long long)s.stats.timer_coalesced, lbl,
        (unsigned long long)s.stats.groups_cancelled, lbl,
        (unsigned long long)s.stats.deadline_expired_before_start, lbl,
        (unsigned long long)s.stats.deadline_expired_in_flight, lbl,
        s.stats.idle_ratio(), lbl, (unsigned long long)s.stats.callback_errors,
        lbl, (unsigned long long)s.stats.internal_errors, lbl,
        (unsigned long long)s.stats.coro_spawned, lbl,
        (unsigned long long)s.stats.coro_completed, lbl,
        (unsigned long long)s.stats.coro_heap_frames);
    std::string o(b);
    histo_prom(o, "afx_iteration_ns", lbl, s.lat.iteration_ns);
    histo_prom(o, "afx_timer_lateness_ns", lbl, s.lat.timer_lateness_ns);
    histo_prom(o, "afx_mailbox_queue_ns", lbl, s.lat.mailbox_queue_ns);
    histo_prom(o, "afx_recv_to_handler_ns", lbl, s.lat.recv_to_handler_ns);
    histo_prom(o, "afx_write_queue_depth", lbl, s.lat.write_queue_depth);
    histo_prom(o, "afx_deadline_headroom_ns", lbl, s.lat.deadline_headroom_ns);
    histo_prom(o, "afx_nic_to_kernel_ns", lbl, s.lat.nic_to_kernel_ns);
    histo_prom(o, "afx_kernel_to_dequeue_ns", lbl, s.lat.kernel_to_dequeue_ns);
    histo_prom(o, "afx_dequeue_to_handler_ns", lbl,
               s.lat.dequeue_to_handler_ns);
    // §21 per-stage durations — empty unless the EM's profile_stages is set.
    for (std::size_t st = 0; st < LatencyMetrics::kStageCount; ++st) {
        char sl[48];
        std::snprintf(sl, sizeof sl, "%s,stage=\"%s\"", lbl,
                      LatencyMetrics::kStageNames[st]);
        histo_prom(o, "afx_stage_ns", sl, s.lat.stage_ns[st]);
    }
    return o;
}

std::string conns_json(const ShardSnap& s) {
    std::string o;
    for (const auto& ci : s.conns) {
        char b[256];
        std::snprintf(b, sizeof b, "{\"id\":%u,\"gen\":%u,\"role\":\"", ci.id,
                      ci.gen);
        o += b;
        jesc(o, ci.role);
        o += "\",\"source\":\"";
        jesc(o, ci.name);
        o += "\",\"peer\":\"";
        jesc(o, ci.peer);
        std::snprintf(b, sizeof b,
                      "\",\"state\":\"%s\",\"queued_bytes\":%llu,"
                      "\"open_ns\":%llu},",
                      conn_state_name(ci.state),
                      (unsigned long long)ci.queued_write_bytes,
                      (unsigned long long)ci.open_ns);
        o += b;
    }
    if (!o.empty() && o.back() == ',') o.pop_back();
    return '[' + o + ']';
}

// Binary flight dump: per shard a 16B header {magic[8], u32 shard, u32 n}
// then n raw FlightRecords — the format afx-flight merge consumes.
std::vector<std::byte> flight_bin(const Gather& g) {
    std::size_t total = 0;
    for (auto& s : g.snaps) total += s.flight.size();
    std::vector<std::byte> buf;
    buf.reserve(16 * g.snaps.size() + total * sizeof(FlightRecord));
    auto push = [&](const void* p, std::size_t n) {
        auto* b = static_cast<const std::byte*>(p);
        buf.insert(buf.end(), b, b + n);
    };
    std::uint32_t i = 0;
    for (auto& s : g.snaps) {
        struct __attribute__((packed)) H {
            char magic[8];
            std::uint32_t shard, n;
        } h{{'A', 'F', 'X', 'F', 'L', 'T', '0', '1'},
            i++,
            std::uint32_t(s.flight.size())};
        push(&h, sizeof h);
        if (!s.flight.empty())
            push(s.flight.data(), s.flight.size() * sizeof(FlightRecord));
    }
    return buf;
}

void respond_bin(AdminEM& em, ConnId id, std::span<const std::byte> body) {
    auto* c = Connection<HttpProto, AdminEM>::resolve(em, id);
    if (!c) return;
    std::string r =
        "HTTP/1.1 200 OK\r\ncontent-type: application/octet-"
        "stream\r\ncontent-length: " +
        std::to_string(body.size()) + "\r\nconnection: close\r\n\r\n";
    (void)c->send(std::as_bytes(std::span{std::string_view(r)}));
    if (!body.empty()) (void)c->send(body);
    c->shutdown_write();
}

// --------------------------------------------------------- gather plumbing

void gather_finish(AdminEM& em, const std::shared_ptr<Gather>& g,
                   bool timed_out) {
    if (g->deadline.valid()) {
        em.cancel(g->deadline);
        g->deadline = TimerId{};
    } else if (!timed_out) {
        return;  // already finished (double-arrive guard)
    }
    switch (g->kind) {
        case GatherKind::Stats: {
            std::string o = "{\"timeout\":";
            o += timed_out ? "true" : "false";
            o += ",\"shards\":[";
            for (auto& s : g->snaps) o += stats_json(s) + ',';
            if (o.back() == ',') o.pop_back();
            o += "]}";
            respond_text(em, g->reply_conn, "200 OK", "application/json", o);
            break;
        }
        case GatherKind::Metrics: {
            std::string o;
            for (int i = 0; i < (int)g->snaps.size(); ++i)
                if (g->snaps[i].ok) o += metrics_prom(g->snaps[i], i);
            respond_text(em, g->reply_conn, "200 OK",
                         "text/plain; version=0.0.4", o);
            break;
        }
        case GatherKind::Conns: {
            std::string o = "[";
            for (auto& s : g->snaps) o += conns_json(s) + ',';
            if (o.size() > 1) o.pop_back();
            o += ']';
            respond_text(em, g->reply_conn, "200 OK", "application/json", o);
            break;
        }
        case GatherKind::Flight:
            respond_bin(em, g->reply_conn, flight_bin(*g));
            break;
        case GatherKind::Config: {
            std::string o = "[";
            for (auto& s : g->snaps) {
                o += "{\"name\":\"";
                jesc(o, s.name);
                o += "\",\"ok\":";
                o += s.ok ? "true" : "false";
                if (s.ok) {
                    o += ",\"config\":\"";
                    jesc(o, s.cfg);
                    o += '"';
                }
                o += "},";
            }
            if (o.back() == ',') o.pop_back();
            o += ']';
            respond_text(em, g->reply_conn, "200 OK", "application/json", o);
            break;
        }
    }
}

void gather_arrive(AdminEM& em, const std::shared_ptr<Gather>& g) {
    if (g->arrived.fetch_add(1) + 1 == (int)g->snaps.size())
        gather_finish(em, g, false);
}

// Runs on the admin EM thread. Fans out to all shards.
void gather_start(AdminEM& em, Runtime& rt, Duration timeout, GatherKind k,
                  ConnId reply) {
    auto g = std::make_shared<Gather>();
    g->kind = k;
    g->reply_conn = reply;
    g->snaps.resize(rt.shards());
    Mailbox self = em.mailbox();
    AdminEM* aem = &em;
    for (std::size_t i = 0; i < rt.shards(); ++i) {
        auto* e = rt.em(i);
        if (!e) {
            ++g->arrived;
            continue;
        }
        auto res = e->mailbox().post([&rt, e, g, self, aem, i] {
            auto& s = g->snaps[i];
            s.name = rt.shard_info(i).name;
            // Only copy what the endpoint needs — LatencyMetrics is
            // histograms-of-histograms and a blind copy is the dominant
            // gather cost on a busy shard.
            if (g->kind == GatherKind::Stats || g->kind == GatherKind::Metrics)
                s.stats = e->stats();
            if (g->kind == GatherKind::Metrics) s.lat = e->latency();
            switch (g->kind) {
                case GatherKind::Conns:
                    e->enumerate_conns(s.conns);
                    break;
                case GatherKind::Flight: {
                    s.flight.resize(FlightRecorder::kCapacity);
                    s.flight.resize(e->recorder().snapshot(s.flight.data(),
                                                           s.flight.size()));
                    break;
                }
                case GatherKind::Config: {
                    const auto& c = e->config();
                    char b[256];
                    std::snprintf(
                        b, sizeof b,
                        "wait=%s spin_budget_ns=%lld timer_tick_ns=%lld "
                        "stall_threshold_ns=%lld arena_bytes=%zu "
                        "mailbox_capacity=%zu chaos=%s profile_stages=%s",
                        c.wait == WaitStrategy::Block  ? "block"
                        : c.wait == WaitStrategy::Spin ? "spin"
                                                       : "spin_then_block",
                        (long long)c.spin_budget.count(),
                        (long long)c.timer_tick.count(),
                        (long long)c.stall_threshold.count(),
                        c.memory.arena_bytes, c.mailbox_capacity,
                        e->chaos_enabled() ? "on" : "off",
                        c.profile_stages ? "on" : "off");
                    s.cfg = b;
                    break;
                }
                default:
                    break;
            }
            s.ok = true;
            if (self.post([g, aem] { gather_arrive(*aem, g); }) !=
                PostResult::Ok)
                ++g->arrived;
        });
        if (res != PostResult::Ok) ++g->arrived;
    }
    if ((int)g->snaps.size() == g->arrived) {
        gather_finish(em, g, false);
        return;
    }
    g->deadline =
        em.after(timeout, [g, aem](TimerCtx) { gather_finish(*aem, g, true); });
}

// -------------------------------------------------------------- toggles

// Post a runtime toggle to shard i (or all shards when i < 0), then reply.
template <class F>
void for_each_shard(AdminEM& em, Runtime& rt, long shard, ConnId reply,
                    F&& apply) {
    int done = 0, target = 0;
    for (std::size_t i = 0; i < rt.shards(); ++i) {
        if (shard >= 0 && (long)i != shard) continue;
        auto* e = rt.em(i);
        if (!e) continue;
        ++target;
        // `apply` is a reference into route()'s frame — copy it into the
        // posted task; by the time the shard runs it, route() is gone.
        if (e->mailbox().post([apply, e] { apply(*e); }) == PostResult::Ok)
            ++done;
    }
    char b[128];
    std::snprintf(b, sizeof b, "{\"applied\":%d,\"targeted\":%d}", done,
                  target);
    respond_text(em, reply, "200 OK", "application/json", b);
}

// ---------------------------------------------------------------- routing

struct AdminCtx {
    Runtime& rt;
    Duration timeout;
};

void route(AdminEM& em, AdminCtx& ctx, const Msg& r, ConnId id) {
    // Every response carries Connection: close — one request per conn.
    // Anything pipelined behind it is dropped once we've answered.
    auto* c = Connection<HttpProto, AdminEM>::resolve(em, id);
    if (!c || c->state() != ConnState::Established) return;

    const bool get = r.method == "GET";
    const bool post = r.method == "POST";
    auto& rt = ctx.rt;

    if (get && r.path == "/healthz") {
        respond_text(em, id, "200 OK", "text/plain", "ok\n");
    } else if (get && r.path == "/version") {
        char b[256];
        std::snprintf(b, sizeof b,
                      "{\"version\":\"%s\",\"build\":\"%s %s\","
                      "\"chaos_compiled\":%s}",
                      AFX_VERSION, __DATE__, __TIME__,
#ifdef AFX_DEBUG_CHAOS
                      "true"
#else
                      "false"
#endif
        );
        respond_text(em, id, "200 OK", "application/json", b);
    } else if (get && r.path == "/stats") {
        gather_start(em, rt, ctx.timeout, GatherKind::Stats, id);
    } else if (get && r.path == "/metrics") {
        gather_start(em, rt, ctx.timeout, GatherKind::Metrics, id);
    } else if (get && r.path == "/conns") {
        gather_start(em, rt, ctx.timeout, GatherKind::Conns, id);
    } else if (get && r.path == "/flight") {
        gather_start(em, rt, ctx.timeout, GatherKind::Flight, id);
    } else if (get && r.path == "/config") {
        gather_start(em, rt, ctx.timeout, GatherKind::Config, id);
    } else if (get && r.path == "/placement") {
        // ShardInfo is Runtime-owned and thread-safe — no gather needed.
        std::string o = "[";
        for (std::size_t i = 0; i < rt.shards(); ++i) {
            auto si = rt.shard_info(i);
            o += "{\"name\":\"";
            jesc(o, si.name);
            char b[256];
            std::snprintf(b, sizeof b,
                          "\",\"requested_core\":%d,\"bound_core\":%d,"
                          "\"numa_node\":%d,\"running\":%s,\"failed\":%s},",
                          si.requested_core, si.bound_core, si.numa_node,
                          si.running ? "true" : "false",
                          si.failed ? "true" : "false");
            o += b;
        }
        if (o.size() > 1) o.pop_back();
        o += ']';
        respond_text(em, id, "200 OK", "application/json", o);
    } else if (post && r.path == "/admin/stall_threshold") {
        long long ns = qnum(r.query, "ns");
        long shard = qnum(r.query, "shard", -1);
        for_each_shard(em, rt, shard, id,
                       [ns](AdminEM& e) { e.set_stall_threshold(Nanos(ns)); });
    } else if (post && r.path == "/admin/chaos") {
        bool on = qnum(r.query, "on", 0) != 0;
        long shard = qnum(r.query, "shard", -1);
        for_each_shard(em, rt, shard, id,
                       [on](AdminEM& e) { e.set_chaos(on); });
    } else if (post && r.path == "/admin/log_level") {
        auto lv = qget(r.query, "level");
        LogLevel l = lv == "debug"   ? LogLevel::Debug
                     : lv == "warn"  ? LogLevel::Warn
                     : lv == "error" ? LogLevel::Error
                                     : LogLevel::Info;
        set_log_level(l);
        respond_text(em, id, "200 OK", "application/json", "{\"applied\":1}");
    } else if (get && r.path == "/") {
        respond_text(em, id, "200 OK", "text/plain",
                     "GET /healthz /version /stats /metrics /conns "
                     "/placement /config /flight\n"
                     "POST /admin/stall_threshold?ns=N[&shard=i]\n"
                     "POST /admin/chaos?on=0|1[&shard=i]\n"
                     "POST /admin/log_level?level=debug|info|warn|error\n");
    } else {
        respond_text(em, id, "404 Not Found", "text/plain", "not found\n");
    }
}

}  // namespace

// ------------------------------------------------------------------ server

Result<SockAddr> AdminServer::start() {
    if (th_.joinable()) return make_error(ErrorCategory::Config, Err::Invalid);
    th_ = std::thread([this] { thread_main(); });
    for (int i = 0; i < 2000 && !ready_.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(1ms);
    if (!ready_.load(std::memory_order_acquire)) {
        stop();
        return make_error(ErrorCategory::Internal, Err::Expired);
    }
    if (em_.load() == nullptr) return Result<SockAddr>(start_err_);
    return bound_;
}

void AdminServer::stop() noexcept {
    stop_req_.store(true, std::memory_order_release);
    if (auto* e = em_.load()) e->stop();  // thread-safe, wakes the backend
    if (th_.joinable()) th_.join();
    em_.store(nullptr);
}

void AdminServer::thread_main() {
    EventManagerConfig cfg;
    cfg.wait = WaitStrategy::Block;   // §20: idle admin costs the dataplane
    cfg.spin_budget = Nanos::zero();  // nothing when it isn't serving
    cfg.memory.arena_bytes = 0;       // admin EM doesn't need the slab arena
    cfg.mailbox_capacity = 256;
    cfg.uring_sqpoll = false;  // Block-mode EM: sqpoll buys nothing here

    EventManager em(cfg);
    em_.store(&em, std::memory_order_release);
    AdminCtx ctx{rt_, cfg_.gather_timeout};

    Handlers<HttpProto> h;
    h.on_messages = [&em, &ctx](ConnId id, std::span<const Msg> batch) {
        for (const auto& r : batch) route(em, ctx, r, id);
    };
    auto srv = em.make_server<HttpProto>(
        ServerConfig{.bind = cfg_.bind,
                     .backlog = cfg_.backlog,
                     .reuse_port = false,
                     .idle_read_timeout = cfg_.request_timeout},
        std::move(h));
    if (!srv) {
        start_err_ = srv.error();
        em_.store(nullptr, std::memory_order_release);
        ready_.store(true, std::memory_order_release);
        return;
    }
    bound_ = (*srv)->bound_addr();
    if (stop_req_.load(std::memory_order_acquire)) em.stop();
    ready_.store(true, std::memory_order_release);
    em.run();
    em_.store(nullptr, std::memory_order_release);
}

}  // namespace afx::admin

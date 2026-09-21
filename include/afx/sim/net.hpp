#pragma once

// sim/net.hpp — SimNet: the virtual network fabric for deterministic
// simulation (DESIGN.md §22.3, M10-01/04). SimBackends attach to one SimNet;
// submits become scheduled events instead of instant completions:
//
//   submit_connect(u, fd, addr) → resolves the listener by port, wires a
//       link between the client fd and a fresh server-side peer fd, then
//       schedules {ConnectDone on the client} + {Accept on the server}.
//   submit_sendv(u, fd, bytes) → schedules a Bytes event against the peer
//       end of the fd's link, subject to the FaultProfile.
//   detach(fd) → schedules PeerFin at the link's far end, then forgets it.
//
// All ordering is by (due, seq) — seq is a counter drawn at enqueue — so a
// given seed produces one exact interleaving (M10-06).
//
// Not a hot path: events own their payloads. EM-thread == sim-thread.

#include <queue>
#include <random>
#include <unordered_map>
#include <vector>

#include "afx/backend/sim.hpp"
#include "afx/itc/mailbox.hpp"
#include "afx/net/sock_addr.hpp"
#include "afx/sys/types.hpp"

namespace afx {

// M10-04: the knobs a scenario turns. All probabilities are per-delivery
// draws against the runtime's seeded RNG.
struct FaultProfile {
    double packet_loss = 0.0;       // drop probability per delivery
    bool partial_reads = false;     // fragment deliveries into 1..3 pieces
    double slow_peer = 0.0;         // fraction of deliveries given slow_latency
    double reset_chance = 0.0;      // RST probability per delivery
    double partition_chance = 0.0;  // chance a delivery blackholes its link
    std::pair<Nanos, Nanos> partition_window{0ns, 0ns};
    Nanos min_latency{0};   // base per-link delivery delay
    Nanos max_latency{0};   // (uniform in [min, max])
    Nanos slow_latency{0};  // extra delay for slow_peer draws
};

struct SimEvent {
    enum class Kind : std::uint8_t {
        Bytes,         // feed bytes into dst fd's inbound queue
        Accept,        // pending accept fires on listen fd (aux = peer fd)
        ConnectDone,   // pending connect completes (aux = result, ud = tag)
        PeerFin,       // orderly close delivered to dst fd
        PeerReset,     // conn reset delivered to dst fd (aux = errno)
        PartitionEnd,  // lift the partition on link `aux`
        Post,          // scheduled mailbox delivery (runtime applies it)
    };
    TimePoint due{};
    std::uint64_t seq = 0;
    Kind kind = Kind::Bytes;
    SimBackend* dst = nullptr;
    int fd = -1;  // fd on dst (listen_fd for Accept)
    int aux = 0;  // peer fd / errno / link id
    UserData ud{};
    std::vector<std::byte> bytes;
    std::size_t shard = SIZE_MAX;  // Post target shard
    afx::Task task{};              // Post payload
};

class SimNet {
  public:
    explicit SimNet(std::uint64_t seed) : rng_(seed) {}

    void set_faults(FaultProfile f) { faults_ = f; }
    const FaultProfile& faults() const noexcept { return faults_; }

    TimePoint now() const noexcept { return now_; }
    void set_now(TimePoint t) noexcept { now_ = t; }

    // ---- backend hooks -----------------------------------------------------

    // submit_accept: learn which port a listen fd serves (real socket, so
    // getsockname reports the bound port).
    void listen(SimBackend& b, int listen_fd);

    // submit_connect with a net attached: deferred, never instant.
    void connect(SimBackend& src, int fd, const SockAddr& addr, UserData u);

    // submit_sendv with a net attached: route the send to the link's far
    // end. The whole iovec is ONE delivery — TCP preserves byte order across
    // a send call, so fragments share one latency draw and stay in order.
    void deliver_bytes(SimBackend& src, int fd, std::span<const ByteSpan> iov);

    // detach: the fd went away — deliver FIN to the peer, forget the link.
    void detach(SimBackend& src, int fd);

    // Runtime-driven extras.
    void post(std::size_t shard, afx::Task fn, Nanos delay);
    void partition(std::size_t link, Nanos window);  // test hook
    std::size_t links() const noexcept { return links_.size(); }

    // ---- event queue
    // ---------------------------------------------------------

    struct Later {
        bool operator()(const SimEvent& a, const SimEvent& b) const {
            if (a.due != b.due) return a.due > b.due;
            return a.seq > b.seq;
        }
    };

    // Pop the earliest event if due by `t`; empty optional otherwise.
    bool due_next(TimePoint t) const {
        return !q_.empty() && q_.top().due <= t;
    }
    SimEvent pop() {
        // priority_queue has no move-pop; SimEvent is cheap to copy except
        // bytes/task — move out via const_cast on the top is UB. Use a
        // two-step: copy top (task moves out of the stored element).
        SimEvent ev = std::move(const_cast<SimEvent&>(q_.top()));
        q_.pop();
        return ev;
    }
    const SimEvent* peek() const { return q_.empty() ? nullptr : &q_.top(); }
    TimePoint next_due() const {
        return q_.empty() ? TimePoint::max() : q_.top().due;
    }
    bool empty() const noexcept { return q_.empty(); }

    // Apply a popped event to its destination backend. Post events are the
    // runtime's job (they carry a shard index, not a backend).
    void apply(const SimEvent& ev);

    // Draw a uniform [0,1) — one RNG stream for all fault decisions keeps a
    // seed's event sequence reproducible.
    double draw() { return std::generate_canonical<double, 53>(rng_); }
    Nanos latency() {
        Nanos lo = faults_.min_latency, hi = faults_.max_latency;
        if (hi <= lo) return lo;
        return lo + Nanos(std::int64_t(draw() * double((hi - lo).count())));
    }

    std::uint64_t dropped() const noexcept { return dropped_; }

  private:
    struct Link {
        SimBackend* a;
        int a_fd;
        SimBackend* b;
        int b_fd;
        bool partitioned = false;
        // FIFO watermarks: a→b and b→a deliveries never overtake — the next
        // send on a direction is scheduled no earlier than the previous
        // send's last fragment (TCP is an ordered stream per direction).
        TimePoint next_free_ab{};
        TimePoint next_free_ba{};
    };

    void push(SimEvent ev) {
        ev.seq = seq_++;
        q_.push(std::move(ev));
    }

    Link* link_for(SimBackend& src, int fd, SimBackend** dst, int* dst_fd);

    std::priority_queue<SimEvent, std::vector<SimEvent>, Later> q_;
    // Real fds are process-unique, so a flat fd → link map suffices.
    std::unordered_map<int, std::size_t> fd_link_;
    std::unordered_map<std::uint16_t, std::pair<SimBackend*, int>> listeners_;
    std::vector<Link> links_;
    FaultProfile faults_{};
    std::mt19937_64 rng_;
    std::uint64_t seq_ = 0;
    std::uint64_t dropped_ = 0;
    TimePoint now_{};
};

}  // namespace afx

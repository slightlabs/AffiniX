#pragma once

// sim/runtime.hpp — SimRuntime: deterministic multi-EM simulation (M10-02/03).
//
//   SimRuntime sim(seed);
//   sim.add_shards(4, setup);            // or add_shard(setup)
//   sim.inject(FaultProfile{...});
//   sim.run_for(60s);                    // virtual time
//   CHECK(sim.invariants_held());
//
// All shards run interleaved on ONE thread — there is no parallelism to
// hide nondeterminism. Each shard is a BasicEventManager<VirtualClock,
// SimBackend>; its backend is attached to a shared SimNet fabric so sends
// become scheduled events and connects resolve listeners by port.
//
// One step = pop every due net event, apply it, then poll each shard until
// quiescent — in a per-step shard order shuffled by the seeded RNG. Virtual
// time then jumps to the earliest pending instant (net event, timer, or
// scheduled post) — no idle grinding.
//
// M10-03 deterministic ITC: post() lands in the target's mailbox at a
// scheduler-chosen instant, so cross-shard delivery ordering is part of the
// seed's interleaving, not incidental ring timing.
//
// M10-05/06: every delivered event is appended to trace(); same seed →
// byte-identical traces.

#include <algorithm>
#include <functional>
#include <memory>
#include <random>
#include <vector>

#include "afx/backend/sim.hpp"
#include "afx/core/event_manager.hpp"
#include "afx/sim/net.hpp"
#include "afx/sys/clock.hpp"

namespace afx {

class SimRuntime {
  public:
    using EM = BasicEventManager<VirtualClock, SimBackend>;

    // What the replay trace records per delivered event — compact and
    // comparable, so same-seed runs diff to byte-identical sequences.
    struct EventRecord {
        std::uint64_t seq;
        std::int64_t due_ns;
        std::uint8_t kind;
        std::uint32_t fd, aux;
        std::uint64_t bytes_hash;  // FNV-1a of payload, 0 when none
        std::uint32_t shard;
        bool operator==(const EventRecord&) const = default;
    };

    explicit SimRuntime(std::uint64_t seed = 1)
        : net_(seed), rng_(seed), seed_(seed) {}

    // ---- shards -------------------------------------------------------------

    EM& add_shard() {
        return add_shard([](EM&) {});
    }

    template <class F>
    EM& add_shard(F&& setup) {
        EventManagerConfig cfg;
        cfg.wait = WaitStrategy::Spin;  // sim never blocks on the host
        cfg.name = "sim-" + std::to_string(shards_.size());
        auto& s = shards_.emplace_back();
        s.em =
            std::make_unique<EM>(std::move(cfg), VirtualClock{}, SimBackend{});
        s.em->backend().set_net(&net_);
        s.em->clock().set(now_);
        order_.push_back(shards_.size() - 1);
        setup(*s.em);
        return *s.em;
    }

    void add_shards(std::size_t n, const std::function<void(EM&)>& setup = {}) {
        for (std::size_t i = 0; i < n; ++i) {
            if (setup)
                add_shard(setup);
            else
                add_shard();
        }
    }

    EM& shard(std::size_t i) { return *shards_.at(i).em; }
    SimBackend& backend(std::size_t i) { return shards_.at(i).em->backend(); }
    std::size_t size() const noexcept { return shards_.size(); }

    // ---- faults / ITC
    // ---------------------------------------------------------

    SimNet& net() noexcept { return net_; }
    void inject(FaultProfile f) { net_.set_faults(std::move(f)); }

    // Deliver `fn` to shard `i`'s mailbox at a scheduler-chosen instant —
    // the deterministic analogue of cross-thread post() (M10-03).
    void post(std::size_t shard, afx::Task fn, Nanos delay = Nanos(0)) {
        net_.post(shard, std::move(fn), delay);
    }

    // ---- driving ------------------------------------------------------------

    TimePoint now() const noexcept { return now_; }
    std::size_t steps() const noexcept { return steps_; }

    // Run until virtual time reaches now()+d.
    void run_for(Duration d) { run_until(now_ + d); }
    void run_until(TimePoint target);

    // Run until no events, timers, or pending work remain — or max_steps
    // elapse. Returns false if the cap was hit (a live-lock smell).
    bool run_until_idle(std::size_t max_steps = 100'000);

    // Invariant hook (M10-05): invoked after each step. A failing check can
    // abort via REQUIRE/throw — the trace up to the fault is available.
    void on_step(std::function<void(SimRuntime&)> f) {
        step_fn_ = std::move(f);
    }

    // ---- replay (M10-06)
    // ------------------------------------------------------

    const std::vector<EventRecord>& trace() const noexcept { return trace_; }
    std::uint64_t seed() const noexcept { return seed_; }
    bool invariants_held() const noexcept { return invariants_held_; }
    void mark_invariant_broken() noexcept { invariants_held_ = false; }

  private:
    void set_time(TimePoint t) {
        now_ = t;
        net_.set_now(t);
        for (auto& s : shards_) s.em->clock().set(t);
    }

    // Poll every shard until quiescent at the current instant — per-step
    // order shuffled by the seeded RNG (deterministic interleaving choice).
    // Returns true if any shard did work.
    bool drain_all() {
        bool any = false;
        std::shuffle(order_.begin(), order_.end(), rng_);
        for (std::size_t i : order_) {
            for (int n = 0; n < 1024 && shards_[i].em->poll_once(); ++n)
                any = true;
        }
        return any;
    }

    void apply(SimEvent&& ev) {
        if (ev.kind == SimEvent::Kind::Post) {
            if (ev.shard >= shards_.size()) return;
            Mailbox mb = shards_[ev.shard].em->mailbox();
            (void)mb.post(std::move(ev.task));
            return;
        }
        net_.apply(ev);
    }

    static std::uint64_t fnv1a(ByteSpan b) {
        std::uint64_t h = 1469598103934665603ull;
        for (std::byte c : b) {
            h ^= std::uint8_t(c);
            h *= 1099511628211ull;
        }
        return h;
    }

    EventRecord record(const SimEvent& ev) const {
        return EventRecord{ev.seq,
                           ev.due.time_since_epoch().count(),
                           std::uint8_t(ev.kind),
                           std::uint32_t(ev.fd),
                           std::uint32_t(ev.aux),
                           fnv1a(ByteSpan(ev.bytes.data(), ev.bytes.size())),
                           std::uint32_t(ev.shard)};
    }

    struct Shard {
        std::unique_ptr<EM> em;
    };

    SimNet net_;
    std::mt19937_64 rng_;
    std::vector<Shard> shards_;
    std::vector<std::size_t> order_;
    std::vector<EventRecord> trace_;
    std::function<void(SimRuntime&)> step_fn_;
    TimePoint now_{};
    std::size_t steps_ = 0;
    std::uint64_t seed_;
    bool invariants_held_ = true;
};

}  // namespace afx

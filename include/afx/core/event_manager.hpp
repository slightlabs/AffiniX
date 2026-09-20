#pragma once

// EventManager — the per-thread event loop and execution context
// (DESIGN.md §7). Template parameters are the two pluggable seams:
// the Clock policy (ADR-0001) and the IoBackend (ADR-0002).

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "afx/backend/backend.hpp"
#include "afx/backend/epoll.hpp"
#include "afx/core/context.hpp"
#include "afx/core/flight_recorder.hpp"
#include "afx/core/handle_table.hpp"
#include "afx/core/stats.hpp"
#include "afx/core/timer_wheel.hpp"
#include "afx/itc/mailbox.hpp"
#include "afx/sys/annotations.hpp"
#include "afx/sys/clock.hpp"
#include "afx/sys/inline_fn.hpp"

namespace afx {

// Net-layer forward declarations (factories are defined in net/*.hpp).
struct ServerConfig;
struct ClientConfig;
template <class P, class EM>
class TcpServer;
template <class P, class EM>
class TcpClient;

enum class WaitStrategy : std::uint8_t {
    Block,          // sleep in the backend until an event or deadline
    SpinThenBlock,  // poll with timeout 0 for spin_budget, then block
    Spin,           // never sleep
};

enum class CallbackErrorPolicy : std::uint8_t {
    RouteToHandler,  // swallow into on_error / stats (default)
    CloseConnection,
    Terminate,
};

enum class NumaPolicy : std::uint8_t { Default, LocalAlloc, Interleave };

struct MemoryConfig {
    std::size_t arena_bytes = 8u << 20;
    std::size_t read_buffer_size = 64u << 10;
    bool hugepages = false;
    NumaPolicy numa = NumaPolicy::Default;
    bool prefault = true;
};

struct EventManagerConfig {
    std::string name;
    WaitStrategy wait = WaitStrategy::Block;
    Nanos spin_budget = 50us;
    Nanos timer_tick = 1ms;
    std::size_t max_io_events = 256;
    std::size_t max_itc_batch = 128;
    std::size_t max_defer_batch = 256;
    BackendKind backend = BackendKind::Auto;
    MemoryConfig memory{};
    CallbackErrorPolicy on_callback_error = CallbackErrorPolicy::RouteToHandler;
    // appended fields (config aggregates grow by appending, §26.3)
    std::size_t mailbox_capacity = 4096;  // power of two
    OverflowPolicy mailbox_overflow = OverflowPolicy::Fail;
    Nanos stall_threshold = Nanos::zero();  // 0 = detector off
};

struct IterationInfo {
    std::uint64_t iteration;
    bool did_mailbox = false;
    bool did_timers = false;
    bool did_io = false;
    bool did_defer = false;
    Nanos duration{};
};

// RAII scope installing a Context as the EM's ambient context.
class ContextScope {
  public:
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
    ~ContextScope() { restore(); }
    ContextScope(ContextScope&& o) noexcept
        : prev_(o.prev_), tls_prev_(o.tls_prev_), active_(o.active_) {
        o.active_ = false;
    }
    void restore() noexcept {
        if (!active_) return;
        active_ = false;
        *slot_ = prev_;
        detail::tls_ambient = tls_prev_;
    }

  private:
    ContextScope(Context* slot, const Context& next)
        : slot_(slot),
          prev_(*slot),
          tls_prev_(detail::tls_ambient),
          active_(true) {
        *slot = next;
        detail::tls_ambient = slot;
    }
    Context* slot_;
    Context prev_;
    const Context* tls_prev_;
    bool active_;
    template <class, class>
    friend class BasicEventManager;
};

// ---------------------------------------------------------------------------

template <class Clock = SteadyClock, class Backend = EpollBackend>
class BasicEventManager {
  public:
    using Task = afx::Task;
    using IoFn = InlineFn<void(IoId, std::int32_t ready_mask), 48>;
    using IdleFn = InlineFn<void(), 48>;
    using IterationFn = InlineFn<void(const IterationInfo&), 48>;

    // A sink is where a completion lands: conn ops and raw watches alike.
    struct SinkEntry {
        void* obj = nullptr;
        void (*fn)(void*, OpKind, const Completion&) = nullptr;
        void (*teardown)(void*) = nullptr;  // on slot release
        const void* type_tag = nullptr;     // per-subsystem identity check
        Context ctx{};                      // ambient for dispatches
        bool write_pending = false;
    };
    using SinkHandle = Handle<struct SinkTag_>;

    explicit BasicEventManager(EventManagerConfig cfg)
        requires std::default_initializable<Clock> &&
                 std::default_initializable<Backend>
        : BasicEventManager(std::move(cfg), Clock{}, Backend{}) {}

    BasicEventManager(EventManagerConfig cfg, Clock clock, Backend backend)
        : config_(std::move(cfg)),
          clock_(std::move(clock)),
          backend_(std::move(backend)),
          wheel_(config_.timer_tick),
          mb_(std::make_shared<MailboxImpl>(config_.mailbox_capacity)),
          comp_buf_(config_.max_io_events),
          owner_(std::this_thread::get_id()) {
        mb_->overflow = config_.mailbox_overflow;
        mb_->wake_fn = [](void* p) { static_cast<Backend*>(p)->wake(); };
        mb_->wake_ctx = &backend_;
    }

    ~BasicEventManager() {
        mb_->dead.store(true, std::memory_order_release);
        // Defined close order (§20): owned objects (servers/clients) tear
        // down their connections, then sinks, then timers.
        for (auto& [p, del] : owned_) del(p);
        sinks_.for_each([](SinkEntry& s, SinkHandle) {
            if (s.teardown) s.teardown(s.obj);
        });
        sinks_.clear();
        timers_.clear();
        groups_.clear();
    }

    BasicEventManager(const BasicEventManager&) = delete;
    BasicEventManager& operator=(const BasicEventManager&) = delete;

    // ---- lifecycle --------------------------------------------------------
    void run() {
        // The EM may be constructed on one thread and run on another
        // (Runtime spawns EMs inside shard threads); the first run() claims
        // ownership of the loop for AFX_ASSERT_CURRENT.
        owner_ = std::this_thread::get_id();
        running_.store(true, std::memory_order_release);
        while (!stop_.load(std::memory_order_acquire)) poll_once();
        running_.store(false, std::memory_order_release);
    }

    // One loop iteration; stage order is the public contract (§7.3).
    bool poll_once() {
        AFX_ASSERT_CURRENT(*this);
        IterationInfo info{++stats_.iterations};
        now_ = clock_.now();
        auto iter_start = now_;

        info.did_mailbox = drain_mailbox();  // 1
        info.did_timers = expire_timers();   // 2
        Nanos timeout = wait_timeout();      // 3
        int nio = backend_.wait(comp_buf_, timeout);
        if (blocked_)  // publish Running again
            mb_->state.store(MailboxState::Running, std::memory_order_seq_cst);
        dispatch_completions(nio);  // 4
        info.did_io = nio > 0;
        info.did_defer = run_deferred();  // 5
        flush_writes();                   // 6
        bookkeeping(info, iter_start);    // 7
        return info.did_mailbox || info.did_timers || info.did_io ||
               info.did_defer;
    }

    void stop() noexcept {  // thread-safe, idempotent
        stop_.store(true, std::memory_order_release);
        backend_.wake();
    }
    bool is_current() const noexcept {
        return owner_ == std::this_thread::get_id();
    }
    bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // ---- clock --------------------------------------------------------------
    TimePoint now() const noexcept { return now_; }  // cached per iteration
    Clock& clock() noexcept { return clock_; }
    Backend& backend() noexcept { return backend_; }

    // ---- work ---------------------------------------------------------------
    void defer(Task&& t) {  // same thread, end of iteration
        AFX_ASSERT_CURRENT(*this);
        defer_next_.push_back(std::move(t));
    }
    PostResult post(Task&& t) {  // thread-safe, from anywhere
        return mb_->ring.try_push(
                   PostedItem{std::move(t), detail::ambient_context(), 0})
                   ? (post_wake(), PostResult::Ok)
                   : post_full(std::move(t));
    }
    Mailbox mailbox() const noexcept { return Mailbox(mb_); }

    // ---- context ------------------------------------------------------------
    const Context& context() const noexcept { return ctx_; }
    Deadline deadline() const noexcept { return ctx_.deadline; }
    [[nodiscard]] ContextScope with_context(Context c) {
        return ContextScope(&ctx_, std::move(c));
    }

    // ---- timers
    // ---------------------------------------------------------------
    TimerId after(Duration d, TimerFn&& fn, TimerGroup g = {}) {
        return arm(now_ + d, Duration{}, RepeatMode::FixedRate, std::move(fn),
                   g, /*precise=*/false);
    }
    TimerId at(TimePoint t, TimerFn&& fn, TimerGroup g = {}) {
        return arm(t, Duration{}, RepeatMode::FixedRate, std::move(fn), g,
                   /*precise=*/true);
    }
    TimerId every(Duration period, TimerFn&& fn,
                  Duration initial_delay = Duration::zero(),
                  RepeatMode mode = RepeatMode::FixedRate, TimerGroup g = {}) {
        Duration d = initial_delay.count() ? initial_delay : period;
        return arm(now_ + d, period, mode, std::move(fn), g, false);
    }

    bool cancel(TimerId id) noexcept {
        TimerNode* n = timers_.get(id);
        if (!n || n->cancelled) return false;
        n->cancelled = true;
        unlink_timer(*n);
        detail::glist_unlink(n);  // dead timers leave their group
        n->group = {};
        dead_timers_.push_back(id);
        ++stats_.timers_cancelled;
        return true;
    }

    bool reschedule(TimerId id, Duration d) {
        TimerNode* n = timers_.get(id);
        if (!n || n->cancelled) return false;
        unlink_timer(*n);
        n->expiry = now_ + d;
        n->expiry_tick = wheel_.tick_of(n->expiry);
        insert_timer(*n);
        return true;
    }

    std::optional<Duration> time_until(TimerId id) const {
        const TimerNode* n = timers_.get(id);
        if (!n || n->cancelled) return std::nullopt;
        return n->expiry > now_ ? n->expiry - now_ : Duration::zero();
    }

    TimerGroup make_timer_group() {
        auto [id, st] = groups_.emplace();
        (void)st;
        return id;
    }

    std::size_t cancel_group(TimerGroup g) noexcept {
        TimerGroupState* gs = groups_.get(g);
        if (!gs) return 0;
        std::size_t n = 0;
        while (!gs->empty()) {
            TimerNode* node = gs->sentinel.group_next;
            detail::glist_unlink(node);
            node->group = {};
            node->cancelled = true;
            unlink_timer(*node);
            dead_timers_.push_back(node->id);
            ++n;
        }
        stats_.groups_cancelled += n ? 1 : 0;
        stats_.timers_cancelled += n;
        return n;
    }

    // ---- raw fd escape hatch
    // --------------------------------------------------
    Result<IoId> watch(int fd, Interest i, IoFn&& fn) {
        AFX_ASSERT_CURRENT(*this);
        auto* box = new IoFn(std::move(fn));
        auto [h, s] = sinks_.emplace(
            SinkEntry{box,
                      [](void* p, OpKind, const Completion& c) {
                          auto* b = static_cast<IoFn*>(p);
                          // recover the IoId: sink handle == the completion's
                          // tag
                          (*b)(IoId{c.user.slot(), c.user.gen()}, c.result);
                      },
                      [](void* p) { delete static_cast<IoFn*>(p); }, nullptr,
                      ctx_, false});
        UserData ud = UserData::make(std::uint8_t(OpKind::Watch), h.idx, h.gen);
        auto r = backend_.attach(fd, i, ud);
        if (!r) {
            sinks_.release(h);
            return r.error();
        }
        watch_fds_[key_of(h)] = fd;
        return IoId{h.idx, h.gen};
    }

    Result<void> modify(IoId id, Interest i) {
        auto it = watch_fds_.find(key_of(SinkHandle{id.idx, id.gen}));
        if (it == watch_fds_.end())
            return make_error(ErrorCategory::Internal, Err::NotFound);
        UserData ud =
            UserData::make(std::uint8_t(OpKind::Watch), id.idx, id.gen);
        return backend_.modify(it->second, i, ud);
    }

    void unwatch(IoId id) noexcept {
        auto it = watch_fds_.find(key_of(SinkHandle{id.idx, id.gen}));
        if (it != watch_fds_.end()) {
            backend_.detach(it->second);
            watch_fds_.erase(it);
        }
        SinkEntry* s = sinks_.get({id.idx, id.gen});
        if (s && s->teardown) s->teardown(s->obj);
        sinks_.release({id.idx, id.gen});
    }

    // ---- net-layer plumbing (used by Connection / TcpServer / TcpClient)
    // ------ Register a completion sink; returns the handle encoded into
    // UserData.
    SinkHandle register_sink(void* obj,
                             void (*fn)(void*, OpKind, const Completion&),
                             void (*teardown)(void*) = nullptr,
                             Context ctx = {}, const void* type_tag = nullptr) {
        auto [h, s] =
            sinks_.emplace(SinkEntry{obj, fn, teardown, type_tag, ctx, false});
        return h;
    }
    void release_sink(SinkHandle h) noexcept {
        SinkEntry* s = sinks_.get(h);
        if (s && s->teardown) s->teardown(s->obj);
        sinks_.release(h);
    }
    SinkEntry* sink(SinkHandle h) noexcept { return sinks_.get(h); }
    UserData tag_of(SinkHandle h, OpKind kind) const noexcept {
        return UserData::make(std::uint8_t(kind), h.idx, h.gen);
    }
    void note_write_pending(SinkHandle h) {
        SinkEntry* s = sinks_.get(h);
        if (s && !s->write_pending) {
            s->write_pending = true;
            pending_writes_.push_back(h);
        }
    }
    Result<void> submit_recv(SinkHandle h, int fd, MutByteSpan buf) {
        return backend_.submit_recv(tag_of(h, OpKind::Recv), fd, buf);
    }
    Result<void> submit_send(SinkHandle h, int fd, ByteSpan b) {
        return backend_.submit_send(tag_of(h, OpKind::Send), fd, b);
    }
    Result<void> submit_sendv(SinkHandle h, int fd,
                              std::span<const ByteSpan> iov) {
        return backend_.submit_sendv(tag_of(h, OpKind::Send), fd, iov);
    }
    Result<void> submit_accept(SinkHandle h, int listen_fd) {
        return backend_.submit_accept(tag_of(h, OpKind::Accept), listen_fd);
    }
    Result<void> submit_connect(SinkHandle h, int fd, const SockAddr& a) {
        return backend_.submit_connect(tag_of(h, OpKind::Connect), fd, a);
    }
    Result<void> backend_cancel(SinkHandle h, OpKind k) {
        return backend_.cancel(tag_of(h, k));
    }
    Result<void> backend_detach(int fd) { return backend_.detach(fd); }
    Result<void> backend_attach(int fd, Interest i, SinkHandle h, OpKind k) {
        return backend_.attach(fd, i, tag_of(h, k));
    }

    // ---- hooks
    // ----------------------------------------------------------------
    void on_idle(IdleFn&& f) { idle_fn_ = std::move(f); }
    void on_iteration(IterationFn&& f) { iter_fn_ = std::move(f); }

    // Ownership: servers/clients made by make_server/make_client are owned by
    // the EM and destroyed with it (§7.2).
    void own(void* p, void (*del)(void*)) { owned_.emplace_back(p, del); }

    // ---- introspection
    // ----------------------------------------------------------
    const Stats& stats() const noexcept { return stats_; }
    Stats& stats() noexcept { return stats_; }
    LatencyMetrics& latency() noexcept { return latency_; }
    const EventManagerConfig& config() const noexcept { return config_; }
    FlightRecorder& recorder() noexcept { return recorder_; }

    // Factories (defined in net/*.hpp — one def, used by every instantiation).
    template <class P, class Handlers>
    Result<TcpServer<P, BasicEventManager>*> make_server(ServerConfig,
                                                         Handlers&&);
    template <class P, class Handlers>
    Result<TcpClient<P, BasicEventManager>*> make_client(ClientConfig,
                                                         Handlers&&);

  private:
    // ---- stages
    // -----------------------------------------------------------------
    bool drain_mailbox() {
        std::size_t n = 0;
        PostedItem it;
        while (n < config_.max_itc_batch && mb_->ring.try_pop(it)) {
            ++stats_.mailbox_pops;
            ++n;
            if (it.ctx.deadline.expired(now_)) {
                // §7.4: shed work whose caller has already given up.
                ++stats_.deadline_expired_before_start;
                recorder_.record(
                    EventKind::DeadlineExpired, 0,
                    std::uint32_t(it.ctx.deadline.remaining(now_).count()));
                continue;
            }
            run_guarded(it.ctx, std::move(it.fn));
        }
        return n > 0;
    }

    bool expire_timers() {
        bool fired = false;
        auto fire = [this, &fired](TimerNode& n) {
            fired = true;
            fire_timer(n);
        };
        wheel_.advance(wheel_.floor_tick(now_), fire);
        heap_.expire(now_, fire);
        return fired;
    }

    void fire_timer(TimerNode& n) {
        if (n.cancelled) {
            dead_timers_.push_back(n.id);
            return;
        }
        TimerCtx tc{n.id, n.expiry, now_, 0};
        bool repeats = n.period.count() > 0;
        if (repeats && n.mode == RepeatMode::FixedRate) {
            TimePoint next = n.expiry + n.period;
            while (next <= now_) {
                next += n.period;
                ++tc.missed;
            }
            stats_.timer_coalesced += tc.missed;
            n.expiry = next;
            n.expiry_tick = wheel_.tick_of(next);
            insert_timer(n);
        }
        ++stats_.timers_fired;
        recorder_.record(EventKind::TimerFire, n.id.idx,
                         std::uint32_t(tc.lateness().count() & 0xFFFFFFFF),
                         tc.missed, n.ctx.trace.short_id());
        latency_.timer_lateness_ns.record(std::uint64_t(
            std::max<Nanos>(tc.lateness(), Nanos::zero()).count()));
        run_guarded(n.ctx, [&]() { n.fn(tc); });
        if (repeats && n.mode == RepeatMode::FixedDelay && !n.cancelled) {
            n.expiry = now_ + n.period;
            n.expiry_tick = wheel_.tick_of(n.expiry);
            insert_timer(n);
        }
        // cancel()/cancel_group() already queued a cancelled node for
        // reclamation; only live one-shots enqueue here. A one-shot that
        // fires on its own (rather than via cancel()) was never unlinked
        // from its TimerGroup, so do that now — otherwise the group's
        // intrusive list keeps a pointer into a slot that reclaim() is
        // about to recycle for an unrelated timer, corrupting the list
        // (cancel_group() on the real member can then loop forever).
        if (!repeats && !n.cancelled) {
            detail::glist_unlink(&n);
            dead_timers_.push_back(n.id);
        }
    }

    void insert_timer(TimerNode& n) {
        n.in_heap = false;
        if (wheel_.fits(n.expiry_tick, wheel_.now_tick()) && !n.precise_)
            wheel_.insert(n, wheel_.now_tick());
        else
            heap_.push(n);
    }
    // Unlink from the scheduling structure only — group membership is a
    // lifetime property, so reschedule() must not eject the timer from its
    // TimerGroup. Cancel paths unlink the group list separately.
    void unlink_timer(TimerNode& n) {
        if (n.in_heap)
            heap_.remove(n);
        else
            wheel_.unlink(n);
    }

    TimerId arm(TimePoint expiry, Duration period, RepeatMode mode,
                TimerFn&& fn, TimerGroup g, bool precise) {
        auto [id, n] = timers_.emplace();
        n->id = id;
        n->expiry = expiry;
        n->expiry_tick = wheel_.tick_of(expiry);
        n->period = period;
        n->mode = mode;
        n->fn = std::move(fn);
        n->ctx = ctx_;  // context follows into timers
        n->precise_ = precise;
        if (g.valid())
            if (TimerGroupState* gs = groups_.get(g)) {
                gs->add(*n);
                n->group = g;
            }
        insert_timer(*n);
        ++stats_.timers_armed;
        return id;
    }

    Nanos wait_timeout() {
        // Spin strategy never blocks; SpinThenBlock blocks once the spin
        // budget after last useful work has elapsed (§8).
        bool may_block;
        switch (config_.wait) {
            case WaitStrategy::Spin:
                may_block = false;
                break;
            case WaitStrategy::Block:
                may_block = true;
                break;
            case WaitStrategy::SpinThenBlock:
            default:
                may_block = now_ >= spin_until_;
                break;
        }
        if (!defer_next_.empty()) may_block = false;
        if (!mb_->ring.empty()) may_block = false;

        Nanos timeout = Nanos::zero();
        if (may_block) {
            // Arm/block protocol, consumer side (§8.1).
            mb_->state.store(MailboxState::Blocked, std::memory_order_seq_cst);
            if (!mb_->ring.empty()) {
                mb_->state.store(MailboxState::Running,
                                 std::memory_order_seq_cst);
                return Nanos::zero();
            }
            ++stats_.blocks;
            timeout = next_deadline_timeout();
            if (timeout < Nanos::zero()) timeout = Nanos::zero();
            blocked_ = true;
        } else {
            ++stats_.spins;
        }
        return timeout;
    }

    Nanos next_deadline_timeout() {
        TimePoint nd = TimePoint::max();
        if (TimerNode* t = heap_.top()) nd = t->expiry;
        if (wheel_.size()) {
            TimePoint w(Nanos((wheel_.now_tick() + 1) * wheel_.tick().count()));
            if (w < nd) nd = w;
        }
        if (nd == TimePoint::max()) return Nanos(24h);  // effectively forever
        return nd > now_ ? nd - now_ : Nanos::zero();
    }

    void dispatch_completions(int n) {
        for (int i = 0; i < n; ++i) {
            const Completion& c = comp_buf_[i];
            SinkHandle h{c.user.slot(), c.user.gen()};
            SinkEntry* s = sinks_.get(h);
            if (!s) continue;  // object closed while completion in flight
            run_guarded(s->ctx,
                        [&] { s->fn(s->obj, OpKind(c.user.kind()), c); });
        }
    }

    bool run_deferred() {
        if (defer_next_.empty()) return false;
        defer_run_.clear();
        defer_run_.swap(defer_next_);
        std::size_t n = 0;
        for (auto& t : defer_run_) {
            if (n++ >= config_.max_defer_batch) {
                defer_next_.push_back(std::move(t));  // remainder next round
                continue;
            }
            run_guarded(ctx_, std::move(t));
        }
        defer_run_.clear();
        return true;
    }

    void flush_writes() {
        // Stage 6: the write-coalescing point. Connection::send() queues
        // bytes; here each dirty connection emits one sendv.
        auto pending = std::move(pending_writes_);
        pending_writes_.clear();
        for (SinkHandle h : pending) {
            SinkEntry* s = sinks_.get(h);
            if (!s) continue;
            s->write_pending = false;
            static const Completion kFlush{};
            s->fn(s->obj, OpKind::FlushWrite, kFlush);
        }
    }

    void bookkeeping(IterationInfo& info, TimePoint iter_start) {
        bool worked = info.did_mailbox || info.did_timers || info.did_io ||
                      info.did_defer || !pending_writes_.empty();
        if (worked) spin_until_ = now_ + config_.spin_budget;
        if (blocked_) {
            blocked_ = false;
            ++stats_.wakeups;
        }
        if (!worked) {
            ++stats_.idle_iterations;
            if (idle_fn_) idle_fn_();
        }
        info.duration = clock_.now_uncached() - iter_start;
        latency_.iteration_ns.record(std::uint64_t(
            std::max<Nanos>(info.duration, Nanos::zero()).count()));
        if (iter_fn_) iter_fn_(info);

        // Deferred reclamation (§13): slots released this iteration only
        // return to the free list now.
        for (TimerId id : dead_timers_) timers_.release(id);
        dead_timers_.clear();
        timers_.reclaim();
        sinks_.reclaim();
        groups_.reclaim();
    }

    // ---- callback error policy
    // --------------------------------------------------
    template <class F>
    void run_guarded(const Context& c, F&& fn) {
        auto scope = with_context(c);
        if (config_.stall_threshold > Nanos::zero()) {
            TimePoint a = clock_.now_uncached();
            invoke_guarded(std::forward<F>(fn));
            Nanos d = clock_.now_uncached() - a;
            if (d > config_.stall_threshold) report_stall(d);
        } else {
            invoke_guarded(std::forward<F>(fn));
        }
    }

    template <class F>
    void invoke_guarded(F&& fn) {
        if constexpr (true) {
            try {
                fn();
            } catch (...) {
                ++stats_.callback_errors;
                recorder_.record(EventKind::CallbackError, 0);
                if (config_.on_callback_error == CallbackErrorPolicy::Terminate)
                    std::terminate();
                // RouteToHandler: surfaced via stats; CloseConnection is
                // handled by the net layer's own wrappers.
            }
        }
    }

    void report_stall(Nanos d) {
        // Self-diagnosing stalls (§21): flight record + counter; a LogSink
        // hook can pick this up without a synchronous write in the loop.
        ++stats_.internal_errors;
        recorder_.record(EventKind::StateChange, 0,
                         std::uint32_t(d.count() & 0xFFFFFFFF), 1);
    }

    PostResult post_full(Task&& t) {
        ++stats_.mailbox_full;
        switch (config_.mailbox_overflow) {
            case OverflowPolicy::Fail:
                return PostResult::Full;
            case OverflowPolicy::Abort:
                std::abort();
            case OverflowPolicy::SpinRetry:
                for (;;) {
                    if (mb_->ring.try_push(PostedItem{
                            std::move(t), detail::ambient_context(), 0})) {
                        post_wake();
                        return PostResult::Ok;
                    }
                    if (mb_->dead.load(std::memory_order_acquire))
                        return PostResult::Closed;
                }
        }
        return PostResult::Full;
    }
    void post_wake() {
        ++stats_.mailbox_pushes;
        if (mb_->state.load(std::memory_order_seq_cst) == MailboxState::Blocked)
            backend_.wake();
    }

    // ---- members
    // ------------------------------------------------------------------
    EventManagerConfig config_;
    Clock clock_;
    Backend backend_;
    TimePoint now_{};
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};

    TimerWheel wheel_;
    TimerHeap heap_;
    HandleTable<TimerNode, TimerId> timers_;
    HandleTable<TimerGroupState, TimerGroupId> groups_;
    std::vector<TimerId> dead_timers_;

    static std::uint64_t key_of(SinkHandle h) noexcept {
        return (std::uint64_t(h.idx) << 32) | h.gen;
    }

    HandleTable<SinkEntry, SinkHandle> sinks_;
    std::unordered_map<std::uint64_t, int> watch_fds_;  // watch -> fd
    std::vector<SinkHandle> pending_writes_;

    std::shared_ptr<MailboxImpl> mb_;
    std::vector<Completion> comp_buf_;
    std::vector<Task> defer_next_, defer_run_;

    std::vector<std::pair<void*, void (*)(void*)>> owned_;

    Context ctx_{};
    IdleFn idle_fn_;
    IterationFn iter_fn_;

    Stats stats_{};
    LatencyMetrics latency_;
    FlightRecorder recorder_;

    std::thread::id owner_;
    TimePoint spin_until_{};  // epoch default: blocks until first work
    bool blocked_ = false;
};

// The default production configuration: real clock + epoll on Linux.
// Tests instantiate BasicEventManager<VirtualClock, SimBackend>.
using EventManager = BasicEventManager<SteadyClock, EpollBackend>;

}  // namespace afx

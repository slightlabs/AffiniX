#pragma once

// EventManager — the per-thread event loop and execution context
// (DESIGN.md §7). Template parameters are the two pluggable seams:
// the Clock policy (ADR-0001) and the IoBackend (ADR-0002).

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef AFX_DEBUG_CHAOS
#include <random>
#endif

#include "afx/backend/backend.hpp"
#ifdef AFX_HAVE_KQUEUE
#include "afx/backend/kqueue.hpp"
#else
#include "afx/backend/epoll.hpp"
#endif
#ifdef AFX_WITH_URING
#include "afx/backend/uring.hpp"
#endif
#include "afx/core/arena.hpp"
#include "afx/core/context.hpp"
#include "afx/core/flight_recorder.hpp"
#include "afx/core/handle_table.hpp"
#include "afx/core/stats.hpp"
#include "afx/core/timer_wheel.hpp"
#include "afx/coro/frame_cache.hpp"
#include "afx/coro/ops.hpp"
#include "afx/coro/task.hpp"
#include "afx/itc/mailbox.hpp"
#include "afx/sys/annotations.hpp"
#include "afx/sys/clock.hpp"
#include "afx/sys/crash_dump.hpp"
#include "afx/sys/inline_fn.hpp"

namespace afx {

// Net-layer forward declarations (factories are defined in net/*.hpp).
struct ServerConfig;
struct ClientConfig;
struct UdpConfig;
template <class P, class EM>
class TcpServer;
template <class P, class EM>
class TcpClient;
template <class P, class EM>
class UdpSocket;

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
    // M8-08: SQPOLL on the io_uring backend when wait==Spin — submissions go
    // through the kernel's poll thread with zero syscalls. Degrades silently
    // to a normal ring when SQPOLL is denied.
    bool uring_sqpoll = true;
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
                 (std::default_initializable<Backend> ||
                  std::constructible_from<Backend, BackendKind>)
        : BasicEventManager(
              std::move(cfg), Clock{},
              make_backend<Backend>(
                  cfg.backend,
                  cfg.uring_sqpoll && cfg.wait == WaitStrategy::Spin)) {}

    BasicEventManager(EventManagerConfig cfg, Clock clock, Backend backend)
        : config_(std::move(cfg)),
          clock_(std::move(clock)),
          backend_(std::move(backend)),
          wheel_(config_.timer_tick),
          timers_(first_gen_seed()),
          groups_(first_gen_seed()),
          sinks_(first_gen_seed()),
          mb_(std::make_shared<MailboxImpl>(config_.mailbox_capacity)),
          comp_buf_(config_.max_io_events),
          owner_(std::this_thread::get_id()) {
        // Seed cached time so timers armed before the first poll_once()
        // (e.g. TcpClient::connect_next's timeout) measure from real now —
        // epoch would collapse them to immediate expiry.
        now_ = clock_.now();
        mb_->overflow = config_.mailbox_overflow;
        mb_->wake_fn = [](void* p) { static_cast<Backend*>(p)->wake(); };
        mb_->wake_ctx = &backend_;
        // M8-05: publish our ring fd so peers on uring EMs can MSG_RING us.
        mb_->msgring_fd = backend_.wake_ring_fd();

        // M5-05: the arena carries the MemoryConfig NUMA policy. When the EM
        // is constructed inside its shard thread (Runtime's thread_main pins
        // first), LocalAlloc binds to the shard's own node.
        numa::AllocOpts mo;
        mo.hugepages = config_.memory.hugepages;
        mo.prefault = config_.memory.prefault;
        mo.interleave = config_.memory.numa == NumaPolicy::Interleave;
        mo.node = config_.memory.numa == NumaPolicy::LocalAlloc
                      ? numa::current_node()
                      : -1;
        arena_ = Arena(config_.memory.arena_bytes, mo);
        frame_cache_.bind(&arena_);

        // M9: type-erased coroutine hooks (coro/task.hpp). The promise talks
        // to the EM only through this table so coro code never names the
        // BasicEventManager template.
        coro_ops_.em = this;
        coro_ops_.frame_alloc = &coro_alloc_thunk;
        coro_ops_.frame_free = &coro_free_thunk;
        coro_ops_.root_link = &coro_link_thunk;
        coro_ops_.root_unlink = &coro_unlink_thunk;
        coro_ops_.ctx_push = &coro_push_thunk;
        coro_ops_.ctx_pop = &coro_pop_thunk;
        coro_ops_.timer_after = &coro_after_thunk;
        coro_ops_.timer_cancel = &coro_cancel_thunk;
        coro_ops_.home_mb = mb_;

        // M7-08: expose this EM's flight recorder to the crash-dump handler.
        register_recorder(&recorder_);
    }

    ~BasicEventManager() {
        unregister_recorder(&recorder_);
        mb_->dead.store(true, std::memory_order_release);
        // M9: cancel coroutine roots first — their awaiters resume with a
        // Cancelled result and unwind while conns/timers are still alive.
        // A root that refuses cancellation (non-cancellable custom awaiter)
        // is destroyed outright; parked frames never outlive the arena.
        while (coro_roots_) {
            coro::PromiseBase* p = coro_roots_;
            if (p->cancel_fn) p->request_stop();
            if (coro_roots_ == p) {
                coro_unlink_thunk(this, p);
                if (p->destroy_frame) p->destroy_frame(p);
            }
        }
        // Defined close order (§20): owned objects (servers/clients) tear
        // down their connections, then sinks, then timers.
        for (auto& o : owned_) o.del(o.p);
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
        // M8-05: posts made on this thread may wake peers via MSG_RING on
        // our own ring when the backend supports it.
        if constexpr (requires(Backend& b, int fd,
                               const std::weak_ptr<MailboxImpl>& t) {
                          { b.msg_ring_wake(fd, t) } -> std::same_as<bool>;
                      }) {
            detail::wake_bridge.backend = &backend_;
            detail::wake_bridge.send_msg_ring =
                +[](void* b, int fd, const std::weak_ptr<MailboxImpl>& t) {
                    return static_cast<Backend*>(b)->msg_ring_wake(fd, t);
                };
        }
        while (!stop_.load(std::memory_order_acquire)) poll_once();
        if constexpr (requires(Backend& b, int fd,
                               const std::weak_ptr<MailboxImpl>& t) {
                          { b.msg_ring_wake(fd, t) } -> std::same_as<bool>;
                      })
            detail::wake_bridge.backend = nullptr;
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
    // Earliest instant at which a timer could fire — the sim scheduler (M10)
    // uses it to jump virtual time without grinding empty iterations.
    // TimePoint::max() means no timer is armed.
    TimePoint next_wakeup() noexcept {
        now_ = clock_.now();
        TimePoint nd = TimePoint::max();
        if (TimerNode* t = heap_.top()) nd = t->expiry;
        if (wheel_.size()) {
            std::uint64_t t = wheel_.next_due_tick();
            nd = std::min(
                nd, TimePoint(Nanos(std::int64_t(t) * wheel_.tick().count())));
        }
        return nd;
    }
    // Per-EM NUMA-local bump arena (§12.2). Pools carve chunks from it;
    // when exhausted (or arena_bytes == 0) alloc() returns nullptr and
    // callers fall back to the heap with visible accounting.
    Arena& arena() noexcept { return arena_; }

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

    // ---- coroutines (M9; ADR-0005 opt-in layer)
    // --------------------------------------------------
    // spawn(): start `t` as a root task on this EM. The task self-destroys
    // at completion; EM teardown cancels still-parked roots. `ctx` merges
    // with the ambient Context: trace inherits, deadlines only tighten.
    // Cancellation: use TaskScope (request_stop + join) or StopToken in ctx.
    void spawn(coro::Task<void> t, Context ctx = {}) {
        auto& p = t.promise();
        p.engine = &coro_ops_;
        p.is_root = true;
        const Context& amb = detail::ambient_context();
        p.ctx = ctx;
        if (!ctx.trace.valid()) p.ctx.trace = amb.trace;
        p.ctx.deadline = amb.deadline.earliest_of(ctx.deadline);
        if (!ctx.stop) p.ctx.stop = amb.stop;
        coro_link_thunk(this, &p);
        t.start();
        (void)t.release();  // root ownership lives in the coro_roots_ list
    }

    // `co_await em.sleep(d)` — timer-wheel resolution, cancellable.
    coro::SleepOp sleep(Duration d) noexcept { return {&coro_ops_, d}; }

    coro::EngineOps* coro_ops() noexcept { return &coro_ops_; }
    // Frames that fell back to the heap (arena exhausted/oversized) —
    // spike 0002's visibility contract.
    std::size_t coro_heap_frames() const noexcept {
        return frame_cache_.heap_fallbacks();
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
    // §9.4: opt a fd into SO_TIMESTAMPING stamp delivery on recv completions.
    void backend_set_timestamping(int fd, bool on) {
        backend_.set_timestamping(fd, on);
    }
    // M11-06: register the SCM_RIGHTS landing pad for fd (Connection-owned).
    void backend_set_fd_inbox(int fd, FdInbox in) {
        backend_.set_fd_inbox(fd, in);
    }

    // ---- hooks
    // ----------------------------------------------------------------
    void on_idle(IdleFn&& f) { idle_fn_ = std::move(f); }
    void on_iteration(IterationFn&& f) { iter_fn_ = std::move(f); }

    // Ownership: servers/clients made by make_server/make_client are owned by
    // the EM and destroyed with it (§7.2). ShutdownHooks let them take part
    // in the §20 drain sequence (begin_shutdown below).
    struct ShutdownHooks {
        void (*begin)(void*) = nullptr;    // stop accepting new work
        void (*notify)(void*) = nullptr;   // tell the app shutdown is coming
        bool (*drained)(void*) = nullptr;  // in-flight writes finished?
        void (*shutdown_write)(void*) = nullptr;  // TCP half-close
    };
    struct Owned {
        void* p;
        void (*del)(void*);
        ShutdownHooks hooks{};
    };
    void own(void* p, void (*del)(void*), ShutdownHooks h = {}) {
        owned_.push_back(Owned{p, del, h});
    }

    // §20 drain sequence: (1) listeners close, (2) apps are notified,
    // (3) in-flight writes drain until `deadline`, (4) connections
    // half-close, (5) the loop stops and destructors hard-close the rest.
    // Driven by Runtime::shutdown; safe to call directly on the EM thread.
    void begin_shutdown(TimePoint deadline) {
        AFX_ASSERT_CURRENT(*this);
        if (shutting_down_) return;
        shutting_down_ = true;
        for (auto& o : owned_)
            if (o.hooks.begin) o.hooks.begin(o.p);
        for (auto& o : owned_)
            if (o.hooks.notify) o.hooks.notify(o.p);
        shutdown_deadline_ = deadline;
        drain_step();
    }
    bool shutting_down() const noexcept { return shutting_down_; }

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
    template <class P, class Handlers>
    Result<UdpSocket<P, BasicEventManager>*> make_udp(UdpConfig, Handlers&&);
    // M9: a server whose per-connection session is a coroutine —
    // `session(ConnRef<P,EM>) -> CoroTask<void>`, spawned at on_open.
    template <class P, class SessionFactory>
    Result<TcpServer<P, BasicEventManager>*> make_coro_server(ServerConfig,
                                                              SessionFactory&&);

  private:
    // Drain step (§20 stage 3): poll owned objects until their write queues
    // empty or the deadline expires, then half-close and stop.
    void drain_step() {
        bool all = true;
        for (auto& o : owned_)
            if (o.hooks.drained && !o.hooks.drained(o.p)) {
                all = false;
                break;
            }
        if (all || now_ >= shutdown_deadline_) {
            for (auto& o : owned_)
                if (o.hooks.shutdown_write) o.hooks.shutdown_write(o.p);
            stop();  // stage 5: run() returns, destructors hard-close
            return;
        }
        after(1ms, [this](TimerCtx) { drain_step(); });
    }

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
            // Arm/block protocol, consumer side (§8.1). The seq_cst fence
            // forbids the ring check's loads from completing before the
            // Blocked store is visible (StoreLoad); without it a producer
            // can push, read stale Running, and skip the wake while we see
            // an empty ring and sleep.
            mb_->state.store(MailboxState::Blocked, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
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
        // M6-13: completion dispatch order across sinks is unspecified —
        // shuffle it under AFX_DEBUG_CHAOS so tests can't depend on kernel
        // return order.
        chaos_shuffle_n(comp_buf_, std::size_t(n));
        std::uint64_t rt = 0;
        bool rt_taken = false;
        for (int i = 0; i < n; ++i) {
            const Completion& c = comp_buf_[i];
            // M8-07 wire-level histograms. dequeue→handler uses the TSC the
            // backend stamped at dequeue; kernel-side spans need the sw/hw
            // stamps, which only exist when SO_TIMESTAMPING is enabled.
            if (c.stamps.tsc) {
                std::uint64_t dq = tsc_delta_to_ns(rdtsc() - c.stamps.tsc);
                if (dq) latency_.dequeue_to_handler_ns.record(dq);
            }
            if (c.flags & CompletionFlag::HasSwStamp) {
                if (!rt_taken) {
                    rt = realtime_ns();  // one vDSO read per batch, not per op
                    rt_taken = true;
                }
                if (rt > c.stamps.sw_ns) {
                    latency_.kernel_to_dequeue_ns.record(rt - c.stamps.sw_ns);
                    if (c.user.kind() == std::uint8_t(OpKind::Recv))
                        latency_.recv_to_handler_ns.record(rt - c.stamps.sw_ns);
                }
                if ((c.flags & CompletionFlag::HasHwStamp) &&
                    c.stamps.sw_ns > c.stamps.hw_ns)
                    latency_.nic_to_kernel_ns.record(c.stamps.sw_ns -
                                                     c.stamps.hw_ns);
            }
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
        // M6-13: flush order between connections is unspecified.
        chaos_shuffle(pending);
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
        // return to the free list now. M6-13: reclamation order is
        // unspecified — shuffle so freelist order varies between runs.
        chaos_shuffle(dead_timers_);
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
        // See Mailbox::after_push — the seq_cst fence keeps this load from
        // completing before the ring push it guards is globally visible.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (mb_->state.load(std::memory_order_seq_cst) == MailboxState::Blocked)
            backend_.wake();
    }

    // ---- members
    // ------------------------------------------------------------------
    EventManagerConfig config_;
    Clock clock_;
    Backend backend_;
    Arena arena_;
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

    std::vector<Owned> owned_;
    bool shutting_down_ = false;
    TimePoint shutdown_deadline_{};

    Context ctx_{};
    IdleFn idle_fn_;
    IterationFn iter_fn_;

    Stats stats_{};
    LatencyMetrics latency_;
    FlightRecorder recorder_;

    std::thread::id owner_;
    TimePoint spin_until_{};  // epoch default: blocks until first work
    bool blocked_ = false;

    // ---- coroutine layer (M9)
    // ------------------------------------------------------------------
    coro::EngineOps coro_ops_{};
    coro::FrameCache frame_cache_{};
    coro::PromiseBase* coro_roots_ = nullptr;

    static void* coro_alloc_thunk(void* e, std::size_t n) noexcept {
        return static_cast<BasicEventManager*>(e)->frame_cache_.alloc(n);
    }
    static void coro_free_thunk(void* e, void* p) noexcept {
        static_cast<BasicEventManager*>(e)->frame_cache_.free(p);
    }
    static void coro_link_thunk(void* e, void* pr) noexcept {
        auto& em = *static_cast<BasicEventManager*>(e);
        auto* p = static_cast<coro::PromiseBase*>(pr);
        p->coro_prev = nullptr;
        p->coro_next = em.coro_roots_;
        if (em.coro_roots_) em.coro_roots_->coro_prev = p;
        em.coro_roots_ = p;
        ++em.stats_.coro_spawned;
    }
    static void coro_unlink_thunk(void* e, void* pr) noexcept {
        auto& em = *static_cast<BasicEventManager*>(e);
        auto* p = static_cast<coro::PromiseBase*>(pr);
        if (p->coro_prev)
            p->coro_prev->coro_next = p->coro_next;
        else
            em.coro_roots_ = p->coro_next;
        if (p->coro_next) p->coro_next->coro_prev = p->coro_prev;
        p->coro_prev = p->coro_next = nullptr;
        ++em.stats_.coro_completed;
    }
    // Context push/pop mirror ContextScope but stash the saved slot pair in
    // the promise so resume/suspend nesting stays balanced without a scope
    // object surviving across suspends.
    static void coro_push_thunk(void* e, coro::PromiseBase* p) noexcept {
        auto& em = *static_cast<BasicEventManager*>(e);
        p->saved_ctx_ = em.ctx_;
        p->saved_tls_ = detail::tls_ambient;
        em.ctx_ = p->ctx;
        detail::tls_ambient = &em.ctx_;
    }
    static void coro_pop_thunk(void* e, coro::PromiseBase* p) noexcept {
        auto& em = *static_cast<BasicEventManager*>(e);
        em.ctx_ = p->saved_ctx_;
        detail::tls_ambient = p->saved_tls_;
    }
    static void* coro_after_thunk(void* e, Nanos d, void* arg,
                                  void (*fire)(void*)) {
        auto& em = *static_cast<BasicEventManager*>(e);
        TimerId id = em.after(d, [arg, fire](TimerCtx) { fire(arg); });
        return std::bit_cast<void*>(id);
    }
    static bool coro_cancel_thunk(void* e, void* tok) noexcept {
        return static_cast<BasicEventManager*>(e)->cancel(
            std::bit_cast<TimerId>(tok));
    }

#ifdef AFX_DEBUG_CHAOS
    // M6-13: debug builds randomise order the design leaves unspecified
    // (completion dispatch, write-flush order, slot reclamation) so test
    // suites can't ossify against incidental implementation order.
    std::mt19937_64 chaos_{std::random_device{}()};
    template <class T>
    void chaos_shuffle(std::vector<T>& v) {
        if (v.size() > 1) std::shuffle(v.begin(), v.end(), chaos_);
    }
    template <class T>
    void chaos_shuffle_n(std::vector<T>& v, std::size_t n) {
        n = std::min(n, v.size());
        if (n > 1) std::shuffle(v.begin(), v.begin() + n, chaos_);
    }
#else
    template <class T>
    void chaos_shuffle(std::vector<T>&) {}
    template <class T>
    void chaos_shuffle_n(std::vector<T>&, std::size_t) {}
#endif

    // M6-13: handle tables start at a random nonzero generation under chaos
    // so no test can depend on generation 1 being the first issued.
    static std::uint32_t first_gen_seed() noexcept {
#ifdef AFX_DEBUG_CHAOS
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        std::uint32_t g = std::uint32_t(rng());
        return g ? g : 1;
#else
        return 1;
#endif
    }

    // Backend construction honoring config.backend when the backend type
    // accepts a BackendKind (AutoBackend); fixed-type backends ignore it.
    // Backends that can take an sqpoll flag (AutoBackend) additionally see
    // whether the wait strategy is Spin (M8-08).
    template <class B>
    static B make_backend(BackendKind kind, bool sqpoll) {
        if constexpr (std::constructible_from<B, BackendKind, bool>)
            return B{kind, sqpoll};
        else if constexpr (std::constructible_from<B, BackendKind>)
            return B{kind};
        else {
            (void)kind;
            (void)sqpoll;
            return B{};
        }
    }
};

// The platform's readiness backend (M11-07): kqueue on macOS/BSD, epoll on
// Linux. The dedicated-resolver EM and any place needing a concrete
// readiness backend (not AutoBackend's uring preference) should use this.
#if defined(AFX_HAVE_KQUEUE)
using DefaultPollBackend = KqueueBackend;
#else
using DefaultPollBackend = EpollBackend;
#endif

// The default production configuration: real clock + auto backend selection
// (io_uring where available, epoll otherwise; §9.5; kqueue on macOS/BSD).
// Tests instantiate BasicEventManager<VirtualClock, SimBackend>.
#ifdef AFX_WITH_URING
using EventManager = BasicEventManager<SteadyClock, AutoBackend>;
#else
using EventManager = BasicEventManager<SteadyClock, DefaultPollBackend>;
#endif

}  // namespace afx

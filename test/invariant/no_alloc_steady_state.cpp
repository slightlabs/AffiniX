// Invariant test (IMPLEMENTATION_PLAN.md M2-11): once warmed up, the event
// loop's steady-state hot path must not allocate. Counting happens through a
// global operator-new override that is armed only around the measured phase,
// so doctest's own bookkeeping and all setup allocations are invisible.
//
// Scope note: this exercises the raw-fd path — watch()/readable → recv/send —
// plus timers and mailbox posts, on the real epoll backend. The backend's
// completion path writes into the caller's buffer without heap traffic. The
// connection send path's queued-write case (partial writes) still allocates
// by design until core/pool.hpp (M6-02) lands — see the plan's deviations.

#include <doctest/doctest.h>

#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cstdlib>
#include <new>

#include "../test_env.hpp"
#include "afx/core/event_manager.hpp"

using namespace afx;

namespace {

std::atomic<bool> g_armed{false};
std::atomic<std::size_t> g_allocs{0};

void* counted_alloc(std::size_t n) {
    if (g_armed.load(std::memory_order_relaxed))
        g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

}  // namespace

void* operator new(std::size_t n) {
    return counted_alloc(n);
}
void* operator new[](std::size_t n) {
    return counted_alloc(n);
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    if (g_armed.load(std::memory_order_relaxed))
        g_allocs.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    if (g_armed.load(std::memory_order_relaxed))
        g_allocs.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n ? n : 1);
}
void* operator new(std::size_t n, std::align_val_t a) {
    if (g_armed.load(std::memory_order_relaxed))
        g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = nullptr;
    if (posix_memalign(&p, std::size_t(a), n ? n : 1) != 0)
        throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) {
    return operator new(n, a);
}

void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    std::free(p);
}
// Sized+aligned forms too — an over-aligned deallocation resolves to these;
// leaving them to the runtime would route ASan-malloc-family memory through
// its operator-delete interceptor and trip alloc-dealloc-mismatch.
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}

namespace {

struct ArmGuard {
    ArmGuard() {
        g_allocs.store(0);
        g_armed.store(true);
    }
    ~ArmGuard() { g_armed.store(false); }
};

// One raw-echo iteration: the peer writes, the watched fd sees readable,
// the callback echoes the bytes back.
void pump_echo(EventManager& em, int peer_fd, std::string_view payload) {
    ::send(peer_fd, payload.data(), payload.size(), 0);
    for (int i = 0; i < 64; ++i) em.poll_once();
}

}  // namespace

TEST_CASE("invariant: steady-state loop iterations do not allocate") {
    EventManager em(EventManagerConfig{.wait = WaitStrategy::Spin});

    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0);

    int echoes = 0;
    auto io = em.watch(fds[0], Interest::Readable, [&](IoId, std::int32_t) {
        char buf[8192];
        ssize_t r;
        while ((r = ::recv(fds[0], buf, sizeof buf, 0)) > 0) {
            ::send(fds[0], buf, std::size_t(r), MSG_NOSIGNAL);
            ++echoes;
        }
    });
    REQUIRE(io.has_value());

    // A repeating timer and a mailbox post every iteration, so all stages
    // are hot.
    em.every(1ms, [](TimerCtx) {});

    // Warm-up: grow every vector/table to steady-state capacity.
    for (int i = 0; i < 200; ++i) {
        pump_echo(em, fds[1], "warm");
        (void)em.mailbox().post([] {});
        em.poll_once();
    }

    {
        ArmGuard arm;
        for (int i = 0; i < 500; ++i) {
            pump_echo(em, fds[1], "x");
            (void)em.mailbox().post([] {});
            em.poll_once();
        }
    }
    CHECK(g_allocs.load() == 0);
    CHECK(echoes > 0);

    ::close(fds[1]);
    em.unwatch(*io);
    ::close(fds[0]);
}

#include "afx/sys/crash_dump.hpp"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "afx/core/flight_recorder.hpp"

namespace afx {

namespace {

constexpr std::size_t kMaxRecorders = 512;
std::atomic<const FlightRecorder*> g_recorders[kMaxRecorders];

// Async-signal-safe path building: "afx-crash-<pid>-<sig>.bin\0" assembled by
// hand — snprintf is not on the POSIX async-signal-safe list.
char g_dir[256] = ".";

char* append_dec(char* p, long v) noexcept {
    char tmp[20];
    int n = 0;
    if (v == 0) {
        *p++ = '0';
        return p;
    }
    while (v > 0) {
        tmp[n++] = char('0' + v % 10);
        v /= 10;
    }
    while (n > 0) *p++ = tmp[--n];
    return p;
}

void crash_handler(int sig) noexcept {
    char path[512];
    char* p = path;
    for (const char* d = g_dir; *d && p < path + sizeof(path) - 64; ++d)
        *p++ = *d;
    const char* mid = "/afx-crash-";
    while (*mid) *p++ = *mid++;
    p = append_dec(p, ::getpid());
    *p++ = '-';
    p = append_dec(p, sig);
    const char* ext = ".bin";
    while (*ext) *p++ = *ext++;
    *p = '\0';

    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        for (auto& slot : g_recorders)
            if (const FlightRecorder* r =
                    slot.load(std::memory_order_relaxed))
                r->dump(fd);
        ::close(fd);
    }

    // Re-raise with the default disposition so the core dump still happens.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

}  // namespace

void register_recorder(const FlightRecorder* r) noexcept {
    for (auto& slot : g_recorders) {
        const FlightRecorder* expected = nullptr;
        if (slot.compare_exchange_strong(expected, r,
                                         std::memory_order_relaxed))
            return;
    }
}

void unregister_recorder(const FlightRecorder* r) noexcept {
    for (auto& slot : g_recorders)
        if (slot.load(std::memory_order_relaxed) == r)
            slot.store(nullptr, std::memory_order_relaxed);
}

void install_crash_dump(std::string_view dir) {
    std::size_t n = dir.size() < sizeof(g_dir) - 1 ? dir.size()
                                                 : sizeof(g_dir) - 1;
    std::memcpy(g_dir, dir.data(), n);
    g_dir[n] = '\0';

    struct sigaction sa {};
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    // SA_NODEFER lets a fault inside the handler itself terminate instead of
    // recursing forever on the blocked signal.
    sa.sa_flags = SA_NODEFER;
    ::sigaction(SIGSEGV, &sa, nullptr);
    ::sigaction(SIGABRT, &sa, nullptr);
    ::sigaction(SIGBUS, &sa, nullptr);
}

}  // namespace afx

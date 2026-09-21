#pragma once

// Minimal leveled logging for the framework's own diagnostics (DESIGN.md
// §21). Not a logging subsystem — the async binary LogSink design stays an
// extension (§28.2); this is the gate the M12-05 log-level admin toggle
// controls so runtime warnings (placement, NIC locality, stalls) are
// observable and silenceable without a rebuild.

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace afx {

enum class LogLevel : std::uint8_t { Error = 0, Warn = 1, Info = 2, Debug = 3 };

namespace detail {
inline std::atomic<int>& log_level_atomic() noexcept {
    static std::atomic<int> lvl{int(LogLevel::Info)};
    return lvl;
}
}  // namespace detail

inline LogLevel log_level() noexcept {
    return LogLevel(detail::log_level_atomic().load(std::memory_order_relaxed));
}
inline void set_log_level(LogLevel l) noexcept {
    detail::log_level_atomic().store(int(l), std::memory_order_relaxed);
}

inline void log(LogLevel l, const char* fmt, ...) noexcept {
    if (int(l) > detail::log_level_atomic().load(std::memory_order_relaxed))
        return;
    std::va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}
}  // namespace afx

#define AFX_LOG(l, ...) ::afx::log((l), __VA_ARGS__)

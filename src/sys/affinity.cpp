#include "afx/sys/affinity.hpp"

#include <pthread.h>
#include <unistd.h>
#ifdef __linux__
#include <sched.h>
#endif
#include <cerrno>
#include <cstring>

namespace afx {

Result<void> pin_this_thread(const CoreSet& set) {
    if (set.unrestricted()) return {};
#ifdef __linux__
    cpu_set_t cs;
    CPU_ZERO(&cs);
    for (int c : set.cpus()) CPU_SET(c, &cs);
    if (::sched_setaffinity(0, sizeof(cs), &cs) < 0) return last_errno();
    return {};
#else
    // macOS/BSD have no hard cpu pinning (thread affinity is a hint-only tag
    // mechanism, deliberately not mapped onto a CoreSet). Report it.
    return make_error(ErrorCategory::Config, Err::Unsupported);
#endif
}

Result<void> apply_sched_this_thread(const SchedPolicy& p) {
    return std::visit(
        [](auto&& v) -> Result<void> {
            using T = std::decay_t<decltype(v)>;
            int policy =
                std::is_same_v<T, SchedPolicy::Other>
                    ? SCHED_OTHER
                    : (std::is_same_v<T, SchedPolicy::Fifo> ? SCHED_FIFO
                                                            : SCHED_RR);
            sched_param sp{};
            if constexpr (!std::is_same_v<T, SchedPolicy::Other>)
                sp.sched_priority = v.prio;
            // pthread_setschedparam is portable POSIX; sched_setscheduler is
            // Linux-only.
            if (::pthread_setschedparam(pthread_self(), policy, &sp) != 0)
                return last_errno();
            return {};
        },
        p.v);
}

Result<CoreSet> allowed_cpus() {
#ifdef __linux__
    cpu_set_t cs;
    if (::sched_getaffinity(0, sizeof(cs), &cs) < 0) return last_errno();
    CoreSet s;
    for (int i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &cs)) s.add(i);
    return s;
#else
    // No affinity-mask query: report the full online-CPU range.
    long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) return make_error(ErrorCategory::Config, Err::Unsupported);
    CoreSet s;
    for (int i = 0; i < n; ++i) s.add(i);
    return s;
#endif
}

}  // namespace afx

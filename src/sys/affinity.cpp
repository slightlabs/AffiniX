#include "afx/sys/affinity.hpp"

#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <sched.h>

namespace afx {

Result<void> pin_this_thread(const CoreSet& set) {
    if (set.unrestricted()) return {};
    cpu_set_t cs;
    CPU_ZERO(&cs);
    for (int c : set.cpus()) CPU_SET(c, &cs);
    if (::sched_setaffinity(0, sizeof(cs), &cs) < 0) return last_errno();
    return {};
}

Result<void> apply_sched_this_thread(const SchedPolicy& p) {
    return std::visit(
        [](auto&& v) -> Result<void> {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, SchedPolicy::Other>) {
                sched_param sp{};
                if (::sched_setscheduler(0, SCHED_OTHER, &sp) < 0)
                    return last_errno();
                return {};
            } else {
                int policy = std::is_same_v<T, SchedPolicy::Fifo> ? SCHED_FIFO
                                                                : SCHED_RR;
                sched_param sp{};
                sp.sched_priority = v.prio;
                if (::sched_setscheduler(0, policy, &sp) < 0)
                    return last_errno();
                return {};
            }
        },
        p.v);
}

Result<CoreSet> allowed_cpus() {
    cpu_set_t cs;
    if (::sched_getaffinity(0, sizeof(cs), &cs) < 0) return last_errno();
    CoreSet s;
    for (int i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &cs)) s.add(i);
    return s;
}

} // namespace afx

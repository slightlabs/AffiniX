#include "afx/sim/runtime.hpp"

namespace afx {

void SimRuntime::run_until(TimePoint target) {
    while (now_ < target) {
        // Deliver every event due at the current instant, draining shards
        // between events so causal order lands before the next apply.
        while (net_.due_next(now_)) {
            SimEvent ev = net_.pop();
            trace_.push_back(record(ev));
            apply(std::move(ev));
            drain_all();
            ++steps_;
            if (step_fn_) step_fn_(*this);
        }
        if (drain_all()) continue;  // EM work may have queued due events

        // Quiescent at now_: jump to the earliest pending instant — a net
        // event, a timer, or simply the target.
        TimePoint next = target;
        if (TimePoint nd = net_.next_due(); nd < next) next = nd;
        for (auto& s : shards_)
            if (TimePoint w = s.em->next_wakeup(); w < next) next = w;
        if (next <= now_) next = now_ + Nanos(1);  // guarantee progress
        set_time(next);
        ++steps_;
        if (step_fn_) step_fn_(*this);
    }
    set_time(target);
}

bool SimRuntime::run_until_idle(std::size_t max_steps) {
    for (std::size_t i = 0; i < max_steps; ++i) {
        bool progressed = false;
        while (net_.due_next(now_)) {
            SimEvent ev = net_.pop();
            trace_.push_back(record(ev));
            apply(std::move(ev));
            drain_all();
            ++steps_;
            if (step_fn_) step_fn_(*this);
            progressed = true;
        }
        if (drain_all()) continue;
        if (!progressed) {
            // Quiescent — but timers may still be pending in the future.
            TimePoint next = TimePoint::max();
            for (auto& s : shards_)
                if (TimePoint w = s.em->next_wakeup(); w < next) next = w;
            if (TimePoint nd = net_.next_due(); nd < next) next = nd;
            if (next == TimePoint::max()) return true;  // fully idle
            if (next <= now_) next = now_ + Nanos(1);
            set_time(next);
            ++steps_;
            if (step_fn_) step_fn_(*this);
        }
    }
    return false;
}

}  // namespace afx

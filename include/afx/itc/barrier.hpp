#pragma once

// Barrier — DESIGN.md §11.3. Coordinated reconfiguration: every EM runs
// `per_shard`, then exactly once `when_all` on the EM that arrives last.

#include <atomic>
#include <memory>
#include <span>
#include <vector>

#include "afx/itc/mailbox.hpp"

namespace afx {

class Barrier {
public:
    // Post `per_shard` to every mailbox; when the last shard reports in, run
    // `done` (on that shard's thread).
    template <class F1, class F2>
    static void arrive_all(std::span<const Mailbox> mbs, F1&& per_shard,
                           F2&& done) {
        if (mbs.empty()) return;
        struct State {
            State(std::size_t n, F2&& d)
                : remaining(n), done(std::forward<F2>(d)) {}
            std::atomic<std::size_t> remaining;
            std::decay_t<F2> done;
        };
        auto st = std::make_shared<State>(mbs.size(), std::forward<F2>(done));
        for (const Mailbox& mb : mbs) {
            mb.post([st, shard_fn = per_shard]() mutable {
                shard_fn();
                if (st->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    st->done();
            });
        }
    }
};

} // namespace afx

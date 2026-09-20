#pragma once

// Sharded<T> — DESIGN.md §11.3. One T per EM, each owned by its shard. The
// intended replacement for a mutex-guarded global.

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "afx/itc/mailbox.hpp"

namespace afx {

template <class T>
class Sharded {
    struct alignas(64) Slot {
        Mailbox mb;
        std::unique_ptr<T> value;
        std::size_t index = 0;
    };

public:
    // factory() runs on each shard's thread and returns the shard's T.
    template <class F>
    Sharded(std::vector<Mailbox> mbs, F&& factory)
        : n_(mbs.size()) {
        slots_ = std::unique_ptr<Slot[]>(new Slot[n_]);
        for (std::size_t i = 0; i < n_; ++i) {
            slots_[i].mb = mbs[i];
            slots_[i].index = i;
            Slot* s = &slots_[i];
            // Construct on the owning shard.
            mbs[i].post([s, &factory] {
                s->value = std::make_unique<T>(factory(s->index));
            });
        }
    }

    std::size_t shards() const noexcept { return n_; }

    // fn(T&) runs on shard i's thread.
    template <class F>
    PostResult invoke_on(std::size_t i, F&& fn) const {
        Slot* s = &slots_[i];
        return s->mb.post([s, f = std::forward<F>(fn)]() mutable {
            if (s->value) f(*s->value);
        });
    }

    // map(T&) -> R on every shard; the results are reduced with reduce and
    // delivered to done(R) on the last shard to report.
    template <class Map, class Reduce, class Done>
    void map_reduce(Map&& map, Reduce&& reduce, Done&& done) const {
        using R = std::invoke_result_t<Map&, T&>;
        struct State {
            std::atomic<std::size_t> remaining;
            std::atomic<R> acc;
            std::decay_t<Reduce> reduce;
            std::decay_t<Done> done;
        };
        auto st = std::make_shared<State>(State{
            n_, {}, std::forward<Reduce>(reduce), std::forward<Done>(done)});
        for (std::size_t i = 0; i < n_; ++i) {
            Slot* s = &slots_[i];
            s->mb.post([st, s, map]() mutable {
                if (!s->value) {
                    if (st->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                        st->done(st->acc.load());
                    return;
                }
                R r = map(*s->value);
                // fold into acc (small contention, once per shard)
                R cur = st->acc.load(std::memory_order_relaxed);
                while (!st->acc.compare_exchange_weak(
                    cur, st->reduce(cur, r), std::memory_order_acq_rel)) {}
                if (st->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    st->done(st->acc.load());
            });
        }
    }

private:
    std::size_t n_ = 0;
    std::unique_ptr<Slot[]> slots_;
};

} // namespace afx

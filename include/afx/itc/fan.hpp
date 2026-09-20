#pragma once

// Fan — DESIGN.md §11.3. Topology-aware dispatch over a set of mailboxes:
// round-robin, hash-by-key (shard-affine routing) and broadcast.

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "afx/itc/mailbox.hpp"

namespace afx {

struct HashByTag {};
template <class T, class K>
struct HashBy {
    K T::*member;  // shard key is &T::member
};

enum class FanMode : std::uint8_t { RoundRobin, Hash, Broadcast };

template <class T>
class Fan {
  public:
    Fan(std::vector<Mailbox> mbs, FanMode mode = FanMode::RoundRobin)
        : mbs_(std::move(mbs)), mode_(mode) {}

    // Hash-by-key: all items with the same key land on the same shard.
    template <class K>
    Fan(std::vector<Mailbox> mbs, HashBy<T, K> k)
        : mbs_(std::move(mbs)),
          mode_(FanMode::Hash),
          key_fn_([m = k.member](const T& t) { return std::hash<K>{}(t.*m); }) {
    }

    // dispatch: the handler runs on the target EM with the item.
    template <class F>
    PostResult dispatch(T item, F&& on_item) {
        if (mbs_.empty()) return PostResult::Closed;
        return mbs_[pick(item)].post(
            [i = std::move(item), f = std::forward<F>(on_item)]() mutable {
                f(std::move(i));
            });
    }

    template <class F>
    void broadcast(T item, F&& on_item) {
        for (auto& mb : mbs_)
            mb.post([i = item, f = on_item]() mutable { f(std::move(i)); });
    }

    std::size_t size() const noexcept { return mbs_.size(); }

  private:
    std::size_t pick(const T& item) {
        switch (mode_) {
            case FanMode::Hash:
                if (key_fn_) return key_fn_(item) % mbs_.size();
                [[fallthrough]];
            case FanMode::Broadcast:
            case FanMode::RoundRobin:
            default:
                return rr_.fetch_add(1, std::memory_order_relaxed) %
                       mbs_.size();
        }
    }

    std::vector<Mailbox> mbs_;
    FanMode mode_;
    std::function<std::uint64_t(const T&)> key_fn_;  // config-time only
    std::atomic<std::uint64_t> rr_{0};
};

}  // namespace afx

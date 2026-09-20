#pragma once

// Runtime — owns threads and places EMs onto cores (DESIGN.md §19).
// EventManager is affinity-agnostic; Runtime places it.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "afx/core/event_manager.hpp"
#include "afx/sys/affinity.hpp"
#include "afx/sys/topology.hpp"

namespace afx {

enum class Placement : std::uint8_t {
    OnePerPhysicalCore,  // skip SMT siblings — default for spinning loops
    OnePerLogicalCore,
    Explicit,  // use ThreadConfig::cores round-robin
    None,      // restricted containers: no pinning attempted
};

struct ThreadConfig {
    CoreSet cores{};
    Placement placement = Placement::OnePerPhysicalCore;
    SchedPolicy sched{};
    NumaPolicy numa = NumaPolicy::LocalAlloc;
    EventManagerConfig em{};
};

class Runtime {
  public:
    explicit Runtime(Topology topo) : topo_(std::move(topo)) {}
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    class Group {
      public:
        // Runs fn(EventManager&) on each shard's thread at startup, before
        // the loop begins. Multiple each() registrations compose.
        Group& each(std::function<void(EventManager&)> fn);
        std::vector<Mailbox> mailboxes() const;
        std::size_t size() const noexcept { return count_; }

      private:
        friend class Runtime;
        Group(Runtime* rt, std::size_t begin, std::size_t count)
            : rt_(rt), begin_(begin), count_(count) {}
        Runtime* rt_;
        std::size_t begin_, count_;
    };

    Group spawn_group(std::string name, std::size_t n, ThreadConfig cfg);

    // Signals are handled with signalfd on a designated control EM (§20) —
    // never in an async signal handler.
    void on_signal(std::vector<int> signals, std::function<void()> fn);

    void start();                     // launch all threads
    void join();                      // wait for all threads to exit
    void shutdown(Duration timeout);  // defined drain sequence (§20)

    std::size_t shards() const noexcept { return shards_.size(); }
    std::vector<Mailbox> mailboxes() const;
    EventManager* em(std::size_t i) const;

    // A stop reason is observable from Runtime (§18).
    bool stopping() const noexcept { return stopping_.load(); }

  private:
    struct Shard {
        std::string name;
        ThreadConfig cfg;
        std::vector<std::function<void(EventManager&)>> setup;
        std::thread thread;
        std::atomic<EventManager*> em{nullptr};
        std::atomic<bool> ready{false};
        std::atomic<bool> failed{false};
        Error start_error{};
    };

    void thread_main(Shard& s, int core);
    void signal_thread_main();

    Topology topo_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::vector<std::pair<int, std::function<void()>>> signal_handlers_;
    std::atomic<bool> stopping_{false};
    bool started_ = false;
    std::thread signal_thread_;
    std::atomic<bool> signal_stop_{false};
    int signal_selfpipe_[2] = {-1, -1};
};

}  // namespace afx

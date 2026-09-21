#pragma once

// AdminServer — the introspection endpoint (M12-01). A dedicated
// Block-mode EventManager on its own thread serves a minimal HTTP/1.1
// subset built with AffiniX itself (HttpProto over make_server): the
// endpoint is dogfood for the custom-framing seam and, because it is a
// Block-mode EM on its own thread, provably costs a spinning shard nothing
// when idle.
//
//   AdminServer admin(rt, {.bind = SockAddr::loopback(9100)});
//   admin.start();
//   // GET /stats /metrics /conns /placement /config /flight /healthz
//   // POST /admin/stall_threshold?ns=5000000&shard=0
//   // POST /admin/chaos?on=0&shard=0     POST /admin/log_level?level=warn

#include <atomic>
#include <thread>

#include "afx/admin/http.hpp"
#include "afx/net/sock_addr.hpp"
#include "afx/runtime.hpp"
#include "afx/sys/result.hpp"
#include "afx/sys/types.hpp"

namespace afx::admin {

struct AdminConfig {
    // Loopback by default — this endpoint reads internals and toggles
    // runtime state; binding wider is an explicit opt-in.
    SockAddr bind = SockAddr::loopback(0);
    Duration gather_timeout = 2s;    // cross-shard collect deadline
    Duration request_timeout = 10s;  // bounds the request read and the
                                     // half-close drain (peer FIN wait)
    int backlog = 16;
};

class AdminServer {
  public:
    AdminServer(Runtime& rt, AdminConfig cfg = {}) : rt_(rt), cfg_(cfg) {}
    ~AdminServer() { stop(); }

    AdminServer(const AdminServer&) = delete;
    AdminServer& operator=(const AdminServer&) = delete;

    // Spawns the admin thread. Returns the bound address once listening.
    Result<SockAddr> start();
    void stop() noexcept;

    bool running() const noexcept { return em_.load() != nullptr; }

  private:
    void thread_main();

    Runtime& rt_;
    AdminConfig cfg_;
    std::thread th_;
    std::atomic<EventManager*> em_{nullptr};
    std::atomic<bool> stop_req_{false};  // stop() racing thread_main start
    SockAddr bound_{};                   // written before ready_ release-store
    std::atomic<bool> ready_{false};     // acquire → bound_ readable
    Error start_err_{};
};

}  // namespace afx::admin

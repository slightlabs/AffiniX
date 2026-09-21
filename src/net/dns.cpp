#include "afx/net/dns.hpp"

#include <cstring>

namespace afx {

Resolver::Resolver()
    : em_(EventManagerConfig{.name = "afx-dns", .wait = WaitStrategy::Block},
          SteadyClock{}, DefaultPollBackend{}) {
    thread_ = std::thread([this] { em_.run(); });
}

Resolver::~Resolver() {
    em_.stop();
    if (thread_.joinable()) thread_.join();
}

void Resolver::run_job(Job&& j) {
    // Stale-before-start: the requester's timer will deliver Expired;
    // spending a getaddrinfo here would only delay newer work.
    Duration d = delay_.load(std::memory_order_relaxed);
    if (d.count() > 0) {
        std::this_thread::sleep_for(d);
    }
    AddrList out;
    Error err{};
    if (em_.clock().now() >= j.deadline) {
        // Deadline already passed — drop quietly; the timer owns the report.
    } else {
        addrinfo hints{};
        hints.ai_family = j.family;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        std::string port = std::to_string(j.port);
        if (::getaddrinfo(j.host.c_str(), port.c_str(), &hints, &res) != 0 ||
            !res) {
            err = make_error(ErrorCategory::Net, Err::ResolveFailed);
        } else {
            for (auto* p = res; p; p = p->ai_next) {
                SockAddr a;
                if (p->ai_addrlen <= sizeof(sockaddr_storage)) {
                    std::memcpy(a.addr(), p->ai_addr, p->ai_addrlen);
                    out.push_back(a);
                }
            }
            ::freeaddrinfo(res);
            if (out.empty())
                err = make_error(ErrorCategory::Net, Err::ResolveFailed);
        }
    }

    (void)j.reply_to.post(
        [st = std::move(j.st), out = std::move(out), err]() mutable {
            if (st->done.exchange(true)) return;  // deadline won the race
            if (st->cancel) st->cancel();
            if (st->cb) {
                if (err)
                    st->cb(err);
                else
                    st->cb(Result<AddrList>(std::move(out)));
            }
        });
}

}  // namespace afx

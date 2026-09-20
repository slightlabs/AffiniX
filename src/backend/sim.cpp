#include "afx/backend/sim.hpp"

#include <cerrno>
#include <cstring>
#ifdef AFX_DEBUG_CHAOS
#include <random>
#endif

#include "afx/net/sock_addr.hpp"
#include "afx/sys/clock.hpp"

namespace afx {

Result<void> SimBackend::attach(int fd, Interest i, UserData u) {
    auto& s = fds_[fd];
    s.interest = i;
    s.watch_ud = u;
    return {};
}
Result<void> SimBackend::modify(int fd, Interest i, UserData u) {
    return attach(fd, i, u);
}
Result<void> SimBackend::detach(int fd) {
    fds_.erase(fd);
    return {};
}

Result<void> SimBackend::submit_recv(UserData u, int fd, MutByteSpan buf) {
    auto& s = fds_[fd];
    s.recv_armed = true;
    s.recv_ud = u;
    s.recv_buf = buf;
    pump_recv(fd, s);
    return {};
}

Result<void> SimBackend::submit_send(UserData u, int fd, ByteSpan data) {
    ByteSpan arr[1] = {data};
    return submit_sendv(u, fd, arr);
}

Result<void> SimBackend::submit_sendv(UserData u, int fd,
                                      std::span<const ByteSpan> iov) {
    auto& s = fds_[fd];
    std::size_t total = 0;
    for (auto b : iov) {
        s.outbound.insert(s.outbound.end(), b.begin(), b.end());
        total += b.size();
    }
    push(u, std::int32_t(total));
    return {};
}

Result<void> SimBackend::submit_accept(UserData u, int listen_fd) {
    auto& s = fds_[listen_fd];
    s.accept_armed = true;
    s.accept_ud = u;
    return {};
}

Result<void> SimBackend::submit_connect(UserData u, int fd, const SockAddr&) {
    // Connects succeed immediately unless the harness says otherwise via
    // deliver_watch-style control — the sim models an instant network.
    fds_[fd];  // ensure state exists
    push(u, 0);
    return {};
}

Result<void> SimBackend::cancel(UserData u) {
    for (auto& [fd, s] : fds_) {
        if (s.recv_armed && s.recv_ud == u) {
            s.recv_armed = false;
            push(u, -ECANCELED);
        }
        if (s.accept_armed && s.accept_ud == u) {
            s.accept_armed = false;
            push(u, -ECANCELED);
        }
    }
    return {};
}

int SimBackend::wait(std::span<Completion> out, Nanos) {
    int n = 0;
    while (n < int(out.size()) && !ready_.empty()) {
        out[n++] = ready_.front();
        ready_.pop_front();
    }
    return n;
}

// ---- test / simulation API -------------------------------------------------

int SimBackend::add_fd() {
    fds_[next_fd_];
    return next_fd_++;
}

void SimBackend::feed(int fd, ByteSpan bytes) {
    auto& s = fds_[fd];
    s.inbound.insert(s.inbound.end(), bytes.begin(), bytes.end());
    if (s.recv_armed) pump_recv(fd, s);
}
void SimBackend::feed(int fd, std::string_view sv) {
    feed(fd, {reinterpret_cast<const std::byte*>(sv.data()), sv.size()});
}

void SimBackend::close_peer(int fd) {
    auto& s = fds_[fd];
    s.peer_closed = true;
    if (s.recv_armed) pump_recv(fd, s);
}
void SimBackend::fail_peer(int fd, int err) {
    auto& s = fds_[fd];
    s.peer_err = err;
    if (s.recv_armed) pump_recv(fd, s);
}

void SimBackend::deliver_accept(int listen_fd, int peer_fd) {
    auto& s = fds_[listen_fd];
    fds_[peer_fd];  // peer fd exists
    if (s.accept_armed) push(s.accept_ud, peer_fd);
}

void SimBackend::deliver_watch(int fd, Interest readiness) {
    auto it = fds_.find(fd);
    if (it == fds_.end()) return;
    std::int32_t mask = 0;
    if (has(readiness, Interest::Readable)) mask |= CompletionFlag::ReadyRead;
    if (has(readiness, Interest::Writable)) mask |= CompletionFlag::ReadyWrite;
    push(it->second.watch_ud, mask);
}

const std::vector<std::byte>& SimBackend::sent(int fd) const {
    static const std::vector<std::byte> empty;
    auto it = fds_.find(fd);
    return it == fds_.end() ? empty : it->second.outbound;
}

bool SimBackend::recv_pending(int fd) const {
    auto it = fds_.find(fd);
    return it != fds_.end() && it->second.recv_armed;
}

// ---- internals --------------------------------------------------------------

void SimBackend::push(UserData u, std::int32_t res) {
    Completion c{};
    c.user = u;
    c.result = res;
    c.stamps.tsc = rdtsc();
    ready_.push_back(c);
}

void SimBackend::pump_recv(int, FdState& s) {
    if (!s.recv_armed) return;
    if (s.peer_err) {
        s.recv_armed = false;
        push(s.recv_ud, -s.peer_err);
        return;
    }
    if (s.inbound.empty()) {
        if (s.peer_closed) {
            s.recv_armed = false;
            push(s.recv_ud, 0);
        }
        return;
    }
    std::size_t n = std::min(s.inbound.size(), s.recv_buf.size());
#ifdef AFX_DEBUG_CHAOS
    // M6-13: TCP segmentation is unspecified — deliver a random nonempty
    // prefix so framers see every possible split across runs.
    if (n > 1) {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        n = 1 + std::size_t(rng() % n);
    }
#endif
    for (std::size_t i = 0; i < n; ++i)
        s.recv_buf[i] = s.inbound.front(), s.inbound.pop_front();
    s.recv_armed = false;
    push(s.recv_ud, std::int32_t(n));
}

}  // namespace afx

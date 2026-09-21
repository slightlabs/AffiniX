#include "afx/sim/net.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace afx {

// ---- backend hooks ---------------------------------------------------------

void SimNet::listen(SimBackend& b, int listen_fd) {
    // The listen fd is a real socket created by TcpServer::open — read its
    // bound port so client connects can resolve it by port number.
    sockaddr_storage ss{};
    socklen_t n = sizeof(ss);
    if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&ss), &n) != 0)
        return;
    std::uint16_t port = 0;
    if (ss.ss_family == AF_INET)
        port = ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
    else if (ss.ss_family == AF_INET6)
        port = ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
    if (port) listeners_[port] = {&b, listen_fd};
}

void SimNet::connect(SimBackend& src, int fd, const SockAddr& addr,
                     UserData u) {
    auto it = listeners_.find(addr.port());
    if (it == listeners_.end()) {
        // No listener: ECONNREFUSED after one hop of latency.
        SimEvent ev{};
        ev.due = now_ + latency();
        ev.kind = SimEvent::Kind::ConnectDone;
        ev.dst = &src;
        ev.fd = fd;
        ev.ud = u;
        ev.aux = -ECONNREFUSED;
        push(std::move(ev));
        return;
    }
    auto& [srv_be, listen_fd] = it->second;
    int peer_fd = srv_be->add_fd();  // real socket fd on the server side
    std::size_t link = links_.size();
    links_.push_back(Link{&src, fd, srv_be, peer_fd, false});
    fd_link_[fd] = link;
    fd_link_[peer_fd] = link;

    Nanos half = latency();
    SimEvent conn{};
    conn.due = now_ + half;
    conn.kind = SimEvent::Kind::ConnectDone;
    conn.dst = &src;
    conn.fd = fd;
    conn.ud = u;
    conn.aux = 0;
    push(std::move(conn));

    SimEvent acc{};
    acc.due = now_ + half;
    acc.kind = SimEvent::Kind::Accept;
    acc.dst = srv_be;
    acc.fd = listen_fd;
    acc.aux = peer_fd;
    push(std::move(acc));
}

void SimNet::deliver_bytes(SimBackend& src, int fd,
                           std::span<const ByteSpan> iov) {
    SimBackend* dst = nullptr;
    int dst_fd = -1;
    Link* l = link_for(src, fd, &dst, &dst_fd);
    if (!l) return;  // no link: bytes evaporate (still in sent() for tests)
    if (l->partitioned) {
        ++dropped_;
        return;
    }
    if (faults_.packet_loss > 0 && draw() < faults_.packet_loss) {
        ++dropped_;
        return;
    }
    if (faults_.reset_chance > 0 && draw() < faults_.reset_chance) {
        // RST both directions, delivered after one hop.
        for (int side = 0; side < 2; ++side) {
            SimEvent ev{};
            ev.due = now_ + latency();
            ev.kind = SimEvent::Kind::PeerReset;
            ev.dst = side ? &src : dst;
            ev.fd = side ? fd : dst_fd;
            ev.aux = ECONNRESET;
            push(std::move(ev));
        }
        fd_link_.erase(fd);
        fd_link_.erase(dst_fd);
        return;
    }
    if (faults_.partition_chance > 0 &&
        faults_.partition_window.second > Nanos(0) &&
        draw() < faults_.partition_chance) {
        l->partitioned = true;
        SimEvent ev{};
        ev.kind = SimEvent::Kind::PartitionEnd;
        ev.aux = int(l - links_.data());
        std::int64_t lo = faults_.partition_window.first.count();
        std::int64_t hi = faults_.partition_window.second.count();
        ev.due = now_ + Nanos(lo + std::int64_t(draw() * double(hi - lo)));
        push(std::move(ev));
        // The bytes themselves are lost into the partition.
        ++dropped_;
        return;
    }

    Nanos d = latency();
    if (faults_.slow_peer > 0 && draw() < faults_.slow_peer)
        d += faults_.slow_latency;

    // Ordered stream: this delivery lands no earlier than the previous send
    // on the same direction, so sends can never overtake each other.
    bool a_to_b = (l->a == &src && l->a_fd == fd);
    TimePoint& nf = a_to_b ? l->next_free_ab : l->next_free_ba;
    TimePoint due = now_ + d;
    if (due < nf) due = nf;

    // Flatten the iovec — the send is one ordered byte stream.
    std::size_t total = 0;
    for (auto b : iov) total += b.size();
    if (faults_.partial_reads && total > 1) {
        // Fragment the delivery: 2-4 pieces at successive instants, in
        // stream order (seq orders same-due fragments).
        std::vector<std::byte> flat;
        flat.reserve(total);
        for (auto b : iov) flat.insert(flat.end(), b.begin(), b.end());
        std::size_t frags = 2 + std::size_t(draw() * 3);
        std::size_t off = 0;
        std::size_t i = 0;
        for (; i < frags && off < total; ++i) {
            std::size_t left = total - off;
            std::size_t n =
                i + 1 == frags ? left : 1 + std::size_t(draw() * double(left));
            SimEvent ev{};
            ev.due = due + Nanos(std::int64_t(i));
            ev.kind = SimEvent::Kind::Bytes;
            ev.dst = dst;
            ev.fd = dst_fd;
            ev.bytes.assign(flat.begin() + off, flat.begin() + off + n);
            push(std::move(ev));
            off += n;
        }
        nf = due + Nanos(std::int64_t(i));  // strictly after last fragment
        return;
    }

    SimEvent ev{};
    ev.due = due;
    ev.kind = SimEvent::Kind::Bytes;
    ev.dst = dst;
    ev.fd = dst_fd;
    ev.bytes.reserve(total);
    for (auto b : iov) ev.bytes.insert(ev.bytes.end(), b.begin(), b.end());
    push(std::move(ev));
    nf = due + Nanos(1);
}

void SimNet::detach(SimBackend& src, int fd) {
    // A detached listen fd stops serving: connects to its port then see
    // ECONNREFUSED, like a real socket that went away.
    for (auto it = listeners_.begin(); it != listeners_.end();) {
        if (it->second.first == &src && it->second.second == fd)
            it = listeners_.erase(it);
        else
            ++it;
    }
    auto it = fd_link_.find(fd);
    if (it == fd_link_.end()) return;  // unwired fd — nothing owed
    Link& l = links_[it->second];
    SimBackend* dst;
    int dst_fd;
    if (l.a == &src && l.a_fd == fd) {
        dst = l.b;
        dst_fd = l.b_fd;
    } else {
        dst = l.a;
        dst_fd = l.a_fd;
    }
    fd_link_.erase(fd);
    fd_link_.erase(dst_fd);
    if (!l.partitioned) {
        SimEvent ev{};
        ev.due = now_ + latency();
        ev.kind = SimEvent::Kind::PeerFin;
        ev.dst = dst;
        ev.fd = dst_fd;
        push(std::move(ev));
    }
}

void SimNet::post(std::size_t shard, afx::Task fn, Nanos delay) {
    SimEvent ev{};
    ev.due = now_ + delay;
    ev.kind = SimEvent::Kind::Post;
    ev.shard = shard;
    ev.task = std::move(fn);
    push(std::move(ev));
}

void SimNet::partition(std::size_t link, Nanos window) {
    if (link >= links_.size()) return;
    links_[link].partitioned = true;
    SimEvent ev{};
    ev.due = now_ + window;
    ev.kind = SimEvent::Kind::PartitionEnd;
    ev.aux = int(link);
    push(std::move(ev));
}

void SimNet::apply(const SimEvent& ev) {
    switch (ev.kind) {
        case SimEvent::Kind::Bytes:
            ev.dst->feed(ev.fd, ByteSpan(ev.bytes.data(), ev.bytes.size()));
            break;
        case SimEvent::Kind::Accept:
            ev.dst->deliver_accept(ev.fd, ev.aux);
            break;
        case SimEvent::Kind::ConnectDone:
            ev.dst->deliver_connect(ev.fd, ev.ud, ev.aux);
            break;
        case SimEvent::Kind::PeerFin:
            ev.dst->close_peer(ev.fd);
            break;
        case SimEvent::Kind::PeerReset:
            ev.dst->fail_peer(ev.fd, ev.aux);
            break;
        case SimEvent::Kind::PartitionEnd:
            if (std::size_t(ev.aux) < links_.size())
                links_[std::size_t(ev.aux)].partitioned = false;
            break;
        case SimEvent::Kind::Post:
            break;  // the runtime applies these — it owns the shards
    }
}

SimNet::Link* SimNet::link_for(SimBackend& src, int fd, SimBackend** dst,
                               int* dst_fd) {
    auto it = fd_link_.find(fd);
    if (it == fd_link_.end()) return nullptr;
    Link& l = links_[it->second];
    if (l.a == &src && l.a_fd == fd) {
        *dst = l.b;
        *dst_fd = l.b_fd;
    } else {
        *dst = l.a;
        *dst_fd = l.a_fd;
    }
    return &l;
}

}  // namespace afx

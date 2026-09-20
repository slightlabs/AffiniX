#include "afx/backend/uring.hpp"

#include <linux/io_uring.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "afx/net/sock_addr.hpp"
#include "afx/sys/clock.hpp"

namespace afx {

namespace {

// Internal completion tags: kind byte 0xFF never collides with OpKind.
constexpr std::uint8_t kInternalKind = 0xFF;
constexpr std::uint64_t kWakeTag = UserData::make(kInternalKind, 0, 0).raw;
constexpr std::uint64_t kCancelTag = UserData::make(kInternalKind, 0, 1).raw;
constexpr std::uint64_t kRemoveTag = UserData::make(kInternalKind, 0, 2).raw;

inline bool is_internal(std::uint64_t tag) noexcept {
    return (tag >> 56) == kInternalKind;
}

std::uint32_t to_poll(Interest i) noexcept {
    std::uint32_t e = 0;
    if (has(i, Interest::Readable)) e |= POLLIN;
    if (has(i, Interest::Writable)) e |= POLLOUT;
    return e;
}

std::int32_t to_ready(std::int32_t res) noexcept {
    if (res < 0) return CompletionFlag::ReadyErr;
    std::int32_t m = 0;
    if (res & (POLLIN | POLLRDNORM | POLLRDBAND | POLLPRI))
        m |= CompletionFlag::ReadyRead;
    if (res & (POLLOUT | POLLWRNORM | POLLWRBAND))
        m |= CompletionFlag::ReadyWrite;
    if (res & (POLLERR | POLLNVAL)) m |= CompletionFlag::ReadyErr;
    if (res & (POLLHUP | POLLRDHUP)) m |= CompletionFlag::ReadyHangup;
    return m;
}

inline int uring_setup(unsigned entries, io_uring_params* p) noexcept {
    return int(::syscall(SYS_io_uring_setup, entries, p));
}
inline int uring_enter(int fd, unsigned submit, unsigned min_complete,
                       unsigned flags, const void* arg,
                       std::size_t argsz) noexcept {
    return int(::syscall(SYS_io_uring_enter, fd, submit, min_complete, flags,
                         arg, argsz));
}
inline int uring_register(int fd, unsigned op, const void* arg,
                          unsigned nr) noexcept {
    return int(::syscall(SYS_io_uring_register, fd, op, arg, nr));
}

}  // namespace

// ---------------------------------------------------------------------------
// construction / teardown
// ---------------------------------------------------------------------------

UringBackend::UringBackend() = default;

UringBackend::~UringBackend() {
    teardown();
}

UringBackend::UringBackend(UringBackend&& o) noexcept {
    *this = std::move(o);
}

UringBackend& UringBackend::operator=(UringBackend&& o) noexcept {
    if (this == &o) return *this;
    teardown();
    ring_fd_ = o.ring_fd_;
    o.ring_fd_ = -1;
    wake_fd_ = o.wake_fd_;
    o.wake_fd_ = -1;
    sq_head_ = o.sq_head_;
    sq_tail_ = o.sq_tail_;
    sq_mask_ = o.sq_mask_;
    sq_entries_ = o.sq_entries_;
    sq_array_ = o.sq_array_;
    sqes_ = o.sqes_;
    cq_head_ = o.cq_head_;
    cq_tail_ = o.cq_tail_;
    cq_mask_ = o.cq_mask_;
    cqes_ = o.cqes_;
    o.sq_head_ = o.sq_tail_ = o.sq_mask_ = o.sq_entries_ = nullptr;
    o.sq_array_ = nullptr;
    o.sqes_ = nullptr;
    o.cq_head_ = o.cq_tail_ = o.cq_mask_ = nullptr;
    o.cqes_ = nullptr;
    sq_map_ = o.sq_map_;
    sq_map_sz_ = o.sq_map_sz_;
    cq_map_ = o.cq_map_;
    cq_map_sz_ = o.cq_map_sz_;
    sqes_map_ = o.sqes_map_;
    sqes_map_sz_ = o.sqes_map_sz_;
    o.sq_map_ = o.cq_map_ = o.sqes_map_ = nullptr;
    pending_ = o.pending_;
    o.pending_ = 0;
    watch_ = std::move(o.watch_);
    swallow_ = std::move(o.swallow_);
    armed_accepts_ = std::move(o.armed_accepts_);
    multishot_accept_ = o.multishot_accept_;
    sendv_ = std::move(o.sendv_);
    connect_ = std::move(o.connect_);
    pbuf_ = o.pbuf_;
    o.pbuf_ = nullptr;
    pbuf_map_ = o.pbuf_map_;
    o.pbuf_map_ = nullptr;
    pbuf_map_sz_ = o.pbuf_map_sz_;
    pbuf_backing_ = std::move(o.pbuf_backing_);
    pbuf_mask_ = o.pbuf_mask_;
    pbuf_bgid_ = o.pbuf_bgid_;
    recv_dst_ = std::move(o.recv_dst_);
    return *this;
}

Result<UringBackend> UringBackend::create(const UringConfig& cfg) {
    UringBackend b;
    if (int r = b.init(cfg); r != 0) return errno_error(-r);
    return b;
}

Result<UringCaps> UringBackend::probe() {
    io_uring_params p{};
    int fd = uring_setup(8, &p);
    if (fd < 0) return last_errno();

    std::size_t sz =
        sizeof(io_uring_probe) + IORING_OP_LAST * sizeof(io_uring_probe_op);
    auto* pr = static_cast<io_uring_probe*>(std::calloc(1, sz));
    if (!pr) {
        ::close(fd);
        return errno_error(ENOMEM);
    }
    int r = uring_register(fd, IORING_REGISTER_PROBE, pr, IORING_OP_LAST);
    ::close(fd);
    if (r < 0) {
        std::free(pr);
        return last_errno();
    }

    UringCaps caps;
    for (unsigned i = 0; i < pr->ops_len && i < IORING_OP_LAST; ++i)
        if (pr->ops[i].flags & IO_URING_OP_SUPPORTED)
            caps.supported[i / 32] |= 1u << (i % 32);
    std::free(pr);
    return caps;
}

int UringBackend::init(const UringConfig& cfg) noexcept {
    multishot_accept_ = cfg.multishot_accept;
    io_uring_params p{};
    ring_fd_ = uring_setup(cfg.sq_entries, &p);
    if (ring_fd_ < 0) return -errno;

    std::size_t sq_ring_sz =
        p.sq_off.array + p.sq_entries * sizeof(std::uint32_t);
    std::size_t cq_ring_sz =
        p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
    std::size_t map_sz = sq_ring_sz;
    if (p.features & IORING_FEAT_SINGLE_MMAP)
        map_sz = std::max(sq_ring_sz, cq_ring_sz);

    sq_map_sz_ = map_sz;
    sq_map_ = ::mmap(nullptr, sq_map_sz_, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQ_RING);
    if (sq_map_ == MAP_FAILED) {
        sq_map_ = nullptr;
        goto fail;
    }

    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        cq_map_ = sq_map_;
        cq_map_sz_ = sq_map_sz_;
    } else {
        cq_map_sz_ = cq_ring_sz;
        cq_map_ =
            ::mmap(nullptr, cq_map_sz_, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_CQ_RING);
        if (cq_map_ == MAP_FAILED) {
            cq_map_ = nullptr;
            goto fail;
        }
    }

    sqes_map_sz_ = p.sq_entries * sizeof(io_uring_sqe);
    sqes_map_ = ::mmap(nullptr, sqes_map_sz_, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQES);
    if (sqes_map_ == MAP_FAILED) {
        sqes_map_ = nullptr;
        goto fail;
    }

    {
        auto* base = static_cast<char*>(sq_map_);
        sq_head_ = reinterpret_cast<std::uint32_t*>(base + p.sq_off.head);
        sq_tail_ = reinterpret_cast<std::uint32_t*>(base + p.sq_off.tail);
        sq_mask_ = reinterpret_cast<std::uint32_t*>(base + p.sq_off.ring_mask);
        sq_entries_ =
            reinterpret_cast<std::uint32_t*>(base + p.sq_off.ring_entries);
        sq_array_ = reinterpret_cast<std::uint32_t*>(base + p.sq_off.array);
        sqes_ = static_cast<io_uring_sqe*>(sqes_map_);
    }
    {
        auto* base = static_cast<char*>(cq_map_);
        cq_head_ = reinterpret_cast<std::uint32_t*>(base + p.cq_off.head);
        cq_tail_ = reinterpret_cast<std::uint32_t*>(base + p.cq_off.tail);
        cq_mask_ = reinterpret_cast<std::uint32_t*>(base + p.cq_off.ring_mask);
        cqes_ = reinterpret_cast<io_uring_cqe*>(base + p.cq_off.cqes);
    }

    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) goto fail;

    // Persistent multishot poll on the wake eventfd; completions are filtered.
    {
        io_uring_sqe* s = alloc_sqe();
        if (!s) goto fail;
        s->opcode = IORING_OP_POLL_ADD;
        s->fd = wake_fd_;
        s->poll32_events = POLLIN;
        s->len = IORING_POLL_ADD_MULTI;
        s->user_data = kWakeTag;
    }

    if (cfg.use_pbuf_ring && register_pbuf_ring(cfg) != 0) goto fail;
    return 0;

fail: {
    int e = errno ? errno : EINVAL;
    teardown();
    return -e;
}
}

int UringBackend::register_pbuf_ring(const UringConfig& cfg) noexcept {
    // App-allocated descriptor ring (not the kernel-mmap variant): an
    // anonymous mapping whose address is handed to IORING_REGISTER_PBUF_RING.
    // The register call takes nr_args=1; anything else is EINVAL.
    pbuf_map_sz_ = cfg.pbuf_entries * sizeof(io_uring_buf);
    pbuf_map_ = ::mmap(nullptr, pbuf_map_sz_, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pbuf_map_ == MAP_FAILED) {
        pbuf_map_ = nullptr;
        return -errno;
    }
    pbuf_ = static_cast<io_uring_buf_ring*>(pbuf_map_);

    io_uring_buf_reg reg{};
    reg.ring_addr = std::uint64_t(pbuf_map_);
    reg.ring_entries = cfg.pbuf_entries;
    reg.bgid = cfg.pbuf_bgid;
    if (uring_register(ring_fd_, IORING_REGISTER_PBUF_RING, &reg, 1) < 0)
        return -errno;

    pbuf_backing_.resize(std::size_t(cfg.pbuf_entries) * cfg.pbuf_buf_size);
    pbuf_mask_ = cfg.pbuf_entries - 1;
    pbuf_bgid_ = cfg.pbuf_bgid;
    for (std::uint16_t bid = 0; bid < cfg.pbuf_entries; ++bid)
        provide_buffer(bid);
    return 0;
}

void UringBackend::provide_buffer(std::uint16_t bid) noexcept {
    std::uint16_t tail = pbuf_->tail;
    auto* buf = &pbuf_->bufs[tail & pbuf_mask_];
    buf->addr = std::uint64_t(pbuf_backing_.data() +
                              std::size_t(bid) *
                                  (pbuf_backing_.size() / (pbuf_mask_ + 1)));
    buf->len = std::uint32_t(pbuf_backing_.size() / (pbuf_mask_ + 1));
    buf->bid = bid;
    __atomic_store_n(&pbuf_->tail, std::uint16_t(tail + 1), __ATOMIC_RELEASE);
}

void UringBackend::teardown() noexcept {
    if (ring_fd_ >= 0) {
        if (pbuf_) {
            io_uring_buf_reg reg{};
            reg.bgid = pbuf_bgid_;
            uring_register(ring_fd_, IORING_UNREGISTER_PBUF_RING, &reg, 1);
        }
        ::close(ring_fd_);
        ring_fd_ = -1;
    }
    if (wake_fd_ >= 0) {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }
    bool shared = (cq_map_ == sq_map_);
    if (sq_map_) ::munmap(sq_map_, sq_map_sz_);
    if (cq_map_ && !shared) ::munmap(cq_map_, cq_map_sz_);
    if (sqes_map_) ::munmap(sqes_map_, sqes_map_sz_);
    if (pbuf_map_) ::munmap(pbuf_map_, pbuf_map_sz_);
    sq_map_ = cq_map_ = sqes_map_ = pbuf_map_ = nullptr;
    pbuf_ = nullptr;
}

// ---------------------------------------------------------------------------
// SQE staging and CQ draining
// ---------------------------------------------------------------------------

io_uring_sqe* UringBackend::alloc_sqe() {
    std::uint32_t tail = __atomic_load_n(sq_tail_, __ATOMIC_RELAXED);
    std::uint32_t slot = tail + pending_;
    std::uint32_t head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
    if (slot - head >= *sq_entries_) return nullptr;
    std::uint32_t idx = slot & *sq_mask_;
    sq_array_[idx] = idx;
    ++pending_;
    auto* s = &sqes_[idx];
    std::memset(s, 0, sizeof(*s));
    return s;
}

void UringBackend::commit_sqes() noexcept {
    if (!pending_) return;
    __atomic_store_n(sq_tail_, *sq_tail_ + pending_, __ATOMIC_RELEASE);
    pending_ = 0;
}

Result<void> UringBackend::flush_full_sq() {
    std::uint32_t sub = pending_;
    commit_sqes();
    if (sub && uring_enter(ring_fd_, sub, 0, 0, nullptr, 0) < 0)
        return last_errno();
    return {};
}

int UringBackend::drain_cq(std::span<Completion> out) noexcept {
    std::uint32_t head = __atomic_load_n(cq_head_, __ATOMIC_RELAXED);
    std::uint32_t tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);
    int n = 0;
    while (head != tail && n < int(out.size())) {
        io_uring_cqe cqe = cqes_[head & *cq_mask_];
        ++head;

        if (is_internal(cqe.user_data)) {
            if (cqe.user_data == kWakeTag) {
                std::uint64_t v;
                while (::read(wake_fd_, &v, sizeof(v)) == sizeof(v)) {}
            }
            continue;
        }
        if (cqe.res == -ECANCELED && swallow_.erase(cqe.user_data)) continue;

        Completion& c = out[n++];
        c = Completion{};
        c.user.raw = cqe.user_data;
        if (cqe.flags & IORING_CQE_F_MORE) c.flags |= CompletionFlag::More;
        c.stamps.tsc = rdtsc();

        switch (c.user.kind()) {
            case std::uint8_t(OpKind::Watch):
                c.result = to_ready(cqe.res);
                break;
            case std::uint8_t(OpKind::Send):
                sendv_.erase(cqe.user_data);
                c.result = cqe.res;
                break;
            case std::uint8_t(OpKind::Connect):
                connect_.erase(cqe.user_data);
                c.result = cqe.res;
                break;
            case std::uint8_t(OpKind::Accept):
                if (!(cqe.flags & IORING_CQE_F_MORE))
                    armed_accepts_.erase(cqe.user_data);  // terminal CQE
                c.result = cqe.res;
                break;
            case std::uint8_t(OpKind::Recv): {
                auto it = recv_dst_.find(cqe.user_data);
                if (it != recv_dst_.end()) {
                    // Provided-buffer mode: copy into the caller's span and
                    // hand the buffer back to the ring. The buffer must be
                    // re-provided even on error completions (e.g. -ECANCELED)
                    // or the ring slowly drains empty.
                    if (cqe.flags & IORING_CQE_F_BUFFER) {
                        std::uint16_t bid =
                            cqe.flags >> IORING_CQE_BUFFER_SHIFT;
                        std::size_t buf_sz =
                            pbuf_backing_.size() / (pbuf_mask_ + 1);
                        if (cqe.res > 0) {
                            std::size_t ncp = std::min<std::size_t>(
                                cqe.res, it->second.size());
                            std::memcpy(it->second.data(),
                                        pbuf_backing_.data() +
                                            std::size_t(bid) * buf_sz,
                                        ncp);
                            c.result = std::int32_t(ncp);
                        } else {
                            c.result = cqe.res;
                        }
                        provide_buffer(bid);
                    } else {
                        c.result = cqe.res;
                    }
                    recv_dst_.erase(it);
                } else {
                    c.result = cqe.res;
                }
                break;
            }
            default:
                c.result = cqe.res;
                break;
        }
    }
    __atomic_store_n(cq_head_, head, __ATOMIC_RELEASE);
    return n;
}

// ---------------------------------------------------------------------------
// watch
// ---------------------------------------------------------------------------

Result<void> UringBackend::attach(int fd, Interest i, UserData u) {
    // Re-attach on an already-watched fd: retire the stale poll first so it
    // cannot outlive its sink and emit orphaned completions.
    if (auto it = watch_.find(fd); it != watch_.end()) {
        io_uring_sqe* rm = alloc_sqe();
        if (!rm) {
            if (auto r = flush_full_sq(); !r) return r;
            rm = alloc_sqe();
            if (!rm) return Err::Full;
        }
        rm->opcode = IORING_OP_POLL_REMOVE;
        rm->addr = it->second.raw;
        rm->user_data = kRemoveTag;
    }
    io_uring_sqe* s = alloc_sqe();
    if (!s) {
        if (auto r = flush_full_sq(); !r) return r;
        s = alloc_sqe();
        if (!s) return Err::Full;
    }
    s->opcode = IORING_OP_POLL_ADD;
    s->fd = fd;
    s->poll32_events = to_poll(i);
    s->len = IORING_POLL_ADD_MULTI;
    s->user_data = u.raw;
    watch_[fd] = u;
    return {};
}

Result<void> UringBackend::modify(int fd, Interest i, UserData u) {
    auto it = watch_.find(fd);
    if (it == watch_.end())
        return make_error(ErrorCategory::Internal, Err::NotFound);

    // Replace the poll request: remove old, add new. The old request's
    // terminal -ECANCELED CQE is swallowed so the live sink never sees a
    // spurious error from a successful modify.
    io_uring_sqe* rm = alloc_sqe();
    if (!rm) {
        if (auto r = flush_full_sq(); !r) return r;
        rm = alloc_sqe();
        if (!rm) return Err::Full;
    }
    rm->opcode = IORING_OP_POLL_REMOVE;
    rm->addr = it->second.raw;
    rm->user_data = kRemoveTag;
    swallow_.insert(it->second.raw);

    io_uring_sqe* add = alloc_sqe();
    if (!add) {
        if (auto r = flush_full_sq(); !r) return r;
        add = alloc_sqe();
        if (!add) return Err::Full;
    }
    add->opcode = IORING_OP_POLL_ADD;
    add->fd = fd;
    add->poll32_events = to_poll(i);
    add->len = IORING_POLL_ADD_MULTI;
    add->user_data = u.raw;
    watch_[fd] = u;
    return {};
}

Result<void> UringBackend::detach(int fd) {
    auto it = watch_.find(fd);
    if (it == watch_.end())
        return make_error(ErrorCategory::Internal, Err::NotFound);
    io_uring_sqe* s = alloc_sqe();
    if (!s) {
        if (auto r = flush_full_sq(); !r) return r;
        s = alloc_sqe();
        if (!s) return Err::Full;
    }
    s->opcode = IORING_OP_POLL_REMOVE;
    s->addr = it->second.raw;
    s->user_data = kRemoveTag;
    watch_.erase(it);
    return {};
}

// ---------------------------------------------------------------------------
// I/O ops
// ---------------------------------------------------------------------------

Result<void> UringBackend::submit_recv(UserData u, int fd, MutByteSpan buf) {
    if (pbuf_ && recv_dst_.contains(u.raw))
        return make_error(ErrorCategory::Internal, Err::Invalid);
    io_uring_sqe* s = alloc_sqe();
    if (!s) {
        if (auto r = flush_full_sq(); !r) return r;
        s = alloc_sqe();
        if (!s) return Err::Full;
    }
    s->opcode = IORING_OP_RECV;
    s->fd = fd;
    s->user_data = u.raw;
    if (pbuf_) {
        s->flags |= IOSQE_BUFFER_SELECT;
        s->buf_group = pbuf_bgid_;
        s->len = std::uint32_t(pbuf_backing_.size() / (pbuf_mask_ + 1));
        recv_dst_[u.raw] = buf;
    } else {
        s->addr = reinterpret_cast<std::uint64_t>(buf.data());
        s->len = std::uint32_t(buf.size());
    }
    return {};
}

Result<void> UringBackend::submit_send(UserData u, int fd, ByteSpan data) {
    io_uring_sqe* s = alloc_sqe();
    if (!s) {
        if (auto r = flush_full_sq(); !r) return r;
        s = alloc_sqe();
        if (!s) return Err::Full;
    }
    s->opcode = IORING_OP_SEND;
    s->fd = fd;
    s->addr = reinterpret_cast<std::uint64_t>(data.data());
    s->len = std::uint32_t(data.size());
    s->msg_flags = MSG_NOSIGNAL;
    s->user_data = u.raw;
    return {};
}

Result<void> UringBackend::submit_sendv(UserData u, int fd,
                                        std::span<const ByteSpan> iov) {
    if (iov.size() > 64)
        return make_error(ErrorCategory::Internal, Err::Invalid);
    if (sendv_.contains(u.raw))
        return make_error(ErrorCategory::Internal, Err::Invalid);
    io_uring_sqe* s = alloc_sqe();
    if (!s) {
        if (auto r = flush_full_sq(); !r) return r;
        s = alloc_sqe();
        if (!s) return Err::Full;
    }
    auto& ctx = sendv_[u.raw];
    ctx.msg = msghdr{};
    for (std::size_t i = 0; i < iov.size(); ++i) {
        ctx.iov[i].iov_base = const_cast<std::byte*>(iov[i].data());
        ctx.iov[i].iov_len = iov[i].size();
    }
    ctx.msg.msg_iov = ctx.iov;
    ctx.msg.msg_iovlen = iov.size();
    ctx.msg.msg_flags = MSG_NOSIGNAL;
    s->opcode = IORING_OP_SENDMSG;
    s->fd = fd;
    s->addr = reinterpret_cast<std::uint64_t>(&ctx.msg);
    s->msg_flags = MSG_NOSIGNAL;
    s->user_data = u.raw;
    return {};
}

Result<void> UringBackend::submit_accept(UserData u, int listen_fd) {
    // A multishot accept stays armed across completions; a second submit for
    // the same tag would stack duplicate ops on the listen fd.
    if (multishot_accept_ && armed_accepts_.contains(u.raw))
        return make_error(ErrorCategory::Internal, Err::Invalid);
    io_uring_sqe* s = alloc_sqe();
    if (!s) {
        if (auto r = flush_full_sq(); !r) return r;
        s = alloc_sqe();
        if (!s) return Err::Full;
    }
    s->opcode = IORING_OP_ACCEPT;
    s->fd = listen_fd;
    s->accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
    if (multishot_accept_) {
        s->ioprio |= IORING_ACCEPT_MULTISHOT;
        armed_accepts_.insert(u.raw);
    }
    s->user_data = u.raw;
    return {};
}

Result<void> UringBackend::submit_connect(UserData u, int fd,
                                          const SockAddr& addr) {
    if (connect_.contains(u.raw))
        return make_error(ErrorCategory::Internal, Err::Invalid);
    io_uring_sqe* s = alloc_sqe();
    if (!s) {
        if (auto r = flush_full_sq(); !r) return r;
        s = alloc_sqe();
        if (!s) return Err::Full;
    }
    auto& ctx = connect_[u.raw];
    std::memcpy(&ctx.ss, addr.addr(), addr.len());
    ctx.len = addr.len();
    s->opcode = IORING_OP_CONNECT;
    s->fd = fd;
    s->addr = reinterpret_cast<std::uint64_t>(&ctx.ss);
    s->off = ctx.len;  // addrlen travels in the off/addr2 slot
    s->user_data = u.raw;
    return {};
}

Result<void> UringBackend::cancel(UserData u) {
    io_uring_sqe* s = alloc_sqe();
    if (!s) {
        if (auto r = flush_full_sq(); !r) return r;
        s = alloc_sqe();
        if (!s) return Err::Full;
    }
    s->opcode = IORING_OP_ASYNC_CANCEL;
    s->addr = u.raw;
    s->user_data = kCancelTag;
    return {};
}

// ---------------------------------------------------------------------------
// wait / wake
// ---------------------------------------------------------------------------

int UringBackend::wait(std::span<Completion> out, Nanos timeout) {
    int n = drain_cq(out);
    std::uint32_t sub = pending_;
    commit_sqes();

    bool can_wait = n < int(out.size()) && timeout > Nanos::zero();
    if (sub > 0 || can_wait) {
        if (can_wait) {
            // EXT_ARG: absolute deadline is not needed — relative timespec.
            __kernel_timespec ts{timeout.count() / 1'000'000'000,
                                 timeout.count() % 1'000'000'000};
            io_uring_getevents_arg arg{};
            arg.ts = std::uint64_t(&ts);
            uring_enter(ring_fd_, sub, 1,
                        IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &arg,
                        sizeof(arg));
        } else {
            uring_enter(ring_fd_, sub, 0, 0, nullptr, 0);
        }
        // EINTR / ETIME / errors just mean "drain what arrived".
    }
    if (n < int(out.size())) n += drain_cq(out.subspan(n));
    return n;
}

void UringBackend::wake() {
    std::uint64_t one = 1;
    ssize_t r = ::write(wake_fd_, &one, sizeof(one));
    (void)r;  // EAGAIN: counter saturated → a wake is already pending
}

// ---------------------------------------------------------------------------
// AutoBackend — runtime selection (§9.5)
// ---------------------------------------------------------------------------

AutoBackend::AutoBackend(BackendKind kind) {
    // Probe before committing: ring setup must succeed and every op the
    // backend issues must be supported, else stay on epoll.
    bool ok = kind != BackendKind::Epoll;
    if (ok) {
        auto caps = UringBackend::probe();
        if (!caps) {
            ok = false;
        } else {
            constexpr std::uint8_t required[] = {
                IORING_OP_POLL_ADD,     IORING_OP_POLL_REMOVE,
                IORING_OP_SENDMSG,      IORING_OP_RECV,
                IORING_OP_SEND,         IORING_OP_ACCEPT,
                IORING_OP_ASYNC_CANCEL, IORING_OP_CONNECT,
            };
            for (std::uint8_t op : required)
                if (!caps->supports(op)) {
                    ok = false;
                    break;
                }
        }
    }
    if (ok) {
        if (auto b = UringBackend::create()) {
            impl_.emplace<UringBackend>(std::move(*b));
            kind_ = BackendKind::Uring;
            return;
        }
    }
    kind_ = BackendKind::Epoll;  // variant already holds a default EpollBackend
}

Result<void> AutoBackend::attach(int fd, Interest i, UserData u) {
    return std::visit([&](auto& b) { return b.attach(fd, i, u); }, impl_);
}
Result<void> AutoBackend::modify(int fd, Interest i, UserData u) {
    return std::visit([&](auto& b) { return b.modify(fd, i, u); }, impl_);
}
Result<void> AutoBackend::detach(int fd) {
    return std::visit([&](auto& b) { return b.detach(fd); }, impl_);
}
Result<void> AutoBackend::submit_recv(UserData u, int fd, MutByteSpan buf) {
    return std::visit([&](auto& b) { return b.submit_recv(u, fd, buf); },
                      impl_);
}
Result<void> AutoBackend::submit_send(UserData u, int fd, ByteSpan data) {
    return std::visit([&](auto& b) { return b.submit_send(u, fd, data); },
                      impl_);
}
Result<void> AutoBackend::submit_sendv(UserData u, int fd,
                                       std::span<const ByteSpan> iov) {
    return std::visit([&](auto& b) { return b.submit_sendv(u, fd, iov); },
                      impl_);
}
Result<void> AutoBackend::submit_accept(UserData u, int listen_fd) {
    return std::visit([&](auto& b) { return b.submit_accept(u, listen_fd); },
                      impl_);
}
Result<void> AutoBackend::submit_connect(UserData u, int fd,
                                         const SockAddr& addr) {
    return std::visit([&](auto& b) { return b.submit_connect(u, fd, addr); },
                      impl_);
}
Result<void> AutoBackend::cancel(UserData u) {
    return std::visit([&](auto& b) { return b.cancel(u); }, impl_);
}
int AutoBackend::wait(std::span<Completion> out, Nanos timeout) {
    return std::visit([&](auto& b) { return b.wait(out, timeout); }, impl_);
}
void AutoBackend::wake() {
    std::visit([](auto& b) { b.wake(); }, impl_);
}
int AutoBackend::wake_fd() const noexcept {
    return std::visit([](auto& b) { return b.wake_fd(); }, impl_);
}

}  // namespace afx

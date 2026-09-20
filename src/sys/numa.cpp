#include "afx/sys/numa.hpp"

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

namespace afx::numa {

namespace {

// Raw syscall wrappers — no libnuma (M1-11).
int sys_getcpu(unsigned* cpu, unsigned* node) noexcept {
    return int(::syscall(SYS_getcpu, cpu, node, nullptr));
}
long sys_mbind(void* addr, unsigned long len, int mode,
               const unsigned long* nodemask, unsigned long maxnode,
               unsigned flags) noexcept {
    return ::syscall(SYS_mbind, addr, len, mode, nodemask, maxnode, flags);
}
long sys_get_mempolicy(int* mode, unsigned long* nodemask,
                       unsigned long maxnode, void* addr,
                       unsigned long flags) noexcept {
    return ::syscall(SYS_get_mempolicy, mode, nodemask, maxnode, addr, flags);
}

constexpr int kMpolBind = 2;
constexpr int kMpolInterleave = 3;
constexpr unsigned kMpolMfStrict = 1;

std::size_t page_size() noexcept {
    static const std::size_t ps =
        std::size_t(::sysconf(_SC_PAGESIZE) > 0 ? ::sysconf(_SC_PAGESIZE)
                                              : 4096);
    return ps;
}

void prefault(void* p, std::size_t n, std::size_t page) noexcept {
    // One byte per page: enough to instantiate the mapping. Volatile so the
    // stores are not elided.
    auto* b = static_cast<volatile char*>(p);
    for (std::size_t off = 0; off < n; off += page) b[off] = 0;
}

}  // namespace

bool available() noexcept {
    unsigned long mask[16] = {};
    int mode = 0;
    errno = 0;
    long r = sys_get_mempolicy(&mode, mask, 8 * sizeof(mask), nullptr, 0);
    return r == 0 || errno != ENOSYS;
}

int node_count() noexcept {
    // maxnode of a legal no-op call reports the kernel's node ceiling.
    unsigned long mask[16] = {};
    int mode = 0;
    long r = sys_get_mempolicy(&mode, mask, 8 * sizeof(mask), nullptr, 0);
    (void)r;
    // Count the bits the kernel could possibly set: nodemask comes back as
    // the allowed-node mask for MPOL_BIND-style queries in practice; the
    // portable floor is 1.
    int n = 0;
    for (unsigned long w : mask) n += __builtin_popcountl(w);
    return n > 0 ? n : 1;
}

int current_node() noexcept {
    unsigned cpu = 0, node = 0;
    if (sys_getcpu(&cpu, &node) != 0) return -1;
    return int(node);
}

Result<void> bind_to_node(void* p, std::size_t n, int node) noexcept {
    if (node < 0) return {};
    unsigned long mask[16] = {};
    if (std::size_t(node) < 8 * sizeof(mask)) mask[node / 64] |= 1ul << (node % 64);
    if (sys_mbind(p, n, kMpolBind, mask, 8 * sizeof(mask),
                  kMpolMfStrict) != 0)
        return last_errno();
    return {};
}

Region::~Region() {
    if (!p_) return;
    if (mapped_)
        ::munmap(p_, n_);
    else
        std::free(p_);
    p_ = nullptr;
}

Region& Region::operator=(Region&& o) noexcept {
    if (this == &o) return *this;
    this->~Region();
    p_ = o.p_;
    n_ = o.n_;
    mapped_ = o.mapped_;
    hugepages_ = o.hugepages_;
    o.p_ = nullptr;
    o.n_ = 0;
    return *this;
}

Result<Region> alloc(std::size_t bytes, const AllocOpts& o) {
    if (bytes == 0) return Region{};
    const std::size_t page = page_size();
    bytes = (bytes + page - 1) / page * page;

    void* p = nullptr;
    bool huge = false;

#ifdef __linux__
    if (o.hugepages) {
        // 2 MiB pages: needs a configured hugepage pool; failure is expected
        // on unconfigured hosts and falls through to the THP hint.
        std::size_t hsz = (bytes + (2u << 20) - 1) / (2u << 20) * (2u << 20);
        void* h = ::mmap(nullptr, hsz, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB |
                             (21 << MAP_HUGE_SHIFT),
                         -1, 0);
        if (h != MAP_FAILED) {
            p = h;
            bytes = hsz;
            huge = true;
        }
    }
#endif
    if (!p) {
        p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) p = nullptr;
    }
    if (!p) return errno_error(ENOMEM);

    Region r;
    r.p_ = p;
    r.n_ = bytes;
    r.mapped_ = true;
    r.hugepages_ = huge;

#ifdef __linux__
    if (!huge && o.hugepages)
        (void)::madvise(p, bytes, MADV_HUGEPAGE);  // best-effort THP

    if (o.interleave && available()) {
        unsigned long all[16];
        std::memset(all, 0xFF, sizeof(all));
        (void)sys_mbind(p, bytes, kMpolInterleave, all, 8 * sizeof(all), 0);
    } else if (o.node >= 0) {
        (void)bind_to_node(p, bytes, o.node);  // best-effort: report via stats
    }
#endif

    if (o.prefault) prefault(p, bytes, huge ? (2u << 20) : page);
    return r;
}

}  // namespace afx::numa

#pragma once

// IoBuffer — DESIGN.md §12.1.
// Layout: [ free headroom | readable | writable ]. prepend() writes a header
// in front of a serialised body without a copy.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>

#include "afx/sys/types.hpp"

namespace afx {

class IoBuffer {
public:
    explicit IoBuffer(std::size_t cap = 64u << 10, std::size_t headroom = 64)
        : cap_(std::max(cap, headroom + 1)),
          buf_(std::make_unique<std::byte[]>(cap_)),
          headroom_(headroom),
          start_(headroom) {}

    ByteSpan readable() const noexcept { return {buf_.get() + start_, size_}; }
    MutByteSpan readable_mut() noexcept { return {buf_.get() + start_, size_}; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    std::size_t capacity() const noexcept { return cap_; }

    // Writable region with at least `at_least` bytes, compacting/growing.
    MutByteSpan writable(std::size_t at_least) {
        ensure_writable(at_least);
        return {buf_.get() + start_ + size_, cap_ - start_ - size_};
    }

    void commit(std::size_t n) {
        assert(start_ + size_ + n <= cap_);
        size_ += n;
    }

    void consume(std::size_t n) {
        n = std::min(n, size_);
        start_ += n;
        size_ -= n;
        if (size_ == 0) start_ = headroom_;  // fully drained: reset cheaply
    }

    // Reserve n bytes immediately in front of the readable region and fold
    // them into it. Returns the span to write into.
    MutByteSpan prepend(std::size_t n) {
        if (start_ < n) {
            // Shift data right so at least n bytes precede it.
            std::size_t new_start = std::max(n, headroom_);
            if (new_start + size_ > cap_) grow(new_start + size_);
            std::memmove(buf_.get() + new_start, buf_.get() + start_, size_);
            start_ = new_start;
        }
        start_ -= n;
        size_ += n;
        return {buf_.get() + start_, n};
    }

    void reserve(std::size_t n) { if (n > cap_) grow(n); }

    void compact() {
        if (start_ == headroom_) return;
        std::memmove(buf_.get() + headroom_, buf_.get() + start_, size_);
        start_ = headroom_;
    }

private:
    void ensure_writable(std::size_t at_least) {
        if (cap_ - start_ - size_ >= at_least) return;
        // Reclaim the consumed prefix first; grow only if still short.
        compact();
        if (cap_ - headroom_ - size_ < at_least)
            grow(headroom_ + size_ + at_least);
    }

    void grow(std::size_t need) {
        std::size_t ncap = cap_ ? cap_ * 2 : 64;
        while (ncap < need) ncap *= 2;
        auto nb = std::make_unique<std::byte[]>(ncap);
        std::memcpy(nb.get() + headroom_, buf_.get() + start_, size_);
        buf_ = std::move(nb);
        cap_ = ncap;
        start_ = headroom_;
    }

    std::size_t cap_ = 0;
    std::unique_ptr<std::byte[]> buf_;
    std::size_t headroom_ = 0;
    std::size_t start_ = 0;  // index of first readable byte
    std::size_t size_ = 0;   // readable byte count
};

} // namespace afx

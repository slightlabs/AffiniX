#pragma once

// InlineFn<Sig, N> — move-only callable with N bytes of inline storage and a
// heap fallback for larger callables (DESIGN.md §2.2: no std::function, and
// the heap fallback makes the cost visible rather than surprising).

#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace afx {

template <class Sig, std::size_t N = 48>
class InlineFn;

template <class R, class... A, std::size_t N>
class InlineFn<R(A...), N> {
    struct Ops {
        R (*invoke)(void*, A&&...);
        void (*destroy)(void*) noexcept;
        void (*move)(void* dst, void* src) noexcept;
    };

    template <class F>
    static const Ops* ops_for() noexcept {
        static const Ops ops{
            [](void* p, A&&... a) -> R {
                if constexpr (std::is_void_v<R>) {
                    (*static_cast<F*>(p))(std::forward<A>(a)...);
                } else {
                    return (*static_cast<F*>(p))(std::forward<A>(a)...);
                }
            },
            [](void* p) noexcept { static_cast<F*>(p)->~F(); },
            [](void* dst, void* src) noexcept {
                new (dst) F(std::move(*static_cast<F*>(src)));
                static_cast<F*>(src)->~F();
            },
        };
        return &ops;
    }

public:
    InlineFn() noexcept = default;
    InlineFn(std::nullptr_t) noexcept {}

    template <class F>
        requires (!std::is_same_v<std::decay_t<F>, InlineFn> &&
                  std::is_invocable_r_v<R, F&, A...>)
    InlineFn(F&& f) {
        using D = std::decay_t<F>;
        if constexpr (sizeof(D) <= N && std::is_nothrow_move_constructible_v<D>) {
            new (buf_) D(std::forward<F>(f));
            ops_ = ops_for<D>();
            heap_ = false;
        } else {
            auto* p = new D(std::forward<F>(f));
            *reinterpret_cast<D**>(buf_) = p;
            ops_ = heap_ops<D>();
            heap_ = true;
        }
    }

    InlineFn(InlineFn&& o) noexcept { move_from(o); }
    InlineFn& operator=(InlineFn&& o) noexcept {
        if (this != &o) { reset(); move_from(o); }
        return *this;
    }
    InlineFn(const InlineFn&) = delete;
    InlineFn& operator=(const InlineFn&) = delete;

    ~InlineFn() { reset(); }

    explicit operator bool() const noexcept { return ops_ != nullptr; }

    R operator()(A... a) {
        if constexpr (std::is_void_v<R>) {
            ops_->invoke(ptr(), std::forward<A>(a)...);
        } else {
            return ops_->invoke(ptr(), std::forward<A>(a)...);
        }
    }

    void reset() noexcept {
        if (!ops_) return;
        if (heap_) {
            ops_->destroy(*reinterpret_cast<void**>(buf_));
        } else {
            ops_->destroy(buf_);
        }
        ops_ = nullptr;
    }

    bool heap_allocated() const noexcept { return heap_; }

private:
    void* ptr() noexcept {
        return heap_ ? *reinterpret_cast<void**>(buf_) : static_cast<void*>(buf_);
    }

    void move_from(InlineFn& o) noexcept {
        ops_ = o.ops_;
        heap_ = o.heap_;
        if (!ops_) return;
        if (heap_) {
            *reinterpret_cast<void**>(buf_) = *reinterpret_cast<void**>(o.buf_);
        } else {
            ops_->move(buf_, o.buf_);
        }
        o.ops_ = nullptr;
    }

    template <class F>
    static const Ops* heap_ops() noexcept {
        static const Ops ops{
            [](void* p, A&&... a) -> R {
                if constexpr (std::is_void_v<R>) {
                    (*static_cast<F*>(p))(std::forward<A>(a)...);
                } else {
                    return (*static_cast<F*>(p))(std::forward<A>(a)...);
                }
            },
            [](void* p) noexcept { delete static_cast<F*>(p); },
            [](void*, void*) noexcept {},
        };
        return &ops;
    }

    alignas(std::max_align_t) unsigned char buf_[N > sizeof(void*) ? N : sizeof(void*)]{};
    const Ops* ops_ = nullptr;
    bool heap_ = false;
};

} // namespace afx

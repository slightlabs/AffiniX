#pragma once

// Endianness is the application's (DESIGN.md §14.2); AffiniX just provides
// the helpers.

#include <bit>
#include <concepts>
#include <cstdint>

namespace afx {

template <std::unsigned_integral T>
constexpr T byteswap(T v) noexcept {
    if constexpr (sizeof(T) == 1)
        return v;
    else if constexpr (sizeof(T) == 2)
        return static_cast<T>(__builtin_bswap16(v));
    else if constexpr (sizeof(T) == 4)
        return static_cast<T>(__builtin_bswap32(v));
    else
        return static_cast<T>(__builtin_bswap64(v));
}

template <std::unsigned_integral T>
constexpr T le(T v) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        return v;
    else
        return byteswap(v);
}
template <std::unsigned_integral T>
constexpr T be(T v) noexcept {
    if constexpr (std::endian::native == std::endian::big)
        return v;
    else
        return byteswap(v);
}

constexpr std::uint16_t le16(std::uint16_t v) noexcept {
    return le(v);
}
constexpr std::uint32_t le32(std::uint32_t v) noexcept {
    return le(v);
}
constexpr std::uint64_t le64(std::uint64_t v) noexcept {
    return le(v);
}
constexpr std::uint16_t be16(std::uint16_t v) noexcept {
    return be(v);
}
constexpr std::uint32_t be32(std::uint32_t v) noexcept {
    return be(v);
}
constexpr std::uint64_t be64(std::uint64_t v) noexcept {
    return be(v);
}

}  // namespace afx

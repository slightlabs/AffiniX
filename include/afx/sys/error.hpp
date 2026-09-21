#pragma once

#include <cerrno>
#include <cstdint>
#include <string_view>

namespace afx {

// DESIGN.md §18: 4 bytes, no exceptions on the data path, no raw errno in
// user code. Sys-category errors carry the errno value as `code`.
enum class ErrorCategory : std::uint16_t {
    Ok = 0,
    Sys,        // errno-derived
    Net,        // connect refused, reset, dns...
    Frame,      // protocol/framing errors
    Config,     // bad configuration
    Itc,        // mailbox full, queue overflow...
    Internal,   // framework invariant violated
    Cancelled,  // deadline expired / stopped
};

struct Error {
    std::uint16_t code = 0;
    ErrorCategory category = ErrorCategory::Ok;

    constexpr bool ok() const noexcept { return category == ErrorCategory::Ok; }
    constexpr explicit operator bool() const noexcept { return !ok(); }

    std::string_view message() const noexcept;

    constexpr bool operator==(const Error&) const = default;
};

static_assert(sizeof(Error) == 4);

// Framework error codes (code field when category != Sys).
enum class Err : std::uint16_t {
    None = 0,
    Full,
    Closed,
    Stopped,
    Expired,
    NotFound,
    Invalid,
    WouldBlock,
    Overflow,
    BadMagic,
    UnsupportedVersion,
    FrameTooLarge,
    ConnectFailed,
    ResolveFailed,
    NoResources,
    PermissionDenied,
    Unsupported,
    Truncated,  // datagram ended mid-frame (UDP partial-message tail)
};

constexpr Error make_error(ErrorCategory c, std::uint16_t code) noexcept {
    return Error{code, c};
}
constexpr Error make_error(ErrorCategory c, Err e) noexcept {
    return Error{static_cast<std::uint16_t>(e), c};
}
inline Error errno_error(int e) noexcept {
    return Error{static_cast<std::uint16_t>(e & 0xFFFF), ErrorCategory::Sys};
}
inline Error last_errno() noexcept {
    return errno_error(errno);
}

}  // namespace afx

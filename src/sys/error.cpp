#include "afx/sys/error.hpp"

#include <cstring>

namespace afx {

std::string_view Error::message() const noexcept {
    if (category == ErrorCategory::Sys)
        return std::string_view(std::strerror(code));
    if (category == ErrorCategory::Ok) return "ok";

    static constexpr std::string_view names[] = {
        "none",
        "full",
        "closed",
        "stopped",
        "expired",
        "not found",
        "invalid",
        "would block",
        "overflow",
        "bad magic",
        "unsupported version",
        "frame too large",
        "connect failed",
        "resolve failed",
        "no resources",
        "permission denied",
        "unsupported",
    };
    if (code < std::size(names)) return names[code];
    return "unknown error";
}

}  // namespace afx

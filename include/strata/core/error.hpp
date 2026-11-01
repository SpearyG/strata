#pragma once

#include <cstdint>
#include <string>

namespace strata {
enum class errc {
    invalid_config, invalid_event, full, closed, cancelled,
    io_error, corrupt, truncated, unsupported
};
struct error {
    errc code;
    std::string message;
    std::uint64_t offset{};
};
} // namespace strata

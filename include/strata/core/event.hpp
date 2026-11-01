#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>

namespace strata {
struct event {
    std::uint64_t sequence{};
    std::uint32_t series{};
    double value{};
    std::uint64_t timestamp_ns{};
    friend bool operator==(const event&, const event&) = default;
};

[[nodiscard]] inline bool valid(const event& e, std::uint32_t series_count) noexcept {
    return e.series < series_count && std::isfinite(e.value) && std::abs(e.value) <= 1.0e100;
}
[[nodiscard]] inline std::uint64_t steady_now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace strata

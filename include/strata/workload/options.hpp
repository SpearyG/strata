#pragma once

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <strata/core/error.hpp>
#include <strata/workload/workload.hpp>

namespace strata::workload {
template <std::unsigned_integral T>
[[nodiscard]] std::expected<T, error> parse_unsigned(std::string_view text) {
    T value{};
    const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || code != std::errc{} || end != text.data() + text.size()) {
        return std::unexpected(error{errc::invalid_config, "expected an unsigned decimal integer: " + std::string(text)});
    }
    return value;
}

// The caller owns argument traversal; this only handles shared workload switches.
[[nodiscard]] inline std::expected<void, error> set_option(
    options& settings, std::string_view name, std::string_view value) {
    const auto number = parse_unsigned<std::uint64_t>(value);
    if (!number) return std::unexpected(number.error());
    const auto n = *number;
    if (name == "--events") settings.events = n;
    else if (name == "--seed") settings.seed = n;
    else if (name == "--burst") settings.burst = n;
    else if (name == "--workers" || name == "--producers" || name == "--queue-capacity" || name == "--batch-size") {
        if (n > 16'777'216) return std::unexpected(error{errc::invalid_config, "option exceeds size limit"});
        const auto size = static_cast<std::size_t>(n);
        if (name == "--workers") settings.runtime.workers = size;
        else if (name == "--producers") settings.producers = size;
        else if (name == "--queue-capacity") settings.runtime.queue_capacity = size;
        else settings.runtime.batch_size = size;
    } else if (name == "--series") {
        if (n > 1'048'576) return std::unexpected(error{errc::invalid_config, "series exceeds 1048576"});
        settings.runtime.series_count = static_cast<std::uint32_t>(n);
    } else if (name == "--work") {
        if (n > 100'000) return std::unexpected(error{errc::invalid_config, "work exceeds 100000 rounds"});
        settings.work = static_cast<std::uint32_t>(n);
    } else if (name == "--pause-us") {
        if (n > 1'000'000) return std::unexpected(error{errc::invalid_config, "pause exceeds one second"});
        settings.pause = std::chrono::microseconds(static_cast<std::chrono::microseconds::rep>(n));
    } else return std::unexpected(error{errc::invalid_config, "unknown option: " + std::string(name)});
    return {};
}
inline constexpr std::string_view shared_help =
    "  --workers N          worker shards (1..256; default 4)\n"
    "  --producers N        generating threads (1..256; default 4)\n"
    "  --events N           total generated events (default 1000000)\n"
    "  --queue-capacity N   bounded events per shard (default 4096)\n"
    "  --batch-size N       maximum dequeue batch (default 64)\n"
    "  --series N           distinct telemetry keys (default 1024)\n"
    "  --seed N             deterministic generator seed (default 42)\n"
    "  --work N             observable producer mixing rounds (default 0)\n"
    "  --burst N            pause each producer after N events; 0=sustained\n"
    "  --pause-us N         inter-burst pause (default 1000 microseconds)\n"
    "  --no-latency         disable latency clock reads and histogram\n";
} // namespace strata::workload

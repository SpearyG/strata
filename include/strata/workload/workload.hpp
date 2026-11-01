#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iosfwd>
#include <stop_token>
#include <strata/core/config.hpp>
#include <strata/core/event.hpp>
#include <strata/runtime/engine.hpp>

namespace strata::workload {
struct options {
    config runtime;
    std::size_t producers{4};
    std::uint64_t events{1'000'000};
    std::uint64_t seed{42};
    std::uint32_t work{0};
    std::uint64_t burst{0};
    std::chrono::microseconds pause{1000};
};

struct run_result {
    engine_snapshot metrics;
    std::chrono::nanoseconds elapsed{};
    std::uint64_t generated{};
    std::uint64_t fingerprint{};
    bool cancelled{};
    [[nodiscard]] double events_per_second() const noexcept;
};

// Each sample depends only on sequence, seed and configuration, never scheduling.
[[nodiscard]] event generate(std::uint64_t sequence, const options& settings) noexcept;
[[nodiscard]] std::expected<void, error> validate(const options& settings);
[[nodiscard]] std::expected<run_result, error> run(const options& settings,
                                                 std::stop_token stop = {});
void write_json(std::ostream& out, const options& settings, const run_result& result);
} // namespace strata::workload

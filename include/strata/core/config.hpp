#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <strata/core/error.hpp>

namespace strata {
struct config {
    std::size_t workers{4};
    std::size_t queue_capacity{4096}; // per shard
    std::size_t batch_size{64};
    std::uint32_t series_count{1024};
    bool measure_latency{true};
};
[[nodiscard]] inline std::expected<void, error> validate(const config& c) {
    if (c.workers == 0 || c.workers > 256 || c.queue_capacity == 0 ||
        c.queue_capacity > (1U << 24U) || c.batch_size == 0 ||
        c.batch_size > c.queue_capacity || c.series_count == 0 || c.series_count > (1U << 20U)) {
        return std::unexpected(error{errc::invalid_config, "invalid worker, queue, batch or series limit"});
    }
    // Reject excessive allocations before constructing any worker or queue.
    if (c.workers * c.queue_capacity > (1U << 24U)) {
        return std::unexpected(error{errc::invalid_config, "total queue capacity exceeds 16777216 events"});
    }
    return {};
}
} // namespace strata

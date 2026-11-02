#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <stop_token>
#include <vector>

#include <strata/core/config.hpp>
#include <strata/core/error.hpp>
#include <strata/core/event.hpp>
#include <strata/metrics/latency_histogram.hpp>

namespace strata {

struct series_summary {
    std::uint32_t series{};
    std::uint64_t count{};
    double mean{};
    double m2{}; // Sum of squared deviations; sample variance = m2 / (count-1).
    double minimum{};
    double maximum{};
};

struct engine_snapshot {
    std::uint64_t accepted{};
    std::uint64_t processed{};
    std::uint64_t rejected{};
    std::uint64_t discarded{};
    // Sum of each queue's peak depth, an upper bound on simultaneous queued work.
    std::size_t queue_high_watermark{};
    metrics::latency_histogram latency;
    std::vector<series_summary> series;
};

class engine final {
public:
    // Submission and shutdown may run concurrently. Shutdown joins owned workers;
    // callers must finish their own API calls before destroying this object.
    // snapshot() requires completed shutdown. A submitted stop token is borrowed
    // only until that synchronous submit call returns; no reference is retained.
    // Configuration errors are values. Allocation failure may throw bad_alloc;
    // worker startup system errors are translated into io_error.
    [[nodiscard]] static std::expected<std::unique_ptr<engine>, error> create(config configuration);
    ~engine();
    engine(const engine&) = delete;
    engine& operator=(const engine&) = delete;
    engine(engine&&) = delete;
    engine& operator=(engine&&) = delete;

    [[nodiscard]] std::expected<void, error> submit(event item, const std::stop_token& stop = {});
    [[nodiscard]] std::expected<void, error> try_submit(event item);
    void close() noexcept;
    void cancel() noexcept;
    [[nodiscard]] std::expected<engine_snapshot, error> snapshot() const;
    [[nodiscard]] const config& configuration() const noexcept;

private:
    struct implementation;
    explicit engine(config configuration);
    std::unique_ptr<implementation> impl_;
};
} // namespace strata

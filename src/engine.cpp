#include <strata/runtime/engine.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

#include <strata/concurrency/bounded_channel.hpp>
#include <strata/pipeline/pipeline.hpp>

namespace strata {
namespace {
struct queued_event {
    event item;
    std::uint64_t attempted_ns{};
};

void update(series_summary& summary, double value) noexcept {
    if (summary.count == 0) {
        summary.minimum = value;
        summary.maximum = value;
    } else {
        summary.minimum = std::min(summary.minimum, value);
        summary.maximum = std::max(summary.maximum, value);
    }
    ++summary.count;
    const auto delta = value - summary.mean;
    summary.mean += delta / static_cast<double>(summary.count);
    summary.m2 += delta * (value - summary.mean);
}

// Each worker writes only its own shard, away from other workers' metadata.
struct alignas(64) shard {
    shard(const config& c, std::size_t id)
        : queue(c.queue_capacity), batch(c.batch_size) {
        const auto keys = static_cast<std::size_t>(c.series_count);
        const auto size = id < keys ? (keys - 1 - id) / c.workers + 1 : 0;
        summaries.resize(size);
        for (std::size_t i = 0; i < size; ++i) {
            summaries[i].series = static_cast<std::uint32_t>(id + i * c.workers);
        }
    }
    concurrency::bounded_channel<queued_event> queue;
    std::vector<queued_event> batch;
    std::vector<series_summary> summaries;
    metrics::latency_histogram latency;
    std::uint64_t processed{};
};
} // namespace

struct engine::implementation {
    explicit implementation(config c) : configuration(c) {
        shards.reserve(c.workers);
        workers.reserve(c.workers);
        for (std::size_t i = 0; i < c.workers; ++i) {
            shards.push_back(std::make_unique<shard>(c, i));
        }
    }

    void start() {
        for (auto& owner : shards) {
            auto* target = owner.get();
            workers.emplace_back([this, target] { consume(*target); });
        }
    }
    void consume(shard& target) noexcept {
        for (;;) {
            const auto count = target.queue.pop_batch(std::span(target.batch));
            if (count == 0) {
                return;
            }
            for (std::size_t i = 0; i < count; ++i) {
                const auto& work = target.batch[i];
                const auto index = static_cast<std::size_t>(work.item.series) / configuration.workers;
                update(target.summaries[index], work.item.value);
                ++target.processed;
                if (configuration.measure_latency) {
                    const auto now = steady_now_ns();
                    target.latency.observe(now >= work.attempted_ns ? now - work.attempted_ns : 0);
                }
            }
        }
    }
    void finish(bool discard) noexcept {
        // Closing queues precedes join serialization: cancel can escalate a drain
        // without waiting for the draining caller to release join_mutex.
        for (auto& target : shards) {
            if (discard) {
                target->queue.cancel();
            } else {
                target->queue.close();
            }
        }
        const std::lock_guard lock(join_mutex);
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        joined = true;
    }

    const config configuration;
    std::vector<std::unique_ptr<shard>> shards;
    std::vector<std::jthread> workers;
    // Invalid inputs never enter a channel, so they have a separate cold counter.
    std::atomic<std::uint64_t> invalid_rejected{};
    mutable std::mutex join_mutex;
    bool joined{}; // guarded by join_mutex; worker state read only after join
};

engine::engine(config configuration) : impl_(std::make_unique<implementation>(configuration)) {}
engine::~engine() { close(); }

std::expected<std::unique_ptr<engine>, error> engine::create(config configuration) {
    if (auto result = validate(configuration); !result) {
        return std::unexpected(std::move(result.error()));
    }
    auto result = std::unique_ptr<engine>(new engine(configuration));
    try {
        result->impl_->start();
    } catch (const std::system_error& failure) {
        // The partially started engine's destructor closes and joins its workers.
        return std::unexpected(error{errc::io_error, std::string("worker startup failed: ") + failure.what()});
    }
    return result;
}

std::expected<void, error> engine::submit(event item, const std::stop_token& stop) {
    const auto attempted = impl_->configuration.measure_latency ? steady_now_ns() : 0;
    auto validation = pipeline{validate_event{impl_->configuration.series_count}};
    if (!validation(item)) {
        impl_->invalid_rejected.fetch_add(1, std::memory_order_relaxed);
        return std::unexpected(error{errc::invalid_event, "event outside valid domain"});
    }
    const auto index = static_cast<std::size_t>(item.series) % impl_->configuration.workers;
    return impl_->shards[index]->queue.push(queued_event{item, attempted}, stop);
}

std::expected<void, error> engine::try_submit(event item) {
    const auto attempted = impl_->configuration.measure_latency ? steady_now_ns() : 0;
    auto validation = pipeline{validate_event{impl_->configuration.series_count}};
    if (!validation(item)) {
        impl_->invalid_rejected.fetch_add(1, std::memory_order_relaxed);
        return std::unexpected(error{errc::invalid_event, "event outside valid domain"});
    }
    const auto index = static_cast<std::size_t>(item.series) % impl_->configuration.workers;
    return impl_->shards[index]->queue.try_push(queued_event{item, attempted});
}

void engine::close() noexcept { impl_->finish(false); }
void engine::cancel() noexcept { impl_->finish(true); }
const config& engine::configuration() const noexcept { return impl_->configuration; }

std::expected<engine_snapshot, error> engine::snapshot() const {
    const std::lock_guard lock(impl_->join_mutex);
    if (!impl_->joined) {
        return std::unexpected(error{errc::closed, "snapshot requires close or cancel"});
    }
    engine_snapshot result;
    result.series.resize(impl_->configuration.series_count);
    result.rejected = impl_->invalid_rejected.load(std::memory_order_relaxed);
    for (const auto& target : impl_->shards) {
        const auto counters = target->queue.statistics();
        result.accepted += counters.accepted;
        result.rejected += counters.rejected;
        result.discarded += counters.discarded;
        result.queue_high_watermark += counters.high_watermark;
        result.processed += target->processed;
        result.latency.merge(target->latency);
        for (const auto& summary : target->summaries) {
            result.series[summary.series] = summary;
        }
    }
    return result;
}
} // namespace strata

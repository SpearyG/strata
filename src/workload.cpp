#include <strata/workload/workload.hpp>

#include <algorithm>
#include <bit>
#include <exception>
#include <iomanip>
#include <optional>
#include <ostream>
#include <thread>
#include <vector>

namespace strata::workload {
namespace {
[[nodiscard]] std::uint64_t mix(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31U);
}
struct alignas(64) producer_result {
    std::uint64_t generated{};
    std::uint64_t fingerprint{};
    std::optional<error> failure;
    std::exception_ptr exception;
};
} // namespace

double run_result::events_per_second() const noexcept {
    return elapsed.count() > 0 ? static_cast<double>(metrics.processed) * 1.0e9 /
        static_cast<double>(elapsed.count()) : 0.0;
}

event generate(std::uint64_t sequence, const options& settings) noexcept {
    auto random = mix(sequence ^ mix(settings.seed));
    const auto key = static_cast<std::uint32_t>(random % std::max(settings.runtime.series_count, std::uint32_t{1}));
    for (std::uint32_t i = 0; i < settings.work; ++i) random = mix(random);
    return {sequence, key, static_cast<double>(random % 1'000'001) / 100.0,
            sequence * 1000U};
}

std::uint64_t fingerprint(const event& sample) noexcept {
    return mix(sample.sequence ^ std::bit_cast<std::uint64_t>(sample.value) ^
               (static_cast<std::uint64_t>(sample.series) * 0x9e3779b97f4a7c15ULL) ^
               std::rotl(sample.timestamp_ns, 17));
}

std::expected<void, error> validate(const options& settings) {
    if (auto result = strata::validate(settings.runtime); !result) return result;
    if (settings.producers == 0 || settings.producers > 256 || settings.events == 0 ||
        settings.events > 1'000'000'000'000ULL || settings.work > 100'000 ||
        settings.pause < std::chrono::microseconds::zero() || settings.pause > std::chrono::seconds(1)) {
        return std::unexpected(error{errc::invalid_config, "invalid producers, events, work or pause limit"});
    }
    return {};
}

std::expected<run_result, error> run(const options& settings, std::stop_token stop) {
    if (auto result = validate(settings); !result) return std::unexpected(result.error());
    auto created = engine::create(settings.runtime);
    if (!created) return std::unexpected(created.error());
    auto& runtime = **created;
    std::vector<producer_result> results(settings.producers);
    std::vector<std::jthread> producers;
    producers.reserve(settings.producers);
    std::stop_source cancellation;
    std::stop_callback bridge(stop, [&] { cancellation.request_stop(); });
    const auto token = cancellation.get_token();
    // Includes producer startup, source work, backpressure, and final draining.
    // Engine construction and aggregate allocation are intentionally outside.
    const auto started = std::chrono::steady_clock::now();
    try {
        for (std::size_t p = 0; p < settings.producers; ++p) {
            producers.emplace_back([&, p] {
                auto& result = results[p];
                try {
                    const auto stride = static_cast<std::uint64_t>(settings.producers);
                    std::uint64_t burst_count = 0;
                    for (auto i = static_cast<std::uint64_t>(p); i < settings.events && !token.stop_requested(); i += stride) {
                        const auto sample = generate(i, settings);
                        const auto submitted = runtime.submit(sample, token);
                        if (!submitted) {
                            if (submitted.error().code != errc::cancelled) result.failure = submitted.error();
                            cancellation.request_stop();
                            break;
                        }
                        ++result.generated;
                        result.fingerprint += fingerprint(sample);
                        if (settings.burst > 0 && ++burst_count == settings.burst) {
                            burst_count = 0;
                            std::this_thread::sleep_for(settings.pause);
                        }
                    }
                } catch (...) {
                    result.exception = std::current_exception();
                    cancellation.request_stop();
                }
            });
        }
    } catch (...) {
        cancellation.request_stop();
        runtime.cancel();
        for (auto& producer : producers) producer.join();
        throw;
    }
    for (auto& producer : producers) producer.join();
    if (token.stop_requested()) runtime.cancel();
    else runtime.close();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    auto metrics = runtime.snapshot();
    if (!metrics) return std::unexpected(metrics.error());
    run_result result{std::move(*metrics), std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed),
                      0, 0, stop.stop_requested()};
    for (const auto& producer : results) {
        if (producer.exception) std::rethrow_exception(producer.exception);
        if (producer.failure) return std::unexpected(*producer.failure);
        result.generated += producer.generated;
        result.fingerprint += producer.fingerprint;
    }
    if (result.generated != result.metrics.accepted ||
        result.metrics.accepted != result.metrics.processed + result.metrics.discarded ||
        (!result.cancelled && result.metrics.processed != settings.events)) {
        return std::unexpected(error{errc::corrupt, "runtime count invariant failed"});
    }
    return result;
}

void write_json(std::ostream& out, const options& settings, const run_result& result) {
    const auto& m = result.metrics;
    // Consume every aggregate field outside the timed interval, including with
    // LTO. Source equivalence and aggregate observation are different checks.
    std::uint64_t aggregate_digest{};
    for (const auto& s : m.series) {
        aggregate_digest += mix(s.count ^ s.series ^ std::bit_cast<std::uint64_t>(s.mean) ^
            std::rotl(std::bit_cast<std::uint64_t>(s.m2), 11) ^
            std::rotl(std::bit_cast<std::uint64_t>(s.minimum), 23) ^
            std::rotl(std::bit_cast<std::uint64_t>(s.maximum), 37));
    }
    out << std::setprecision(10)
        << "{\"workers\":" << settings.runtime.workers
        << ",\"producers\":" << settings.producers
        << ",\"batch_size\":" << settings.runtime.batch_size
        << ",\"queue_capacity\":" << settings.runtime.queue_capacity
        << ",\"series\":" << settings.runtime.series_count
        << ",\"seed\":" << settings.seed << ",\"work\":" << settings.work
        << ",\"burst\":" << settings.burst << ",\"pause_us\":" << settings.pause.count()
        << ",\"latency_enabled\":" << (settings.runtime.measure_latency ? "true" : "false")
        << ",\"requested\":" << settings.events << ",\"accepted\":" << m.accepted
        << ",\"processed\":" << m.processed << ",\"rejected\":" << m.rejected
        << ",\"discarded\":" << m.discarded << ",\"fingerprint\":" << result.fingerprint
        << ",\"aggregate_digest\":" << aggregate_digest
        << ",\"cancelled\":" << (result.cancelled ? "true" : "false")
        << ",\"elapsed_ns\":" << result.elapsed.count()
        << ",\"throughput_events_s\":" << result.events_per_second()
        << ",\"queue_high_watermark\":" << m.queue_high_watermark
        << ",\"latency_samples\":" << m.latency.count()
        << ",\"p50_upper_ns\":" << m.latency.percentile(0.50)
        << ",\"p95_upper_ns\":" << m.latency.percentile(0.95)
        << ",\"p99_upper_ns\":" << m.latency.percentile(0.99)
        << ",\"max_latency_ns\":" << m.latency.max_ns() << '}';
}
} // namespace strata::workload

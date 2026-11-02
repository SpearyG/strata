#include <strata/runtime/engine.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stop_token>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
std::uint64_t mix(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31U);
}
struct settings {
    strata::config runtime;
    std::uint64_t events{5'000'000}, seed{42};
    std::size_t producers{4};
    bool hot_key{};
};
strata::event generate(std::uint64_t sequence, const settings& c) noexcept {
    const auto random = mix(sequence ^ mix(c.seed));
    return {sequence, c.hot_key ? 0U : static_cast<std::uint32_t>(random % c.runtime.series_count),
            static_cast<double>(random % 1'000'001) / 100.0, sequence * 1000U};
}
std::uint64_t fingerprint(const strata::event& e) noexcept {
    return mix(e.sequence ^ std::rotl(std::uint64_t{e.series}, 17) ^
               std::rotl(e.timestamp_ns, 31) ^ std::bit_cast<std::uint64_t>(e.value));
}
struct reference {
    std::uint64_t count{};
    long double mean{}, m2{};
    double minimum{std::numeric_limits<double>::infinity()};
    double maximum{-std::numeric_limits<double>::infinity()};
    void observe(double value) {
        ++count;
        const auto delta = static_cast<long double>(value) - mean;
        mean += delta / static_cast<long double>(count);
        m2 += delta * (static_cast<long double>(value) - mean);
        minimum = std::min(minimum, value); maximum = std::max(maximum, value);
    }
};
struct alignas(64) producer_result {
    std::uint64_t fingerprint{};
    std::exception_ptr failure;
};

int execute(int argc, char** argv) {
    settings c;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key(argv[i]);
        if (key == "--hot-key") { c.hot_key = true; continue; }
        if (key == "--no-latency") { c.runtime.measure_latency = false; continue; }
        if (++i == argc) throw std::runtime_error("missing benchmark value");
        const std::string_view text(argv[i]);
        std::uint64_t number{};
        const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (code != std::errc{} || end != text.data() + text.size()) throw std::runtime_error("invalid benchmark value");
        if (key == "--events") c.events = number;
        else if (key == "--seed") c.seed = number;
        else {
            if (number > (1U << 24U)) throw std::runtime_error("benchmark dimension exceeds limit");
            if (key == "--workers") c.runtime.workers = static_cast<std::size_t>(number);
            else if (key == "--producers") c.producers = static_cast<std::size_t>(number);
            else if (key == "--capacity") c.runtime.queue_capacity = static_cast<std::size_t>(number);
            else if (key == "--batch") c.runtime.batch_size = static_cast<std::size_t>(number);
            else if (key == "--series") c.runtime.series_count = static_cast<std::uint32_t>(number);
            else throw std::runtime_error("unknown benchmark option");
        }
    }
    if (!strata::validate(c.runtime) || c.events == 0 || c.events > 1'000'000'000 ||
        c.producers == 0 || c.producers > 256) throw std::runtime_error("invalid benchmark topology");

    // This same harness translation unit is compiled against both engine refs.
    // Reference work and snapshot validation are outside the measured interval.
    std::vector<reference> expected(c.runtime.series_count);
    std::uint64_t expected_fingerprint{};
    for (std::uint64_t i = 0; i < c.events; ++i) {
        const auto e = generate(i, c); expected[e.series].observe(e.value);
        expected_fingerprint += fingerprint(e);
    }
    auto created = strata::engine::create(c.runtime);
    if (!created) throw std::runtime_error(created.error().message);
    auto& runtime = **created;
    std::vector<producer_result> results(c.producers);
    std::stop_source stop;
    const auto token = stop.get_token();
    std::vector<std::jthread> producers;
    producers.reserve(c.producers);
    const auto started = std::chrono::steady_clock::now();
    try {
        for (std::size_t p = 0; p < c.producers; ++p) producers.emplace_back([&, p] {
            try {
                for (auto i = static_cast<std::uint64_t>(p); i < c.events && !token.stop_requested(); i += c.producers) {
                    const auto e = generate(i, c);
                    if (const auto sent = runtime.submit(e, token); !sent)
                        throw std::runtime_error(sent.error().message);
                    results[p].fingerprint += fingerprint(e);
                }
            } catch (...) { results[p].failure = std::current_exception(); stop.request_stop(); }
        });
    } catch (...) {
        stop.request_stop(); runtime.cancel();
        for (auto& p : producers) p.join();
        throw;
    }
    for (auto& p : producers) p.join();
    if (stop.stop_requested()) runtime.cancel(); else runtime.close();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count();
    std::uint64_t source_fingerprint{};
    for (const auto& p : results) {
        if (p.failure) std::rethrow_exception(p.failure);
        source_fingerprint += p.fingerprint;
    }
    const auto snapshot = runtime.snapshot();
    if (!snapshot || snapshot->processed != c.events || snapshot->accepted != c.events ||
        snapshot->rejected != 0 || snapshot->discarded != 0 || source_fingerprint != expected_fingerprint ||
        snapshot->latency.count() != (c.runtime.measure_latency ? c.events : 0) ||
        snapshot->series.size() != expected.size())
        throw std::runtime_error("benchmark count/source invariant failed");
    std::uint64_t aggregate_digest{};
    std::vector<bool> seen(expected.size());
    for (const auto& actual : snapshot->series) {
        if (actual.series >= expected.size() || seen[actual.series])
            throw std::runtime_error("benchmark series identity invariant failed");
        seen[actual.series] = true;
        const auto& r = expected[actual.series];
        if (actual.count != r.count || (r.count != 0 &&
            (!std::isfinite(actual.mean) || !std::isfinite(actual.m2) ||
             !std::isfinite(actual.minimum) || !std::isfinite(actual.maximum) ||
             std::abs(static_cast<long double>(actual.mean) - r.mean) > 1e-8L ||
             std::abs(static_cast<long double>(actual.m2) - r.m2) > std::max(1e-6L, std::abs(r.m2) * 1e-10L) ||
             actual.minimum != r.minimum || actual.maximum != r.maximum)))
            throw std::runtime_error("benchmark aggregate reference mismatch");
        aggregate_digest += mix(actual.count ^ actual.series ^ std::bit_cast<std::uint64_t>(actual.mean) ^
            std::rotl(std::bit_cast<std::uint64_t>(actual.m2), 11) ^
            std::rotl(std::bit_cast<std::uint64_t>(actual.minimum), 23) ^
            std::rotl(std::bit_cast<std::uint64_t>(actual.maximum), 37));
    }
    std::cout << std::setprecision(12) << "{\"events\":" << c.events
        << ",\"workers\":" << c.runtime.workers << ",\"producers\":" << c.producers
        << ",\"capacity\":" << c.runtime.queue_capacity << ",\"batch\":" << c.runtime.batch_size
        << ",\"series\":" << c.runtime.series_count << ",\"seed\":" << c.seed
        << ",\"hot_key\":" << (c.hot_key ? "true" : "false")
        << ",\"latency_enabled\":" << (c.runtime.measure_latency ? "true" : "false")
        << ",\"accepted\":" << snapshot->accepted << ",\"processed\":" << snapshot->processed
        << ",\"rejected\":" << snapshot->rejected << ",\"discarded\":" << snapshot->discarded
        << ",\"latency_samples\":" << snapshot->latency.count() << ",\"fingerprint\":" << source_fingerprint
        << ",\"aggregate_digest\":" << aggregate_digest << ",\"validated\":true"
        << ",\"elapsed_ns\":" << elapsed
        << ",\"events_per_second\":" << static_cast<double>(c.events) * 1e9 / static_cast<double>(elapsed)
        << ",\"p50_upper_ns\":" << snapshot->latency.percentile(.5)
        << ",\"p95_upper_ns\":" << snapshot->latency.percentile(.95)
        << ",\"p99_upper_ns\":" << snapshot->latency.percentile(.99)
        << ",\"max_latency_ns\":" << snapshot->latency.max_ns() << "}\n";
    std::cout.flush();
    return std::cout ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    try { return execute(argc, argv); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

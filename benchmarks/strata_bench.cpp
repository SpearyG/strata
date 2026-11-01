#include <strata/workload/options.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace {
#if defined(__clang__)
constexpr std::string_view compiler_version = "Clang " __clang_version__;
#elif defined(__GNUC__)
constexpr std::string_view compiler_version = "GCC " __VERSION__;
#elif defined(_MSC_VER)
constexpr std::string_view compiler_version = "MSVC";
#else
constexpr std::string_view compiler_version = "unknown";
#endif
struct summary {
    double median{}, minimum{}, maximum{};
};
summary summarize(std::vector<double> samples) {
    std::ranges::sort(samples);
    const auto size = samples.size();
    const auto median = size % 2 == 0 ? (samples[size / 2 - 1] + samples[size / 2]) / 2 : samples[size / 2];
    return {median, samples.front(), samples.back()};
}

int execute(int argc, char** argv) {
    strata::workload::options settings;
    std::size_t repeats = 7, warmups = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string_view name(argv[i]);
        if (name == "--help" || name == "-h") {
            std::cout << "Usage: strata_bench [options]\n" << strata::workload::shared_help
                      << "  --repeats N          measured paired runs (3..100; default 7)\n"
                      << "  --warmups N          unreported warmup pairs (1..10; default 1)\n"
                      << "JSON Lines: environment, every sample, then baseline/tuned summaries.\n"
                      << "Pairs alternate order. Latency percentiles are power-of-two bucket upper bounds.\n";
            return 0;
        }
        if (name == "--no-latency") { settings.runtime.measure_latency = false; continue; }
        if (i + 1 == argc) { std::cerr << "missing value for " << name << '\n'; return 2; }
        const std::string_view value(argv[++i]);
        if (name == "--repeats" || name == "--warmups") {
            const auto count = strata::workload::parse_unsigned<std::size_t>(value);
            if (!count) { std::cerr << count.error().message << '\n'; return 2; }
            if (name == "--repeats") repeats = *count;
            else warmups = *count;
        } else if (const auto result = strata::workload::set_option(settings, name, value); !result) {
            std::cerr << result.error().message << '\n'; return 2;
        }
    }
    if (repeats < 3 || repeats > 100 || warmups < 1 || warmups > 10) {
        std::cerr << "repeats must be 3..100 and warmups 1..10\n"; return 2;
    }
    if (auto result = strata::workload::validate(settings); !result) {
        std::cerr << result.error().message << '\n'; return 2;
    }
    auto baseline = settings;
    baseline.runtime.batch_size = 1;
    std::cout << "{\"type\":\"environment\",\"compiler\":\"" << compiler_version
              << "\",\"cplusplus\":" << __cplusplus
              << ",\"hardware_threads\":" << std::thread::hardware_concurrency()
              << ",\"repeats\":" << repeats << ",\"warmup_pairs\":" << warmups
              << ",\"timing\":\"producer-start-through-drain; engine allocation excluded\"}\n";
    std::vector<double> rates[2];
    for (auto& group : rates) group.reserve(repeats);
    std::optional<std::uint64_t> expected_fingerprint;
    for (std::size_t iteration = 0; iteration < warmups + repeats; ++iteration) {
        for (std::size_t step = 0; step < 2; ++step) {
            const auto group = (iteration + step) % 2;
            const auto& selected = group == 0 ? baseline : settings;
            const auto result = strata::workload::run(selected);
            if (!result) { std::cerr << result.error().message << '\n'; return 1; }
            if (!expected_fingerprint) expected_fingerprint = result->fingerprint;
            if (result->fingerprint != *expected_fingerprint) {
                std::cerr << "generator fingerprint differs across runs\n"; return 1;
            }
            if (iteration < warmups) continue;
            rates[group].push_back(result->events_per_second());
            std::cout << "{\"type\":\"sample\",\"variant\":\"" << (group == 0 ? "baseline" : "batched")
                      << "\",\"iteration\":" << iteration - warmups << ",\"run\":";
            strata::workload::write_json(std::cout, selected, *result);
            std::cout << "}\n";
        }
    }
    const auto single = summarize(rates[0]);
    const auto batched = summarize(rates[1]);
    std::cout << std::setprecision(10)
              << "{\"type\":\"summary\",\"baseline_batch\":1,\"tuned_batch\":" << settings.runtime.batch_size
              << ",\"baseline_median_events_s\":" << single.median
              << ",\"baseline_min_events_s\":" << single.minimum
              << ",\"baseline_max_events_s\":" << single.maximum
              << ",\"batched_median_events_s\":" << batched.median
              << ",\"batched_min_events_s\":" << batched.minimum
              << ",\"batched_max_events_s\":" << batched.maximum
              << ",\"median_speedup\":" << batched.median / single.median << "}\n";
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto result = execute(argc, argv);
        std::cout.flush();
        if (!std::cout) { std::cerr << "failed writing stdout\n"; return 1; }
        return result;
    }
    catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}

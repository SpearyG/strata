#include <strata/storage/journal.hpp>
#include <strata/workload/options.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

strata::workload::options small_options() {
    strata::workload::options settings;
    settings.runtime.workers = 2;
    settings.runtime.series_count = 17;
    settings.runtime.queue_capacity = 3;
    settings.runtime.batch_size = 3;
    settings.producers = 3;
    settings.events = 10'003;
    return settings;
}

void parser_test() {
    using strata::workload::parse_unsigned;
    const auto maximum = parse_unsigned<std::uint64_t>("18446744073709551615");
    check(maximum && *maximum == std::numeric_limits<std::uint64_t>::max(), "uint64 maximum parse");
    for (const auto text : {"", "-1", "+1", " 1", "1 ", "2x", "18446744073709551616"})
        check(!parse_unsigned<std::uint64_t>(text), "invalid integer accepted");
    check(!parse_unsigned<std::uint32_t>("4294967296"), "uint32 overflow accepted");
    auto settings = small_options();
    check(!strata::workload::set_option(settings, "--not-an-option", "1"), "unknown option accepted");
    check(!strata::workload::set_option(settings, "--series", "4294967296"), "series narrowing overflow");
    check(!strata::workload::set_option(settings, "--work", "100001"), "unbounded work accepted");
    check(!strata::workload::set_option(settings, "--pause-us", "1000001"), "unbounded pause accepted");
    check(strata::workload::set_option(settings, "--seed", "18446744073709551615").has_value(), "full seed range");
    check(settings.seed == std::numeric_limits<std::uint64_t>::max(), "seed lost bits");
    settings.producers = 0;
    check(!strata::workload::validate(settings), "zero producer validation");
    settings = small_options();
    settings.events = 0;
    check(!strata::workload::run(settings), "zero event validation");
    settings.events = 1;
    settings.runtime.batch_size = 4;
    check(!strata::workload::validate(settings), "batch larger than queue accepted");
}

void deterministic_generator_test() {
    auto settings = small_options();
    bool seed_changes_output = false, work_changes_output = false;
    auto different_seed = settings;
    different_seed.seed = 99;
    auto different_work = settings;
    different_work.work = 17;
    for (std::uint64_t i = 0; i < 100; ++i) {
        const auto sample = strata::workload::generate(i, settings);
        check(sample == strata::workload::generate(i, settings), "generator determinism");
        check(sample.sequence == i && sample.timestamp_ns == i * 1000U, "source identity");
        check(strata::valid(sample, settings.runtime.series_count), "generator valid range");
        seed_changes_output |= sample != strata::workload::generate(i, different_seed);
        work_changes_output |= sample.value != strata::workload::generate(i, different_work).value;
    }
    check(seed_changes_output && work_changes_output, "seed/work must affect observable payload");
}

void fingerprint_test() {
    const strata::event sample{41, 7, 23.5, 987'654};
    const auto expected = strata::workload::fingerprint(sample);
    auto changed = sample;
    ++changed.sequence;
    check(strata::workload::fingerprint(changed) != expected, "fingerprint includes sequence");
    changed = sample;
    ++changed.series;
    check(strata::workload::fingerprint(changed) != expected, "fingerprint includes series");
    changed = sample;
    changed.value += 1.0;
    check(strata::workload::fingerprint(changed) != expected, "fingerprint includes value bits");
    changed = sample;
    ++changed.timestamp_ns;
    check(strata::workload::fingerprint(changed) != expected, "fingerprint includes source timestamp");

    auto one_key = small_options();
    one_key.runtime.series_count = 1;
    auto many_keys = one_key;
    many_keys.runtime.series_count = 1024;
    std::uint64_t one_fingerprint = 0, many_fingerprint = 0;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        one_fingerprint += strata::workload::fingerprint(strata::workload::generate(i, one_key));
        many_fingerprint += strata::workload::fingerprint(strata::workload::generate(i, many_keys));
    }
    check(one_fingerprint != many_fingerprint, "routing workload affects source fingerprint");
}

void compare_series(const strata::engine_snapshot& actual, const strata::workload::options& settings) {
    std::vector<std::uint64_t> counts(settings.runtime.series_count);
    std::vector<long double> sums(settings.runtime.series_count);
    for (std::uint64_t i = 0; i < settings.events; ++i) {
        const auto sample = strata::workload::generate(i, settings);
        ++counts[sample.series];
        sums[sample.series] += sample.value;
    }
    check(actual.series.size() == settings.runtime.series_count, "all series returned");
    for (const auto& summary : actual.series) {
        check(summary.series < settings.runtime.series_count, "series id in range");
        check(summary.count == counts[summary.series], "partition aggregate count matches reference");
        if (summary.count != 0) {
            const auto expected = static_cast<double>(sums[summary.series] / summary.count);
            check(std::abs(summary.mean - expected) < 1.0e-8, "aggregate mean matches reference");
        }
    }
}

void run_accounting_test() {
    const auto settings = small_options();
    const auto result = strata::workload::run(settings);
    check(result.has_value(), "concurrent workload succeeds");
    check(result->metrics.accepted == settings.events && result->metrics.processed == settings.events,
          "remainder events are not lost between producers");
    check(result->metrics.discarded == 0 && result->metrics.rejected == 0, "drained workload has no losses");
    check(result->metrics.latency.count() == settings.events, "latency covers every processed event");
    check(result->elapsed.count() > 0 && result->events_per_second() > 0.0, "throughput timing");
    std::uint64_t expected_fingerprint = 0;
    for (std::uint64_t i = 0; i < settings.events; ++i)
        expected_fingerprint += strata::workload::fingerprint(strata::workload::generate(i, settings));
    check(result->fingerprint == expected_fingerprint, "run fingerprints complete accepted payloads");
    compare_series(result->metrics, settings);
    auto single_producer = settings;
    single_producer.producers = 1;
    single_producer.runtime.workers = 1;
    const auto serial = strata::workload::run(single_producer);
    check(serial && serial->fingerprint == result->fingerprint, "source identity independent of thread counts");
}

void disabled_latency_and_work_test() {
    auto settings = small_options();
    settings.runtime.measure_latency = false;
    settings.work = 23;
    settings.events = 1001;
    const auto result = strata::workload::run(settings);
    check(result.has_value(), "CPU mixing workload succeeds");
    check(result->metrics.latency.count() == 0 && result->metrics.latency.max_ns() == 0,
          "disabled instrumentation does not record samples");
    compare_series(result->metrics, settings);
}

void cancellation_test() {
    const auto settings = small_options();
    std::stop_source source;
    source.request_stop();
    const auto cancelled = strata::workload::run(settings, source.get_token());
    check(cancelled && cancelled->cancelled, "pre-cancelled workload returns valid partial result");
    check(cancelled->generated == 0 && cancelled->metrics.accepted == 0 &&
          cancelled->metrics.processed == 0 && cancelled->metrics.discarded == 0,
          "pre-cancellation admits no events");
}

void replay_integration_test() {
    struct temporary_file {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("strata-workload-" + std::to_string(strata::steady_now_ns()) + ".journal");
        ~temporary_file() { std::error_code ec; std::filesystem::remove(path, ec); }
    } file;
    auto settings = small_options();
    settings.events = 1009;
    std::vector<strata::event> events;
    events.reserve(static_cast<std::size_t>(settings.events));
    for (std::uint64_t i = 0; i < settings.events; ++i) events.push_back(strata::workload::generate(i, settings));
    {
        auto writer = strata::storage::journal_writer::open(file.path);
        check(writer.has_value(), "journal open");
        check((*writer)->append(std::span<const strata::event>(events).first(500)).has_value(), "first frame append");
        check((*writer)->append(std::span<const strata::event>(events).subspan(500)).has_value(), "second frame append");
        check((*writer)->flush().has_value(), "journal flush");
    }
    auto runtime = strata::engine::create(settings.runtime);
    check(runtime.has_value(), "replay runtime creation");
    const auto replay = strata::storage::replay(file.path,
        [&](std::span<const strata::event> batch) -> std::expected<void, strata::error> {
            for (const auto& event : batch)
                if (auto accepted = (*runtime)->submit(event); !accepted) return accepted;
            return {};
        });
    check(replay && replay->frames == 2 && replay->events == settings.events && !replay->truncated_tail,
          "full journal replay");
    (*runtime)->close();
    const auto snapshot = (*runtime)->snapshot();
    check(snapshot && snapshot->processed == settings.events, "journal events all drained");
    compare_series(*snapshot, settings);
}
} // namespace

int main() {
    try {
        parser_test();
        deterministic_generator_test();
        fingerprint_test();
        run_accounting_test();
        disabled_latency_and_work_test();
        cancellation_test();
        replay_integration_test();
        std::cout << "7 workload test groups passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "workload test failed: " << exception.what() << '\n';
        return 1;
    }
}

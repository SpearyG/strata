#include <strata/storage/journal.hpp>
#include <strata/workload/options.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {
std::atomic<bool> interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free);
extern "C" void on_interrupt(int) { interrupted.store(true, std::memory_order_relaxed); }

void write_json_string(std::ostream& out, std::string_view message) {
    out << '"';
    for (const auto c : message) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (static_cast<unsigned char>(c) < 0x20U) out << ' ';
        else out << c;
    }
    out << '"';
}

void print_error(std::string_view message) {
    std::cerr << "{\"error\":";
    write_json_string(std::cerr, message);
    std::cerr << "}\n";
}

std::expected<void, strata::error> record(const std::filesystem::path& path,
                                         const strata::workload::options& settings,
                                         std::stop_token stop) {
    auto writer = strata::storage::journal_writer::open(path);
    if (!writer) return std::unexpected(writer.error());
    std::array<strata::event, 1024> batch{};
    for (std::uint64_t first = 0; first < settings.events;) {
        if (stop.stop_requested()) return std::unexpected(strata::error{strata::errc::cancelled, "recording cancelled"});
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(batch.size(), settings.events - first));
        for (std::size_t i = 0; i < count; ++i) batch[i] = strata::workload::generate(first + i, settings);
        if (auto result = (*writer)->append(std::span<const strata::event>(batch.data(), count)); !result) return result;
        first += count;
    }
    return (*writer)->flush();
}

int execute(int argc, char** argv) {
    strata::workload::options settings;
    std::filesystem::path journal, replay_path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view name(argv[i]);
        if (name == "--help" || name == "-h") {
            std::cout << "Strata: bounded, key-partitioned telemetry aggregation\nUsage: strata [options]\n"
                      << strata::workload::shared_help
                      << "  --journal PATH       truncate/write generated source before measured run\n"
                      << "  --replay PATH        replay journal sequentially into concurrent workers\n"
                      << "SIGINT cancels admission and discards queued events; JSON reports exact counts.\n";
            return 0;
        }
        if (name == "--no-latency") { settings.runtime.measure_latency = false; continue; }
        if (i + 1 == argc) { print_error("missing value for " + std::string(name)); return 2; }
        const std::string_view value(argv[++i]);
        if (name == "--journal" || name == "--replay") {
            if (value.empty()) { print_error("journal path cannot be empty"); return 2; }
            if (name == "--journal") journal = value;
            else replay_path = value;
        }
        else if (const auto result = strata::workload::set_option(settings, name, value); !result) {
            print_error(result.error().message); return 2;
        }
    }
    if (!journal.empty() && !replay_path.empty()) {
        print_error("--journal and --replay are mutually exclusive"); return 2;
    }
    if (auto valid = strata::workload::validate(settings); !valid) { print_error(valid.error().message); return 2; }
    std::signal(SIGINT, on_interrupt);
    std::stop_source cancellation;
    std::jthread monitor([&](std::stop_token token) {
        while (!token.stop_requested()) {
            if (interrupted.load(std::memory_order_relaxed)) { cancellation.request_stop(); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    const auto stop = cancellation.get_token();
    if (!journal.empty()) {
        if (auto result = record(journal, settings, stop); !result) {
            print_error(result.error().message); return result.error().code == strata::errc::cancelled ? 130 : 1;
        }
    }
    if (!replay_path.empty()) {
        auto created = strata::engine::create(settings.runtime);
        if (!created) { print_error(created.error().message); return 1; }
        auto& engine = **created;
        std::uint64_t accepted = 0, fingerprint = 0, frames = 0;
        const auto started = std::chrono::steady_clock::now();
        const auto replayed = strata::storage::replay(replay_path,
            [&](std::span<const strata::event> events) -> std::expected<void, strata::error> {
                for (const auto& event : events) {
                    if (const auto result = engine.submit(event, stop); !result) return result;
                    ++accepted;
                    fingerprint += strata::workload::fingerprint(event);
                }
                ++frames;
                return {};
            });
        // The signal handler can run before the monitor's next polling interval,
        // including after replay's final callback. Preserve that cancellation.
        if (interrupted.load(std::memory_order_relaxed)) cancellation.request_stop();
        const auto replay_cancelled = !replayed && replayed.error().code == strata::errc::cancelled;
        if (!replayed && !replay_cancelled) {
            engine.cancel(); print_error(replayed.error().message);
            return 1;
        }
        if (stop.stop_requested() || replay_cancelled) engine.cancel();
        else engine.close();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started);
        monitor.request_stop();
        monitor.join();
        const auto cancelled = replay_cancelled || stop.stop_requested() ||
                               interrupted.load(std::memory_order_relaxed);
        auto metrics = engine.snapshot();
        if (!metrics) { print_error(metrics.error().message); return 1; }
        if (metrics->accepted != accepted || metrics->accepted != metrics->processed + metrics->discarded) {
            print_error("replay runtime count invariant failed"); return 1;
        }
        settings.producers = 1;
        settings.events = accepted;
        strata::workload::run_result result{std::move(*metrics), elapsed, accepted, fingerprint, cancelled};
        std::cout << "{\"replay_frames\":" << frames
                  << ",\"replay_complete\":" << (replayed ? "true" : "false")
                  << ",\"truncated_tail\":";
        if (replayed) std::cout << (replayed->truncated_tail ? "true" : "false");
        else std::cout << "null";
        if (cancelled) {
            std::cout << ",\"error\":";
            write_json_string(std::cout, replayed ? "replay cancelled" : replayed.error().message);
        }
        std::cout << ",\"run\":";
        strata::workload::write_json(std::cout, settings, result);
        std::cout << "}\n";
        return cancelled ? 130 : 0;
    }
    auto result = strata::workload::run(settings, stop);
    if (!result) { print_error(result.error().message); return 1; }
    strata::workload::write_json(std::cout, settings, *result);
    std::cout << '\n';
    return result->cancelled ? 130 : 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto result = execute(argc, argv);
        std::cout.flush();
        if (!std::cout) { print_error("failed writing stdout"); return 1; }
        return result;
    }
    catch (const std::exception& exception) { print_error(exception.what()); return 1; }
}

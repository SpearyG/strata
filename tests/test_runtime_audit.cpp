#include "check.hpp"
#include <strata/runtime/engine.hpp>

#include <array>
#include <atomic>
#include <latch>
#include <random>
#include <stop_token>
#include <thread>
#include <vector>

int main() {
    return test_main([] {
        // One producer, many shards, a 90% hot key and capacity-one rings.
        auto skewed = strata::engine::create({16, 1, 1, 64, true});
        check(skewed.has_value());
        std::array<std::uint64_t, 64> reference{};
        for (std::uint64_t i = 0; i < 10000; ++i) {
            const auto key = i % 10U == 0 ? static_cast<std::uint32_t>((i / 10U) % 64U) : 0U;
            check((*skewed)->submit({i, key, static_cast<double>(key), i}).has_value());
            ++reference[key];
        }
        (*skewed)->close();
        const auto snapshot = (*skewed)->snapshot(); check(snapshot.has_value());
        check(snapshot->accepted == 10000 && snapshot->processed == 10000);
        for (std::size_t key = 0; key < reference.size(); ++key) {
            check(snapshot->series[key].count == reference[key]);
            if (reference[key] != 0) check(snapshot->series[key].mean == static_cast<double>(key) && snapshot->series[key].m2 == 0);
        }

        std::mt19937 random(0xa0d17U);
        for (unsigned iteration = 0; iteration < 36; ++iteration) {
            const auto workers = std::size_t{1} + random() % 8U;
            const auto capacity = std::size_t{1} + random() % 8U;
            const auto producers = std::size_t{1} + random() % 6U;
            auto created = strata::engine::create({workers, capacity, 1, 8, false});
            check(created.has_value()); auto& runtime = **created;
            std::vector<std::uint64_t> accepted(producers);
            std::latch entered(static_cast<std::ptrdiff_t>(producers));
            std::atomic<bool> invalid_error{};
            std::stop_source stop;
            std::vector<std::jthread> threads;
            for (std::size_t p = 0; p < producers; ++p) threads.emplace_back([&, p] {
                for (std::uint64_t i = 0; i < 3000; ++i) {
                    const auto result = runtime.submit({i, static_cast<std::uint32_t>(p % 8U), 1, 0}, stop.get_token());
                    if (result) ++accepted[p];
                    else if (result.error().code != strata::errc::cancelled && result.error().code != strata::errc::closed)
                        invalid_error.store(true);
                    if (i == 0) entered.count_down();
                    if (!result) break;
                }
            });
            entered.wait(); // Every producer has completed one real submission.
            std::jthread drain([&] { runtime.close(); });
            std::jthread other([&] { if (iteration % 2U != 0) runtime.cancel(); else runtime.close(); });
            std::jthread cancel_source([&] { stop.request_stop(); });
            drain.join(); other.join(); cancel_source.join(); threads.clear();
            const auto final = runtime.snapshot(); check(final.has_value() && !invalid_error.load());
            std::uint64_t expected{}; for (const auto count : accepted) expected += count;
            check(final->accepted == expected && expected >= producers);
            check(final->accepted == final->processed + final->discarded);
            if (iteration % 2U == 0) check(final->discarded == 0);
            check(final->latency.count() == 0);
        }
    });
}

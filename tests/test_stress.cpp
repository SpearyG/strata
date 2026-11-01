#include "check.hpp"
#include <strata/runtime/engine.hpp>
#include <array>
#include <atomic>
#include <random>
#include <thread>
#include <vector>

int main() {
    return test_main([] {
        std::mt19937 rng(0x53747261U);
        for (int iteration = 0; iteration < 32; ++iteration) {
            strata::config c;
            c.workers = 1U + rng() % 8U; c.queue_capacity = 1U + rng() % 97U;
            c.batch_size = 1U + rng() % c.queue_capacity; c.series_count = 32; c.measure_latency = true;
            auto r = strata::engine::create(c); check(r.has_value()); auto e = std::move(*r);
            const std::uint32_t producers = 1U + rng() % 8U;
            std::array<std::uint64_t, 32> expected{};
            constexpr std::uint64_t per_producer = 2000;
            for (std::uint32_t p = 0; p < producers; ++p)
                for (std::uint64_t i = 0; i < per_producer; ++i) ++expected[(i + p) % 32U];
            std::atomic<bool> failed{};
            std::vector<std::jthread> threads;
            for (std::uint32_t p = 0; p < producers; ++p) threads.emplace_back([&, p] {
                for (std::uint64_t i = 0; i < per_producer; ++i) {
                    const auto key = static_cast<std::uint32_t>((i + p) % 32U);
                    auto item = strata::event{i, key, static_cast<double>(key), i};
                    auto accepted = e->try_submit(item);
                    if (!accepted && accepted.error().code == strata::errc::full) accepted = e->submit(item);
                    if (!accepted) failed.store(true);
                    if (i % 211U == 0) std::this_thread::yield();
                }
            });
            threads.clear(); e->close(); auto s = e->snapshot();
            check(s.has_value() && !failed.load());
            check(s->processed == per_producer * producers && s->accepted == s->processed);
            check(s->latency.count() == s->processed);
            for (std::uint32_t key = 0; key < 32; ++key) {
                check(s->series[key].count == expected[key]);
                if (expected[key] != 0) check(s->series[key].mean == key && s->series[key].m2 == 0);
            }
        }
    });
}

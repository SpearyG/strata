#include "check.hpp"
#include <strata/concurrency/bounded_channel.hpp>
#include <strata/core/config.hpp>
#include <strata/metrics/latency_histogram.hpp>
#include <strata/pipeline/pipeline.hpp>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
struct double_value {
    bool operator()(strata::event& e) const noexcept { e.value *= 2; return true; }
};
struct forbidden_stage { void operator()(strata::event&) {} };
static_assert(!strata::event_stage<forbidden_stage>);
static_assert(strata::valid_topology<4, 4096, 64>());
static_assert(!strata::valid_topology<0, 16, 2>());
static_assert(!strata::valid_topology<2, 8, 9>());
using pipe_type = strata::pipeline<strata::validate_event, double_value>;
static_assert(std::same_as<pipe_type::stage_type<1>, double_value>);
}

int main() {
    return test_main([] {
        auto p = strata::pipeline(strata::validate_event{8}, double_value{});
        strata::event e{1, 3, 2.5, 99};
        check(p(e) && e.value == 5.0, "pipeline transform");
        e.series = 8;
        check(!p(e) && e.value == 5.0, "pipeline short circuit");
        e.series = 1; e.value = std::numeric_limits<double>::quiet_NaN();
        check(!p(e));
        e.value = std::numeric_limits<double>::infinity(); check(!p(e));
        e.value = 1e101; check(!p(e));
        check(p.stage<0>().series_count == 8);
        strata::config c;
        check(strata::validate(c).has_value());
        c.workers = 0; check(!strata::validate(c));
        c.workers = 256; c.queue_capacity = 1U << 24U; check(!strata::validate(c));

        strata::concurrency::bounded_channel<int> q(3);
        check(q.try_push(1).has_value());
        check(q.push(2).has_value());
        check(q.push(3).has_value());
        auto full = q.try_push(4); check(!full && full.error().code == strata::errc::full);
        std::array<int, 2> out{};
        check(q.pop_batch(out) == 2 && out[0] == 1 && out[1] == 2);
        check(q.push(4).has_value()); check(q.push(5).has_value());
        q.close(); q.close();
        check(!q.push(6));
        check(q.pop_batch(out) == 2 && out[0] == 3 && out[1] == 4);
        check(q.pop_batch(out) == 1 && out[0] == 5);
        check(q.pop_batch(out) == 0);
        check(q.statistics().accepted == 5 && q.statistics().high_watermark == 3);

        strata::concurrency::bounded_channel<int> cancel_q(1);
        check(cancel_q.push(1).has_value());
        std::stop_source stop;
        std::atomic<bool> cancelled{};
        std::jthread blocked([&] {
            auto r = cancel_q.push(2, stop.get_token());
            cancelled.store(!r && r.error().code == strata::errc::cancelled);
        });
        stop.request_stop(); blocked.join();
        check(cancelled.load(), "stop must wake a full channel");
        cancel_q.cancel(); check(cancel_q.pop_batch(out) == 0);
        check(cancel_q.statistics().discarded == 1);

        strata::concurrency::bounded_channel<int> closed_q(1);
        check(closed_q.push(1).has_value());
        std::atomic<bool> closed{};
        std::jthread close_waiter([&] { closed.store(!closed_q.push(2)); });
        closed_q.close(); close_waiter.join(); check(closed.load());

        bool threw = false;
        try { strata::concurrency::bounded_channel<int> invalid(0); }
        catch (const std::invalid_argument&) { threw = true; }
        check(threw);
        threw = false;
        try { (void)q.pop_batch({}); }
        catch (const std::invalid_argument&) { threw = true; }
        check(threw);
        strata::concurrency::bounded_channel<std::unique_ptr<int>> owned(2);
        check(owned.push(std::make_unique<int>(42)).has_value());
        std::array<std::unique_ptr<int>, 1> pointer{};
        check(owned.pop_batch(pointer) == 1 && *pointer[0] == 42);
        check(owned.push(std::make_unique<int>(99)).has_value());
        owned.cancel(); owned.cancel(); check(owned.statistics().discarded == 1);

        // Exercise the generic channel's MPMC contract, independently of the
        // engine's single-consumer-per-shard topology.
        constexpr std::size_t items = 12000;
        strata::concurrency::bounded_channel<std::size_t> shared(19);
        std::vector<std::atomic<unsigned>> seen(items);
        std::atomic<bool> failed{};
        std::vector<std::jthread> consumers;
        for (unsigned cidx = 0; cidx < 4; ++cidx) consumers.emplace_back([&] {
            std::array<std::size_t, 7> batch{};
            while (const auto count = shared.pop_batch(batch)) {
                for (std::size_t i = 0; i < count; ++i) {
                    if (batch[i] >= items) failed.store(true);
                    else seen[batch[i]].fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        std::vector<std::jthread> senders;
        for (std::size_t pidx = 0; pidx < 4; ++pidx) senders.emplace_back([&, pidx] {
            for (auto i = pidx; i < items; i += 4) if (!shared.push(i)) failed.store(true);
        });
        senders.clear(); shared.close(); consumers.clear();
        check(!failed.load());
        for (const auto& count : seen) check(count.load() == 1, "exactly once channel transfer");

        strata::metrics::latency_histogram h;
        check(h.count() == 0 && h.percentile(0.5) == 0);
        for (std::uint64_t i = 1; i <= 100; ++i) h.observe(i);
        check(h.count() == 100 && h.max_ns() == 100);
        check(h.percentile(0.5) >= 50 && h.percentile(0.5) <= 100);
        check(h.percentile(0.99) >= 99);
        strata::metrics::latency_histogram other; other.observe(1000); h.merge(other);
        check(h.count() == 101 && h.max_ns() == 1000);
        h.observe(std::numeric_limits<std::uint64_t>::max());
        check(h.percentile(1) == std::numeric_limits<std::uint64_t>::max());
        strata::metrics::latency_histogram zero; zero.observe(0);
        check(zero.count() == 1 && zero.percentile(0.5) == 0);
    });
}

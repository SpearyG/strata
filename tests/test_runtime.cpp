#include "check.hpp"
#include <strata/runtime/engine.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <latch>
#include <limits>
#include <thread>
#include <vector>

namespace {
auto make_engine(strata::config c) {
    auto r = strata::engine::create(c); check(r.has_value(), "engine create"); return std::move(*r);
}
void sequential() {
    auto e = make_engine({2, 17, 7, 4, true});
    check(!e->snapshot(), "snapshot forbidden before join");
    for (std::uint64_t i = 0; i < 1000; ++i) {
        check(e->submit({i, 1, static_cast<double>(i), i}).has_value());
    }
    auto bad = e->submit({1, 4, 3, 0}); check(!bad && bad.error().code == strata::errc::invalid_event);
    check(!e->submit({1, 0, std::numeric_limits<double>::infinity(), 0}));
    e->close(); e->close();
    auto s = e->snapshot(); check(s.has_value());
    check(s->accepted == 1000 && s->processed == 1000 && s->discarded == 0);
    check(s->rejected == 2 && s->latency.count() == 1000);
    const auto& key = s->series[1];
    check(key.count == 1000 && std::abs(key.mean - 499.5) < 1e-10);
    check(std::abs(key.m2 - 83333250.0) < 1e-4);
    check(key.minimum == 0 && key.maximum == 999);
    check(!e->submit({1, 0, 0, 0}));
}
void concurrent() {
    auto e = make_engine({4, 11, 5, 32, true});
    std::atomic<bool> failed{};
    std::vector<std::jthread> producers;
    for (std::uint32_t p = 0; p < 6; ++p) producers.emplace_back([&, p] {
        for (std::uint64_t i = 0; i < 5000; ++i) {
            if (!e->submit({i, p, static_cast<double>(p), i})) failed.store(true);
        }
    });
    producers.clear(); e->close(); auto s = e->snapshot();
    check(!failed.load() && s.has_value());
    check(s->accepted == 30000 && s->processed == 30000);
    for (std::uint32_t p = 0; p < 6; ++p) check(s->series[p].count == 5000 && s->series[p].mean == p);
    check(s->queue_high_watermark <= 44 && s->latency.count() == 30000);
}
void shutdown_races(bool cancel) {
    auto e = make_engine({3, 3, 2, 16, false});
    std::latch start(5);
    std::atomic<bool> wrong_error{};
    std::vector<std::jthread> producers;
    for (std::uint32_t p = 0; p < 4; ++p) producers.emplace_back([&, p] {
        start.arrive_and_wait();
        for (std::uint64_t i = 0; i < 50000; ++i) {
            auto r = e->submit({i, p, 1, i});
            if (!r) {
                if (r.error().code != strata::errc::closed && r.error().code != strata::errc::cancelled)
                    wrong_error.store(true);
                break;
            }
        }
    });
    start.arrive_and_wait();
    std::jthread stopper1([&] { if (cancel) e->cancel(); else e->close(); });
    std::jthread stopper2([&] { e->close(); });
    std::jthread stopper3([&] { if (cancel) e->cancel(); else e->close(); });
    stopper1.join(); stopper2.join(); stopper3.join(); producers.clear();
    auto s = e->snapshot(); check(s.has_value() && !wrong_error.load());
    check(s->accepted == s->processed + s->discarded, "every accepted event accounted for");
    if (!cancel) check(s->discarded == 0);
    check(s->latency.count() == 0, "latency disabled");
}
}
int main() {
    return test_main([] {
        check(!strata::engine::create({0, 1, 1, 1, true}));
        sequential(); concurrent();
        for (int i = 0; i < 12; ++i) { shutdown_races(false); shutdown_races(true); }
        auto e = make_engine({1, 1, 1, 1, true});
        std::stop_source stop; stop.request_stop();
        auto r = e->submit({1, 0, 1, 0}, stop.get_token());
        check(!r && r.error().code == strata::errc::cancelled);
        e->close(); auto s = e->snapshot(); check(s && s->accepted == 0);
        e->cancel(); e->close(); e->cancel(); check(e->snapshot()->accepted == 0);
        auto idle = make_engine({8, 1, 1, 2, true});
        check(idle->submit({1, 1, 2, 0}).has_value()); idle->close();
        check(idle->snapshot()->series[1].count == 1);
        // Destruction of an open, busy engine must drain without external stop.
        { auto scoped = make_engine({2, 7, 3, 4, true});
          for (std::uint64_t i = 0; i < 10000; ++i) check(scoped->submit({i, 0, 1, 0}).has_value()); }
    });
}

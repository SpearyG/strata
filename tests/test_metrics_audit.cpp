#include "check.hpp"

#include <strata/core/config.hpp>
#include <strata/metrics/latency_histogram.hpp>
#include <strata/pipeline/pipeline.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace {
using strata::metrics::latency_histogram;
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();

// Build a legitimate large sample count without overflowing any merge, and
// without requiring one observe call per represented sample.
latency_histogram repeated(std::uint64_t value, std::uint64_t count) {
    latency_histogram result, power;
    power.observe(value);
    while (count != 0) {
        if ((count & 1U) != 0) result.merge(power);
        count >>= 1U;
        if (count != 0) power.merge(power);
    }
    return result;
}

void bucket_edges() {
    std::array<std::uint64_t, 194> values{};
    std::size_t count{};
    values[count++] = 0;
    for (unsigned bit = 0; bit < 64; ++bit) {
        const auto power = std::uint64_t{1} << bit;
        values[count++] = power - 1;
        values[count++] = power;
        values[count++] = power + 1;
    }
    values[count++] = maximum;
    for (std::size_t i = 0; i < count; ++i) {
        const auto value = values[i];
        const auto index = std::min(static_cast<std::size_t>(std::bit_width(value)), std::size_t{63});
        latency_histogram single;
        single.observe(value);
        check(single.count() == 1 && single.buckets()[index] == 1, "bucket edge placement");
        for (std::size_t j = 0; j < single.buckets().size(); ++j) {
            if (j != index) check(single.buckets()[j] == 0);
        }
        check(single.percentile(0.5) == value && single.max_ns() == value);
        single.observe(maximum);
        const auto upper = index == 63 ? maximum : (std::uint64_t{1} << index) - 1;
        check(single.percentile(0.5) == upper, "reported bound matches bucket upper edge");
    }
}

void rank_boundaries() {
    latency_histogram h;
    h.observe(1); h.observe(1024);
    check(h.percentile(0.0) == 1 && h.percentile(-1.0) == 1);
    check(h.percentile(std::numeric_limits<double>::quiet_NaN()) == 1);
    check(h.percentile(-std::numeric_limits<double>::infinity()) == 1);
    check(h.percentile(std::numeric_limits<double>::infinity()) == 1024);
    check(h.percentile(1.0) == 1024 && h.percentile(2.0) == 1024);
    check(h.percentile(std::nextafter(0.0, 1.0)) == 1, "subnormal fraction");
    check(h.percentile(std::nextafter(0.5, 0.0)) == 1);
    check(h.percentile(0.5) == 1);
    check(h.percentile(std::nextafter(0.5, 1.0)) == 1024);
    check(h.percentile(std::nextafter(1.0, 0.0)) == 1024);

    // For the exact binary64 value .95, ceil(UINT64_MAX * .95) is
    // 17524406870024073216. The former long-double calculation rounded this
    // down by one and returned 1ns instead of a conservative 1024ns bound.
    constexpr std::uint64_t rank = 17524406870024073216ULL;
    auto huge = repeated(1, rank - 1);
    huge.merge(repeated(1024, maximum - (rank - 1)));
    check(huge.count() == maximum, "large valid merged count has not overflowed");
    check(huge.percentile(0.95) == 1024, "nearest rank must not round downward");
    auto lower = repeated(1, rank);
    lower.merge(repeated(1024, maximum - rank));
    check(lower.percentile(0.95) == 1, "exact rank remains in lower bucket");

    // Exercise binary exponent shifts on both sides of a 64-bit word boundary.
    for (unsigned exponent = 1; exponent <= 127; ++exponent) {
        const auto fraction = std::ldexp(1.0, -static_cast<int>(exponent));
        const auto exact_rank = exponent < 64 ? (std::uint64_t{1} << (64U - exponent)) : 1U;
        auto before = repeated(1, exact_rank - 1);
        before.merge(repeated(1024, maximum - (exact_rank - 1)));
        check(before.percentile(fraction) == 1024);
        auto at = repeated(1, exact_rank);
        at.merge(repeated(1024, maximum - exact_rank));
        check(at.percentile(fraction) == 1);
    }
}

void merge_equivalence() {
    latency_histogram whole, even, odd;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        const auto value = (i * 7919U) % 100'003U;
        whole.observe(value);
        if ((i & 1U) == 0) even.observe(value);
        else odd.observe(value);
    }
    even.merge(odd);
    even.merge(latency_histogram{});
    check(even.count() == whole.count() && even.max_ns() == whole.max_ns());
    check(even.buckets() == whole.buckets());
    for (const auto fraction : {0.0, 0.01, 0.1, 0.5, 0.95, 0.99, 1.0})
        check(even.percentile(fraction) == whole.percentile(fraction));
}

struct pass {
    bool operator()(strata::event&) const noexcept { return true; }
};
struct reject {
    bool operator()(strata::event&) const noexcept { return false; }
};
using stages = strata::pipeline<pass, reject, strata::validate_event>;
static_assert(std::same_as<stages::stage_type<0>, pass>);
static_assert(std::same_as<stages::stage_type<1>, reject>);
static_assert(std::same_as<stages::stage_type<2>, strata::validate_event>);
static_assert(strata::valid_topology<1, 1U << 24U, 1>());
static_assert(strata::valid_topology<256, 65536, 65536>());
static_assert(!strata::valid_topology<256, 1U << 24U, 1>());
static_assert(!strata::valid_topology<0, 1, 1>());
static_assert(!strata::valid_topology<1, std::numeric_limits<std::size_t>::max(), 1>());

void topology() {
    check(strata::validate({256, 65536, 65536, 1, false}).has_value());
    check(!strata::validate({256, 1U << 24U, 1, 1, false}), "total slot limit without multiplication overflow");
    check(!strata::validate({0, 1, 1, 1, false}));
    check(!strata::validate({1, std::numeric_limits<std::size_t>::max(), 1, 1, false}));
    auto p = strata::pipeline(pass{}, reject{}, strata::validate_event{1});
    strata::event event{};
    check(!p(event) && p.stage<2>().series_count == 1);
    auto empty = strata::pipeline<>();
    check(empty(event));
}
} // namespace

int main() {
    return test_main([] { bucket_edges(); rank_boundaries(); merge_equivalence(); topology(); });
}

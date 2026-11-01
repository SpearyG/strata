#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace strata::metrics {

// Single-owner histogram; merge only after worker join. Bucket 0 contains 0ns,
// bucket i contains [2^(i-1), 2^i-1]ns, with bucket 63 saturating to UINT64_MAX.
// Quantiles are bucket upper bounds, capped by the observed maximum.
class latency_histogram {
public:
    static constexpr std::size_t bucket_count = 64;

    void observe(std::uint64_t ns) noexcept {
        const auto index = std::min(static_cast<std::size_t>(std::bit_width(ns)), bucket_count - 1);
        ++buckets_[index];
        ++count_;
        maximum_ = std::max(maximum_, ns);
    }
    void merge(const latency_histogram& other) noexcept {
        for (std::size_t i = 0; i < bucket_count; ++i) {
            buckets_[i] += other.buckets_[i];
        }
        count_ += other.count_;
        maximum_ = std::max(maximum_, other.maximum_);
    }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t max_ns() const noexcept { return maximum_; }
    [[nodiscard]] const std::array<std::uint64_t, bucket_count>& buckets() const noexcept { return buckets_; }

    [[nodiscard]] std::uint64_t percentile(double fraction) const noexcept {
        if (count_ == 0) {
            return 0;
        }
        if (std::isnan(fraction) || fraction <= 0.0) {
            fraction = 0.0;
        }
        if (fraction >= 1.0) {
            return maximum_;
        }
        const auto rank = std::max(std::uint64_t{1}, static_cast<std::uint64_t>(
            std::ceil(static_cast<long double>(fraction) * static_cast<long double>(count_))));
        std::uint64_t cumulative{};
        for (std::size_t i = 0; i < bucket_count; ++i) {
            cumulative += buckets_[i];
            if (cumulative >= rank) {
                const auto upper = i == bucket_count - 1 ? std::numeric_limits<std::uint64_t>::max()
                                                        : (std::uint64_t{1} << i) - 1;
                return std::min(upper, maximum_);
            }
        }
        return maximum_;
    }

private:
    std::array<std::uint64_t, bucket_count> buckets_{};
    std::uint64_t count_{};
    std::uint64_t maximum_{};
};
} // namespace strata::metrics

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
        const auto rank = nearest_rank(count_, fraction);
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
    [[nodiscard]] static std::uint64_t nearest_rank(std::uint64_t count, double fraction) noexcept {
        static_assert(sizeof(double) == sizeof(std::uint64_t) &&
                      std::numeric_limits<double>::is_iec559 &&
                      std::numeric_limits<double>::digits == 53 &&
                      std::numeric_limits<double>::max_exponent == 1024,
                      "Strata latency ranks require IEEE-754 binary64");
        const auto bits = std::bit_cast<std::uint64_t>(fraction);
        const auto exponent = static_cast<unsigned>((bits >> 52U) & 0x7ffU);
        auto significand = bits & ((std::uint64_t{1} << 52U) - 1U);
        if (exponent != 0) {
            significand |= std::uint64_t{1} << 52U;
        }
        const auto shift = exponent == 0 ? 1074U : 1075U - exponent;
        if (shift >= 128U) {
            return 1;
        }

        // Exact 64x53-bit multiplication avoids rounding a positive fractional
        // rank down to an integer. This cold path needs at most 53 iterations.
        std::uint64_t high{}, low{}, add_high{}, add_low = count;
        while (significand != 0) {
            if ((significand & 1U) != 0) {
                const auto previous = low;
                low += add_low;
                high += add_high + static_cast<std::uint64_t>(low < previous);
            }
            add_high = (add_high << 1U) | (add_low >> 63U);
            add_low <<= 1U;
            significand >>= 1U;
        }
        std::uint64_t quotient{};
        bool remainder{};
        if (shift < 64U) {
            quotient = (high << (64U - shift)) | (low >> shift);
            remainder = (low & ((std::uint64_t{1} << shift) - 1U)) != 0;
        } else if (shift == 64U) {
            quotient = high;
            remainder = low != 0;
        } else {
            quotient = high >> (shift - 64U);
            remainder = low != 0 || (high & ((std::uint64_t{1} << (shift - 64U)) - 1U)) != 0;
        }
        return std::max(std::uint64_t{1}, quotient + static_cast<std::uint64_t>(remainder));
    }

    std::array<std::uint64_t, bucket_count> buckets_{};
    std::uint64_t count_{};
    std::uint64_t maximum_{};
};
} // namespace strata::metrics

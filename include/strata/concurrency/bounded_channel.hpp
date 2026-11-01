#pragma once

#include <algorithm>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>

#include <strata/core/error.hpp>

namespace strata::concurrency {

template<class T>
concept channel_item = std::default_initializable<T> && std::is_nothrow_default_constructible_v<T> &&
                       std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>;

struct channel_statistics {
    std::uint64_t accepted{};
    std::uint64_t rejected{};
    std::uint64_t discarded{};
    std::size_t depth{};
    std::size_t high_watermark{};
};

// Fixed-storage MPMC channel. All slot access and accounting share the same mutex,
// so accepting an item, closing admission and discarding items have linearization
// points independent of notification scheduling. Callers own all thread lifetimes.
template<channel_item T>
class bounded_channel final {
public:
    explicit bounded_channel(std::size_t capacity) : slots_(checked_capacity(capacity)) {}
    bounded_channel(const bounded_channel&) = delete;
    bounded_channel& operator=(const bounded_channel&) = delete;

    [[nodiscard]] std::expected<void, error> push(T item, std::stop_token stop = {}) {
        std::unique_lock lock(mutex_);
        // The queue lock keeps readiness stable; register cancellation only when
        // blocking, while checking stop before every acceptance on both paths.
        if ((state_ == state::open && size_ == slots_.size() &&
             !not_full_.wait(lock, stop, [this] { return state_ != state::open || size_ < slots_.size(); })) ||
            stop.stop_requested()) {
            ++rejected_;
            return std::unexpected(error{errc::cancelled, "submission cancelled"});
        }
        if (state_ != state::open) {
            return reject_closed();
        }
        put(std::move(item));
        lock.unlock();
        not_empty_.notify_one();
        return {};
    }

    [[nodiscard]] std::expected<void, error> try_push(T item) {
        std::unique_lock lock(mutex_);
        if (state_ != state::open) {
            return reject_closed();
        }
        if (size_ == slots_.size()) {
            ++rejected_;
            return std::unexpected(error{errc::full, "queue full"});
        }
        put(std::move(item));
        lock.unlock();
        not_empty_.notify_one();
        return {};
    }

    // Returns zero only when no more items can arrive. An empty destination is
    // a programming error rather than a shutdown signal.
    [[nodiscard]] std::size_t pop_batch(std::span<T> destination) {
        if (destination.empty()) {
            throw std::invalid_argument("pop_batch requires a nonempty destination");
        }
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this] { return size_ != 0 || state_ != state::open; });
        const auto count = std::min(size_, destination.size());
        for (std::size_t i = 0; i < count; ++i) {
            destination[i] = std::move(slots_[head_]);
            head_ = next(head_);
        }
        size_ -= count;
        lock.unlock();
        if (count != 0) {
            // A batch can release multiple producers, so notify_one is insufficient.
            not_full_.notify_all();
        }
        return count;
    }

    void close() noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (state_ == state::open) {
                state_ = state::draining;
            }
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    void cancel() noexcept {
        {
            const std::lock_guard lock(mutex_);
            state_ = state::cancelled;
            discarded_ += static_cast<std::uint64_t>(size_);
            // Release ownership now, including resources held by generic T.
            while (size_ != 0) {
                slots_[head_] = T{};
                head_ = next(head_);
                --size_;
            }
            tail_ = head_;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] channel_statistics statistics() const noexcept {
        const std::lock_guard lock(mutex_);
        return {accepted_, rejected_, discarded_, size_, high_watermark_};
    }

private:
    enum class state { open, draining, cancelled };
    [[nodiscard]] static std::size_t checked_capacity(std::size_t capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("channel capacity must be positive");
        }
        return capacity;
    }
    [[nodiscard]] std::size_t next(std::size_t index) const noexcept {
        return index + 1 == slots_.size() ? 0 : index + 1;
    }
    void put(T item) noexcept {
        slots_[tail_] = std::move(item);
        tail_ = next(tail_);
        ++size_;
        ++accepted_;
        high_watermark_ = std::max(high_watermark_, size_);
    }
    [[nodiscard]] std::expected<void, error> reject_closed() {
        ++rejected_;
        return std::unexpected(error{state_ == state::cancelled ? errc::cancelled : errc::closed,
                                     state_ == state::cancelled ? "queue cancelled" : "queue closed"});
    }

    std::vector<T> slots_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable_any not_full_;
    std::size_t head_{};
    std::size_t tail_{};
    std::size_t size_{};
    std::size_t high_watermark_{};
    std::uint64_t accepted_{};
    std::uint64_t rejected_{};
    std::uint64_t discarded_{};
    state state_{state::open};
};
} // namespace strata::concurrency

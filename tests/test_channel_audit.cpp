#include "check.hpp"
#include <strata/concurrency/bounded_channel.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <memory>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
// A valid move need not empty its source. This handle deliberately retains a
// shared reference, exposing resources held in logically empty ring slots.
struct retaining_move {
    std::shared_ptr<int> owner;
    retaining_move() noexcept = default;
    explicit retaining_move(std::shared_ptr<int> value) noexcept : owner(std::move(value)) {}
    retaining_move(const retaining_move&) = delete;
    retaining_move& operator=(const retaining_move&) = delete;
    retaining_move(retaining_move&& other) noexcept : owner(other.owner) {}
    retaining_move& operator=(retaining_move&& other) noexcept {
        owner = other.owner;
        return *this;
    }
};
static_assert(strata::concurrency::channel_item<retaining_move>);

// A trivial destructor does not imply trivial move assignment. Clearing a
// retained external handle can still have observable cleanup semantics.
struct assignment_cleanup {
    int handle{};
    unsigned* clears{};
    assignment_cleanup() noexcept = default;
    assignment_cleanup(int value, unsigned& counter) noexcept : handle(value), clears(&counter) {}
    assignment_cleanup(const assignment_cleanup&) = delete;
    assignment_cleanup& operator=(const assignment_cleanup&) = delete;
    assignment_cleanup(assignment_cleanup&&) noexcept = default;
    assignment_cleanup& operator=(assignment_cleanup&& other) noexcept {
        if (handle != 0 && other.handle == 0 && clears != nullptr) ++*clears;
        handle = other.handle;
        clears = other.clears;
        return *this;
    }
};
static_assert(strata::concurrency::channel_item<assignment_cleanup>);
static_assert(std::is_trivially_destructible_v<assignment_cleanup>);
static_assert(!std::is_trivially_copyable_v<assignment_cleanup>);

void popped_resources_are_released() {
    strata::concurrency::bounded_channel<retaining_move> queue(1);
    std::weak_ptr<int> observed;
    for (int iteration = 0; iteration < 8; ++iteration) {
        auto resource = std::make_shared<int>(iteration);
        observed = resource;
        retaining_move payload{std::move(resource)};
        check(queue.push(std::move(payload)).has_value());
        payload.owner.reset();
        std::array<retaining_move, 1> received;
        check(queue.pop_batch(received) == 1);
        check(received[0].owner && *received[0].owner == iteration);
        received[0].owner.reset();
        check(observed.expired(), "empty ring slots must not retain a popped resource");
        check(queue.statistics().depth == 0);
    }
    queue.cancel();
    check(observed.expired());

    unsigned clears{};
    strata::concurrency::bounded_channel<assignment_cleanup> external_handles(1);
    check(external_handles.push(assignment_cleanup{7, clears}).has_value());
    std::array<assignment_cleanup, 1> received;
    check(external_handles.pop_batch(received) == 1 && received[0].handle == 7);
    check(clears == 1, "trivial destructor must not bypass nontrivial slot assignment cleanup");
    external_handles.cancel();
    check(clears == 1, "an empty slot is reset exactly once");
}

void failed_sends_preserve_ownership() {
    strata::concurrency::bounded_channel<std::unique_ptr<int>> queue(1);
    check(queue.push(std::make_unique<int>(1)).has_value());

    auto full_payload = std::make_unique<int>(2);
    auto result = queue.try_push(std::move(full_payload));
    check(!result && result.error().code == strata::errc::full);
    check(full_payload && *full_payload == 2, "full send preserves producer ownership");

    std::stop_source cancelled;
    cancelled.request_stop();
    auto stopped_payload = std::make_unique<int>(3);
    result = queue.push(std::move(stopped_payload), cancelled.get_token());
    check(!result && result.error().code == strata::errc::cancelled);
    check(stopped_payload && *stopped_payload == 3, "pre-cancelled send preserves ownership");

    queue.close();
    auto closed_payload = std::make_unique<int>(4);
    result = queue.push(std::move(closed_payload));
    check(!result && result.error().code == strata::errc::closed);
    check(closed_payload && *closed_payload == 4, "closed send preserves ownership");

    queue.cancel();
    auto cancelled_payload = std::make_unique<int>(5);
    result = queue.try_push(std::move(cancelled_payload));
    check(!result && result.error().code == strata::errc::cancelled);
    check(cancelled_payload && *cancelled_payload == 5);
    check(queue.statistics().accepted == 1 && queue.statistics().discarded == 1);

    // Copyable lvalue callers retain their source and the original API's use.
    strata::concurrency::bounded_channel<int> copied(2);
    int source = 7;
    check(copied.push(source).has_value());
    check(copied.try_push(source).has_value());
    source = 9;
    std::array<int, 2> values;
    check(copied.pop_batch(values) == 2 && values[0] == 7 && values[1] == 7);
    copied.close();
}

void cancelled_waiter_preserves_ownership() {
    strata::concurrency::bounded_channel<std::unique_ptr<int>> queue(1);
    check(queue.push(std::make_unique<int>(1)).has_value());
    auto pending = std::make_unique<int>(2);
    std::stop_source cancellation;
    std::latch entering(1);
    bool rejected = false;
    std::jthread sender([&] {
        entering.count_down();
        const auto status = queue.push(std::move(pending), cancellation.get_token());
        rejected = !status && status.error().code == strata::errc::cancelled;
    });
    entering.wait();
    cancellation.request_stop();
    sender.join();
    check(rejected && pending && *pending == 2);
    check(queue.statistics().depth == 1 && queue.statistics().accepted == 1);
    queue.cancel();
}

void capacity_one_mpmc() {
    constexpr std::size_t items = 16000;
    strata::concurrency::bounded_channel<std::size_t> queue(1);
    std::vector<std::atomic<unsigned>> seen(items);
    std::atomic<bool> failed{};
    std::vector<std::jthread> consumers;
    for (unsigned c = 0; c < 4; ++c) {
        consumers.emplace_back([&] {
            std::array<std::size_t, 3> batch;
            while (const auto count = queue.pop_batch(batch)) {
                for (std::size_t i = 0; i < count; ++i) {
                    if (batch[i] >= items) failed.store(true, std::memory_order_relaxed);
                    else seen[batch[i]].fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    std::vector<std::jthread> producers;
    for (std::size_t p = 0; p < 4; ++p) {
        producers.emplace_back([&, p] {
            for (auto i = p; i < items; i += 4) {
                if (!queue.push(i)) failed.store(true, std::memory_order_relaxed);
            }
        });
    }
    producers.clear();
    queue.close();
    consumers.clear();
    check(!failed.load());
    for (const auto& count : seen) check(count.load() == 1, "capacity-one exact transfer");
    const auto counters = queue.statistics();
    check(counters.accepted == items && counters.discarded == 0 && counters.depth == 0);
    check(counters.high_watermark == 1);
}

void cancel_preserves_an_owned_batch() {
    strata::concurrency::bounded_channel<int> queue(4);
    for (int item = 1; item <= 4; ++item) check(queue.push(item).has_value());
    std::latch batch_owned(1), release_batch(1);
    bool correct_batch = false, saw_shutdown = false;
    std::size_t processed{};
    std::jthread consumer([&] {
        std::array<int, 2> batch;
        processed = queue.pop_batch(batch);
        correct_batch = processed == 2 && batch[0] == 1 && batch[1] == 2;
        batch_owned.count_down();
        release_batch.wait();
        saw_shutdown = queue.pop_batch(batch) == 0;
    });
    batch_owned.wait();
    queue.cancel();
    const auto cancelled = queue.statistics();
    release_batch.count_down();
    consumer.join();
    check(correct_batch && saw_shutdown);
    check(cancelled.accepted == 4 && cancelled.discarded == 2 && cancelled.depth == 0);
    check(cancelled.accepted == processed + cancelled.discarded);
}

void concurrent_close_and_cancel() {
    for (unsigned iteration = 0; iteration < 12; ++iteration) {
        strata::concurrency::bounded_channel<std::uint64_t> queue(3);
        check(queue.push(0).has_value());
        std::latch start(11), first_consumed(1);
        std::atomic<bool> failed{}, first{};
        std::atomic<std::uint64_t> processed{};
        std::vector<std::jthread> consumers, producers;
        for (unsigned c = 0; c < 4; ++c) {
            consumers.emplace_back([&] {
                start.arrive_and_wait();
                std::array<std::uint64_t, 2> batch;
                while (const auto count = queue.pop_batch(batch)) {
                    processed.fetch_add(count, std::memory_order_relaxed);
                    if (!first.exchange(true)) first_consumed.count_down();
                }
            });
        }
        for (std::uint64_t p = 0; p < 4; ++p) {
            producers.emplace_back([&, p] {
                start.arrive_and_wait();
                for (std::uint64_t i = p + 1; i < 12000; i += 4) {
                    const auto sent = queue.push(i);
                    if (!sent) {
                        if (sent.error().code != strata::errc::closed &&
                            sent.error().code != strata::errc::cancelled) failed.store(true);
                        break;
                    }
                }
            });
        }
        std::jthread closer([&] {
            start.arrive_and_wait();
            first_consumed.wait();
            for (unsigned i = 0; i < 32; ++i) queue.close();
        });
        std::jthread canceller([&] {
            start.arrive_and_wait();
            first_consumed.wait();
            for (unsigned i = 0; i < 32; ++i) queue.cancel();
        });
        start.arrive_and_wait();
        producers.clear();
        closer.join();
        canceller.join();
        consumers.clear();
        const auto counters = queue.statistics();
        check(!failed.load() && counters.depth == 0);
        check(counters.accepted == processed.load() + counters.discarded,
              "concurrent lifecycle counts include every accepted item");
        const auto after_shutdown = queue.try_push(42);
        check(!after_shutdown && after_shutdown.error().code == strata::errc::cancelled);
    }
}
} // namespace

int main() {
    return test_main([] {
        popped_resources_are_released();
        failed_sends_preserve_ownership();
        cancelled_waiter_preserves_ownership();
        capacity_one_mpmc();
        cancel_preserves_an_owned_batch();
        concurrent_close_and_cancel();
    });
}

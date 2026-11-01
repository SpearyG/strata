#include "check.hpp"

#if defined(__linux__) && defined(__LP64__)
#include <strata/runtime/engine.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <new>
#include <string_view>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace {
// Only the calling thread is fault-injected. Worker and sanitizer-runtime
// allocations remain untouched, including during startup and stack unwinding.
thread_local bool tracking{};
thread_local std::size_t allocations{};
thread_local std::size_t failure_index{};
thread_local bool inject_failure{};
thread_local std::size_t threads_when_failed{};

std::size_t process_threads();

void allocation_check() {
    if (!tracking) return;
    const auto index = allocations++;
    if (inject_failure && index == failure_index) {
        // One-shot failure: cleanup and exception reporting must be able to run.
        tracking = false;
        threads_when_failed = process_threads();
        throw std::bad_alloc{};
    }
}

std::size_t process_threads() {
    std::size_t count{};
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
        ++count;
    }
    return count;
}

void await_thread_count(std::size_t expected, std::string_view phase, std::size_t index = 0) {
    // Linux clears child_tid (waking pthread_join) in exit_mm_release before
    // exit_notify/release_task removes the task from /proc. Observe eventual
    // kernel teardown rather than requiring one immediate directory snapshot.
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(2);
    const auto first = process_threads();
    auto observed = first;
    std::size_t polls{};
    while (observed != expected && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
        ++polls;
        observed = process_threads();
    }
    if (first != expected) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started);
        std::cout << "startup task quiescence: phase=" << phase << " allocation_index=" << index
                  << " baseline=" << expected << " first=" << first << " final=" << observed
                  << " polls=" << polls << " elapsed_ns=" << elapsed.count() << '\n';
    }
    check(observed == expected, "startup task count did not return to baseline before deadline");
}

std::size_t warmed_thread_baseline() {
    // Warm sanitizer thread machinery with a known thread ID. Wait for that
    // exact task to disappear before counting persistent runtime threads; an
    // immediate post-join count could otherwise bake a dying task into baseline.
    long tid{};
    {
        std::jthread warmup([&] { tid = syscall(SYS_gettid); });
        warmup.join();
    }
    check(tid > 0, "warmup thread has a kernel task ID");
    const auto task_path = std::filesystem::path("/proc/self/task") / std::to_string(tid);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::filesystem::exists(task_path) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    check(!std::filesystem::exists(task_path), "warmup task disappeared before baseline capture");
    return process_threads();
}

struct creation_result {
    std::size_t allocation_count{};
    std::size_t threads_at_failure{};
    bool allocation_failed{};
    bool operational_error{};
};

creation_result exercise_creation(bool fail, std::size_t index = 0) {
    allocations = 0;
    failure_index = index;
    inject_failure = fail;
    threads_when_failed = 0;
    tracking = true;
    creation_result result;
    try {
        auto created = strata::engine::create({8, 3, 2, 3, false});
        tracking = false;
        result.operational_error = !created;
        if (created) (*created)->close();
    } catch (const std::bad_alloc&) {
        // All objects local to create(), including partial workers, have already
        // unwound before any thread-count assertion or test allocation occurs.
        tracking = false;
        result.allocation_failed = true;
    } catch (...) {
        tracking = false;
        throw;
    }
    result.allocation_count = allocations;
    result.threads_at_failure = threads_when_failed;
    return result;
}
} // namespace

// GNU-compatible --wrap redirects direct calls from project/test objects, while
// __real__ forwards to the toolchain's original allocator. Sanitizer allocation
// and pthread interceptors remain intact. These Itanium ABI names are for LP64
// GCC/Clang Linux only; the target supplies both matching --wrap linker options.
extern "C" void* __real__Znwm(std::size_t size);
extern "C" void* __real__ZnwmSt11align_val_t(std::size_t size, std::align_val_t alignment);
extern "C" void* __wrap__Znwm(std::size_t size) {
    allocation_check();
    return __real__Znwm(size);
}
extern "C" void* __wrap__ZnwmSt11align_val_t(std::size_t size, std::align_val_t alignment) {
    allocation_check();
    return __real__ZnwmSt11align_val_t(size, alignment);
}

int main() {
    return test_main([] {
        const auto baseline_threads = warmed_thread_baseline();
        // Warm library initialization before discovering allocation sites.
        const auto warmup = exercise_creation(false);
        check(!warmup.allocation_failed && !warmup.operational_error);
        await_thread_count(baseline_threads, "warmup");
        const auto baseline = exercise_creation(false);
        check(!baseline.allocation_failed && !baseline.operational_error);
        check(baseline.allocation_count > 0 && baseline.allocation_count <= 256,
              "bounded startup allocation sites");
        await_thread_count(baseline_threads, "successful-startup");

        bool failed_after_worker_start = false;
        for (std::size_t index = 0; index < baseline.allocation_count; ++index) {
            const auto failed = exercise_creation(true, index);
            check(failed.allocation_failed && !failed.operational_error,
                  "each discovered allocation site propagates bad_alloc");
            check(failed.allocation_count == index + 1, "one-shot failure reached selected site");
            await_thread_count(baseline_threads, "failed-startup", index);
            failed_after_worker_start |= failed.threads_at_failure > baseline_threads;
        }
        check(failed_after_worker_start, "fault sweep includes partial worker startup");
        const auto recovered = exercise_creation(false);
        check(!recovered.allocation_failed && !recovered.operational_error);
        await_thread_count(baseline_threads, "recovery");
        std::cout << "startup allocation sites checked: " << baseline.allocation_count << '\n';
    });
}
#else
int main() {
    std::cout << "startup audit skipped: LP64 Linux /proc thread accounting required\n";
    return 0;
}
#endif

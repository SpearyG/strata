#include <strata/runtime/engine.hpp>

#include <iostream>

int main() {
    strata::config settings;
    settings.workers = 2;
    settings.series_count = 8;
    auto runtime = strata::engine::create(settings);
    if (!runtime) { std::cerr << runtime.error().message << '\n'; return 1; }
    for (std::uint64_t i = 0; i < 10'000; ++i) {
        const auto result = (*runtime)->submit({i, static_cast<std::uint32_t>(i % 8), static_cast<double>(i % 100), 0});
        if (!result) { std::cerr << result.error().message << '\n'; return 1; }
    }
    (*runtime)->close();
    const auto snapshot = (*runtime)->snapshot();
    if (!snapshot) { std::cerr << snapshot.error().message << '\n'; return 1; }
    std::cout << "Processed " << snapshot->processed << " measurements across " << snapshot->series.size() << " series\n";
    for (const auto& series : snapshot->series)
        std::cout << "series=" << series.series << " count=" << series.count << " mean=" << series.mean << '\n';
}

#include <strata/runtime/engine.hpp>

int main() {
    auto runtime = strata::engine::create({2, 4, 2, 8, true});
    if (!runtime || !(*runtime)->submit({1, 7, 42, 0})) return 1;
    (*runtime)->close();
    const auto snapshot = (*runtime)->snapshot();
    return !snapshot || snapshot->processed != 1 || snapshot->series[7].mean != 42;
}

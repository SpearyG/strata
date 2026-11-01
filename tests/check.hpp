#pragma once
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>

inline void check(bool condition, const std::string& message = "check failed",
                  const std::source_location location = std::source_location::current()) {
    if (!condition) throw std::runtime_error(std::string(location.file_name()) + ":" +
        std::to_string(location.line()) + ": " + message);
}
template<class Function>
int test_main(Function&& function) {
    try { function(); std::cout << "all checks passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

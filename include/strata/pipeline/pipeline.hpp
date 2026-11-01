#pragma once

#include <concepts>
#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>
#include <strata/core/event.hpp>

namespace strata {
template<class T>
concept event_stage = requires(T& stage, event& e) {
    { stage(e) } noexcept -> std::same_as<bool>;
};

#if defined(__cpp_pack_indexing) && __cpp_pack_indexing >= 202311L && !defined(STRATA_FORCE_PACK_FALLBACK)
namespace detail {
template<std::size_t I, class... Types> requires (I < sizeof...(Types))
struct indexed_stage {
    using type = Types...[I];
};
} // namespace detail
#endif

// A failed stage short-circuits admission. Stages own their state by value;
// callers synchronize a pipeline if they give a stage mutable shared state.
template<event_stage... Stages>
class pipeline {
public:
    explicit constexpr pipeline(Stages... stages) : stages_(std::move(stages)...) {}

    template<std::size_t I> requires (I < sizeof...(Stages))
#if defined(__cpp_pack_indexing) && __cpp_pack_indexing >= 202311L && !defined(STRATA_FORCE_PACK_FALLBACK)
    // Defer indexing until this alias is requested, including for an empty pipeline.
    using stage_type = typename detail::indexed_stage<I, Stages...>::type;
#else
    using stage_type = std::tuple_element_t<I, std::tuple<Stages...>>;
#endif

    template<std::size_t I> requires (I < sizeof...(Stages))
    // Deduced return avoids GCC 15's incomplete type-pack-index ABI mangling.
    [[nodiscard]] constexpr auto& stage() noexcept { return std::get<I>(stages_); }

    [[nodiscard]] constexpr bool operator()(event& e) noexcept {
        return std::apply([&](auto&... stage) noexcept { return (stage(e) && ...); }, stages_);
    }

private:
    [[no_unique_address]] std::tuple<Stages...> stages_;
};
template<class... S> pipeline(S...) -> pipeline<S...>;

struct validate_event {
    std::uint32_t series_count;
    [[nodiscard]] bool operator()(event& e) const noexcept { return valid(e, series_count); }
};

// Compile-time constraints for fixed deployment profiles, without imposing
// template parameters on the runtime configurable engine API.
template<std::size_t Workers, std::size_t Capacity, std::size_t Batch>
consteval bool valid_topology() {
    return Workers > 0 && Workers <= 256 && Capacity > 0 &&
        Capacity <= (1U << 24U) && Batch > 0 && Batch <= Capacity &&
        Capacity <= (1U << 24U) / Workers;
}
} // namespace strata

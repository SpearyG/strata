#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <strata/core/error.hpp>
#include <strata/core/event.hpp>

namespace strata::storage {

inline constexpr std::size_t frame_header_bytes = 32;
inline constexpr std::size_t event_wire_bytes = 32;
inline constexpr std::size_t max_frame_events = 4096;

[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept;

// These parsers never retain spans. decode_frame accepts trailing bytes and
// reports precisely how much it consumed; malformed lengths cannot allocate
// more than max_frame_events records. Allocation failures propagate normally.
struct decoded_frame {
    std::vector<event> events;
    std::size_t consumed{};
};
[[nodiscard]] std::expected<std::vector<std::byte>, error>
encode_frame(std::span<const event> events);
[[nodiscard]] std::expected<decoded_frame, error>
decode_frame(std::span<const std::byte> bytes);

class journal_writer final {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<journal_writer>, error>
    open(const std::filesystem::path& path);
    ~journal_writer() = default;
    journal_writer(const journal_writer&) = delete;
    journal_writer& operator=(const journal_writer&) = delete;
    journal_writer(journal_writer&&) = delete;
    journal_writer& operator=(journal_writer&&) = delete;

    // One owner only. Validation failures write nothing; I/O failures can leave
    // an incomplete final frame. Empty append is a successful no-op.
    [[nodiscard]] std::expected<void, error> append(std::span<const event> events);
    // Flushes the C++ stream, not an fsync durability barrier. Explicit flush
    // is required to observe errors that a noexcept destructor cannot report.
    [[nodiscard]] std::expected<void, error> flush();

private:
    explicit journal_writer(const std::filesystem::path& path);
    std::ofstream stream_;
    std::uint64_t offset_{};
};

struct replay_result {
    std::uint64_t events{};
    std::uint64_t frames{};
    bool truncated_tail{};
};
using replay_callback = std::function<std::expected<void, error>(std::span<const event>)>;

// Callback is synchronous and must not retain its borrowed span. A complete
// corrupt frame is an error; a partial last frame is ignored and reported.
[[nodiscard]] std::expected<replay_result, error>
replay(const std::filesystem::path& path, const replay_callback& callback);

} // namespace strata::storage

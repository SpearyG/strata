#include <strata/storage/journal.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>

namespace strata::storage {
namespace {
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559,
              "Strata journal format requires IEEE-754 binary64");
constexpr std::array<std::byte, 8> magic{
    std::byte{'S'}, std::byte{'T'}, std::byte{'R'}, std::byte{'A'},
    std::byte{'T'}, std::byte{'A'}, std::byte{'2'}, std::byte{'6'}};

consteval auto make_crc_table() {
    std::array<std::uint32_t, 256> result{};
    for (std::uint32_t i = 0; i < result.size(); ++i) {
        auto word = i;
        for (unsigned bit = 0; bit < 8; ++bit) {
            word = (word >> 1U) ^ ((word & 1U) != 0 ? 0x82f63b78U : 0U);
        }
        result[i] = word;
    }
    return result;
}
constexpr auto crc_table = make_crc_table();

template <class T>
    requires(std::is_unsigned_v<T>)
void put(std::span<std::byte> bytes, std::size_t offset, T value) noexcept {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bytes[offset + i] = static_cast<std::byte>(value & T{0xff});
        value = static_cast<T>(value >> 8U);
    }
}

template <class T>
    requires(std::is_unsigned_v<T>)
T get(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    T result{};
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        result |= static_cast<T>(static_cast<T>(std::to_integer<unsigned>(bytes[offset + i]))
                                 << (i * 8U));
    }
    return result;
}

bool valid_value(double value) noexcept {
    return std::isfinite(value) && std::abs(value) <= 1.0e100;
}

struct header {
    std::uint32_t count;
    std::uint32_t payload_size;
    std::uint32_t payload_crc;
};

std::expected<header, error> parse_header(std::span<const std::byte> bytes) {
    if (bytes.size() < frame_header_bytes) {
        return std::unexpected(error{errc::truncated, "incomplete frame header"});
    }
    if (!std::equal(magic.begin(), magic.end(), bytes.begin())) {
        return std::unexpected(error{errc::corrupt, "invalid frame magic"});
    }
    if (get<std::uint32_t>(bytes, 24) != crc32c(bytes.first(24))) {
        return std::unexpected(error{errc::corrupt, "frame header checksum mismatch", 24});
    }
    if (get<std::uint16_t>(bytes, 8) != 1) {
        return std::unexpected(error{errc::unsupported, "unsupported journal version", 8});
    }
    if (get<std::uint16_t>(bytes, 10) != frame_header_bytes ||
        get<std::uint32_t>(bytes, 20) != 0) {
        return std::unexpected(error{errc::corrupt, "invalid frame header layout", 10});
    }
    const auto count = get<std::uint32_t>(bytes, 12);
    const auto payload = get<std::uint32_t>(bytes, 16);
    // Bound count before multiplying or allocating from an untrusted header.
    if (count == 0 || count > max_frame_events || payload != count * event_wire_bytes) {
        return std::unexpected(error{errc::corrupt, "invalid frame length", 12});
    }
    return header{count, payload, get<std::uint32_t>(bytes, 28)};
}

error at_offset(error value, std::uint64_t offset) {
    value.offset += offset;
    return value;
}
} // namespace

std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (auto value : bytes) {
        const auto index = (crc ^ std::to_integer<std::uint32_t>(value)) & 0xffU;
        crc = crc_table[index] ^ (crc >> 8U);
    }
    return ~crc;
}

std::expected<std::vector<std::byte>, error> encode_frame(std::span<const event> events) {
    if (events.empty() || events.size() > max_frame_events) {
        return std::unexpected(error{errc::invalid_config, "frame requires 1..4096 events"});
    }
    for (const auto& item : events) {
        if (!valid_value(item.value)) {
            return std::unexpected(error{errc::invalid_event, "non-finite or out-of-range value"});
        }
    }
    std::vector<std::byte> bytes(frame_header_bytes + events.size() * event_wire_bytes);
    std::copy(magic.begin(), magic.end(), bytes.begin());
    put<std::uint16_t>(bytes, 8, 1);
    put<std::uint16_t>(bytes, 10, static_cast<std::uint16_t>(frame_header_bytes));
    put<std::uint32_t>(bytes, 12, static_cast<std::uint32_t>(events.size()));
    put<std::uint32_t>(bytes, 16, static_cast<std::uint32_t>(events.size() * event_wire_bytes));
    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto offset = frame_header_bytes + i * event_wire_bytes;
        put<std::uint64_t>(bytes, offset, events[i].sequence);
        put<std::uint32_t>(bytes, offset + 8, events[i].series);
        put<std::uint64_t>(bytes, offset + 16, std::bit_cast<std::uint64_t>(events[i].value));
        put<std::uint64_t>(bytes, offset + 24, events[i].timestamp_ns);
    }
    put<std::uint32_t>(bytes, 24, crc32c(std::span<const std::byte>{bytes}.first(24)));
    put<std::uint32_t>(bytes, 28,
                       crc32c(std::span<const std::byte>{bytes}.subspan(frame_header_bytes)));
    return bytes;
}

std::expected<decoded_frame, error> decode_frame(std::span<const std::byte> bytes) {
    auto info = parse_header(bytes);
    if (!info) {
        return std::unexpected(info.error());
    }
    const auto frame_size = frame_header_bytes + info->payload_size;
    if (bytes.size() < frame_size) {
        return std::unexpected(error{errc::truncated, "incomplete frame payload", frame_header_bytes});
    }
    const auto payload = bytes.subspan(frame_header_bytes, info->payload_size);
    if (crc32c(payload) != info->payload_crc) {
        return std::unexpected(error{errc::corrupt, "frame payload checksum mismatch", 28});
    }
    decoded_frame result;
    result.consumed = frame_size;
    result.events.reserve(info->count);
    for (std::size_t i = 0; i < info->count; ++i) {
        const auto offset = i * event_wire_bytes;
        const auto value = std::bit_cast<double>(get<std::uint64_t>(payload, offset + 16));
        if (get<std::uint32_t>(payload, offset + 12) != 0 || !valid_value(value)) {
            return std::unexpected(error{errc::corrupt, "invalid event encoding",
                                         frame_header_bytes + offset});
        }
        result.events.push_back(event{get<std::uint64_t>(payload, offset),
                                      get<std::uint32_t>(payload, offset + 8), value,
                                      get<std::uint64_t>(payload, offset + 24)});
    }
    return result;
}

journal_writer::journal_writer(const std::filesystem::path& path)
    : stream_(path, std::ios::binary | std::ios::trunc) {}

std::expected<std::unique_ptr<journal_writer>, error>
journal_writer::open(const std::filesystem::path& path) {
    auto writer = std::unique_ptr<journal_writer>(new journal_writer(path));
    if (!writer->stream_) {
        return std::unexpected(error{errc::io_error, "cannot create journal"});
    }
    return writer;
}

std::expected<void, error> journal_writer::append(std::span<const event> events) {
    if (!stream_) {
        return std::unexpected(error{errc::io_error, "journal stream is not writable", offset_});
    }
    // Validate the whole call first: invalid data must not produce a partial append.
    for (const auto& item : events) {
        if (!valid_value(item.value)) {
            return std::unexpected(error{errc::invalid_event, "non-finite or out-of-range value", offset_});
        }
    }
    while (!events.empty()) {
        const auto count = std::min(events.size(), max_frame_events);
        auto bytes = encode_frame(events.first(count));
        if (!bytes) {
            return std::unexpected(at_offset(bytes.error(), offset_));
        }
        stream_.write(reinterpret_cast<const char*>(bytes->data()),
                      static_cast<std::streamsize>(bytes->size()));
        if (!stream_) {
            return std::unexpected(error{errc::io_error, "journal write failed", offset_});
        }
        offset_ += bytes->size();
        events = events.subspan(count);
    }
    return {};
}

std::expected<void, error> journal_writer::flush() {
    stream_.flush();
    if (!stream_) {
        return std::unexpected(error{errc::io_error, "journal flush failed", offset_});
    }
    return {};
}

std::expected<replay_result, error>
replay(const std::filesystem::path& path, const replay_callback& callback) {
    if (!callback) {
        return std::unexpected(error{errc::invalid_config, "replay callback is empty"});
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected(error{errc::io_error, "cannot open journal"});
    }
    replay_result result;
    std::uint64_t offset{};
    std::array<std::byte, frame_header_bytes> header_bytes{};
    for (;;) {
        stream.read(reinterpret_cast<char*>(header_bytes.data()),
                    static_cast<std::streamsize>(header_bytes.size()));
        const auto received = stream.gcount();
        if (received != static_cast<std::streamsize>(header_bytes.size())) {
            if (!stream.eof()) {
                return std::unexpected(error{errc::io_error, "journal header read failed", offset});
            }
            result.truncated_tail = received != 0;
            return result;
        }
        auto info = parse_header(header_bytes);
        if (!info) {
            return std::unexpected(at_offset(info.error(), offset));
        }
        std::vector<std::byte> frame(frame_header_bytes + info->payload_size);
        std::copy(header_bytes.begin(), header_bytes.end(), frame.begin());
        stream.read(reinterpret_cast<char*>(frame.data() + frame_header_bytes),
                    static_cast<std::streamsize>(info->payload_size));
        if (stream.gcount() != static_cast<std::streamsize>(info->payload_size)) {
            if (!stream.eof()) {
                return std::unexpected(error{errc::io_error, "journal payload read failed", offset});
            }
            result.truncated_tail = true;
            return result;
        }
        auto decoded = decode_frame(frame);
        if (!decoded) {
            return std::unexpected(at_offset(decoded.error(), offset));
        }
        if (auto status = callback(decoded->events); !status) {
            return std::unexpected(at_offset(status.error(), offset));
        }
        result.events += decoded->events.size();
        ++result.frames;
        offset += decoded->consumed;
    }
}
} // namespace strata::storage

#include <strata/storage/journal.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using strata::errc;
using strata::error;
using strata::event;
namespace storage = strata::storage;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class T>
void require(const std::expected<T, error>& result, const std::string& message) {
    if (!result) {
        throw std::runtime_error(message + ": " + result.error().message);
    }
}
void put32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::byte>(value & 0xffU);
        value >>= 8U;
    }
}
void refresh_header(std::span<std::byte> bytes) {
    put32(bytes, 24, storage::crc32c(bytes.first(24)));
}
void refresh_payload(std::span<std::byte> bytes) {
    put32(bytes, 28, storage::crc32c(bytes.subspan(storage::frame_header_bytes)));
}
void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.close();
    check(static_cast<bool>(stream), "write test fixture");
}
struct temporary_directory {
    std::filesystem::path path;
    temporary_directory() {
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i = 0; i < 100; ++i) {
            auto candidate = std::filesystem::temp_directory_path() /
                ("strata-storage-" + std::to_string(seed) + "-" + std::to_string(i));
            if (std::filesystem::create_directory(candidate)) {
                path = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("cannot create temporary test directory");
    }
    ~temporary_directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

const std::array<event, 3> sample{{
    {0x0102030405060708ULL, 0x11223344U, -0.0, 42},
    {2, 0, -1.0e100, std::numeric_limits<std::uint64_t>::max()},
    {3, std::numeric_limits<std::uint32_t>::max(), 1.25, 7}}};

void test_crc_and_roundtrip() {
    constexpr std::array<std::byte, 9> reference{
        std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'},
        std::byte{'6'}, std::byte{'7'}, std::byte{'8'}, std::byte{'9'}};
    check(storage::crc32c(reference) == 0xe3069283U, "CRC32C Castagnoli reference vector");
    check(storage::crc32c({}) == 0, "CRC32C empty vector");
    auto bytes = storage::encode_frame(sample);
    require(bytes, "encode sample");
    check(bytes->size() == 128, "wire frame size independent of event object padding");
    check((*bytes)[32] == std::byte{8} && (*bytes)[39] == std::byte{1}, "little endian sequence");
    check((*bytes)[40] == std::byte{0x44} && (*bytes)[43] == std::byte{0x11}, "little endian series");
    auto decoded = storage::decode_frame(*bytes);
    require(decoded, "decode sample");
    check(decoded->consumed == bytes->size(), "decoded byte count");
    check(std::equal(decoded->events.begin(), decoded->events.end(), sample.begin()), "roundtrip events");
    check(std::signbit(decoded->events.front().value), "preserve negative zero binary64");
    bytes->push_back(std::byte{0xff});
    auto trailing = storage::decode_frame(*bytes);
    require(trailing, "allow trailing frame bytes");
    check(trailing->consumed + 1 == bytes->size(), "trailing bytes excluded from consumed");
}

void test_parser_boundaries() {
    auto bytes = storage::encode_frame(sample);
    require(bytes, "encode fixture");
    for (std::size_t size = 0; size < bytes->size(); ++size) {
        auto decoded = storage::decode_frame(std::span<const std::byte>{*bytes}.first(size));
        check(!decoded && decoded.error().code == errc::truncated, "every prefix is truncated");
    }
    for (std::size_t i = 0; i < bytes->size(); ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            auto corrupted = *bytes;
            corrupted[i] ^= static_cast<std::byte>(1U << bit);
            check(!storage::decode_frame(corrupted), "every single-bit corruption rejected");
        }
    }
    auto forged = *bytes;
    put32(forged, 12, std::numeric_limits<std::uint32_t>::max());
    put32(forged, 16, std::numeric_limits<std::uint32_t>::max());
    refresh_header(forged);
    auto decoded = storage::decode_frame(forged);
    check(!decoded && decoded.error().code == errc::corrupt, "hostile count bounded before allocation");
    forged = *bytes;
    put32(forged, 12, 0);
    put32(forged, 16, 0);
    refresh_header(forged);
    check(!storage::decode_frame(forged), "zero-length frame rejected");
    forged = *bytes;
    forged[8] = std::byte{2};
    refresh_header(forged);
    decoded = storage::decode_frame(forged);
    check(!decoded && decoded.error().code == errc::unsupported, "unknown version rejected");
    forged = *bytes;
    forged[20] = std::byte{1};
    refresh_header(forged);
    check(!storage::decode_frame(forged), "header reserved word validated");
    forged = *bytes;
    forged[44] = std::byte{1};
    refresh_payload(forged);
    check(!storage::decode_frame(forged), "record reserved word validated");
    forged = *bytes;
    // Bytes for NaN need explicit little endian encoding even on big endian hosts.
    const auto bits = std::bit_cast<std::uint64_t>(std::numeric_limits<double>::quiet_NaN());
    for (std::size_t i = 0; i < sizeof(bits); ++i) {
        forged[48 + i] = static_cast<std::byte>((bits >> (i * 8U)) & 0xffU);
    }
    refresh_payload(forged);
    check(!storage::decode_frame(forged), "checksum-valid NaN rejected");
}

void test_encode_validation() {
    check(!storage::encode_frame({}), "empty frame rejected");
    std::vector<event> too_many(storage::max_frame_events + 1);
    check(!storage::encode_frame(too_many), "oversized frame rejected");
    for (const auto invalid : {std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN(), 1.01e100}) {
        auto values = sample;
        values[1].value = invalid;
        auto bytes = storage::encode_frame(values);
        check(!bytes && bytes.error().code == errc::invalid_event, "invalid value rejected");
    }
    std::mt19937_64 random(0x5a17U);
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        std::vector<event> values(1 + random() % 300);
        for (auto& item : values) {
            item = event{random(), static_cast<std::uint32_t>(random()),
                         static_cast<double>(random() % 1000000) / 31.0, random()};
        }
        auto bytes = storage::encode_frame(values);
        require(bytes, "randomized encode");
        auto decoded = storage::decode_frame(*bytes);
        require(decoded, "randomized decode");
        check(values == decoded->events, "randomized roundtrip");
    }
}

void test_journal_and_recovery() {
    temporary_directory fixture;
    const auto path = fixture.path / "events.strata";
    const auto collect = [](std::span<const event>) -> std::expected<void, error> { return {}; };
    check(!storage::journal_writer::open(fixture.path / "missing" / "file"), "open write error");
    check(!storage::replay(fixture.path / "missing", collect), "open replay error");
    auto writer = storage::journal_writer::open(path);
    require(writer, "open writer");
    require((*writer)->append({}), "empty append");
    require((*writer)->flush(), "empty flush");
    auto replayed = storage::replay(path, collect);
    require(replayed, "empty journal");
    check(replayed->events == 0 && !replayed->truncated_tail, "empty replay result");
    std::vector<event> values(storage::max_frame_events * 2 + 17);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = event{static_cast<std::uint64_t>(i), static_cast<std::uint32_t>(i % 1024),
                          static_cast<double>(i), 0};
    }
    require((*writer)->append(values), "append split across frames");
    auto invalid = values;
    invalid.back().value = std::numeric_limits<double>::infinity();
    check(!(*writer)->append(invalid), "invalid append rejected before any frame written");
    require((*writer)->flush(), "flush writer");
    writer->reset();
    std::vector<event> restored;
    replayed = storage::replay(path, [&restored](std::span<const event> frame) -> std::expected<void, error> {
        restored.insert(restored.end(), frame.begin(), frame.end());
        return {};
    });
    require(replayed, "replay complete journal");
    check(replayed->events == values.size() && replayed->frames == 3 && !replayed->truncated_tail,
          "multi-frame count");
    check(restored == values, "journal records match");
    auto failure = storage::replay(path, [](std::span<const event>) -> std::expected<void, error> {
        return std::unexpected(error{errc::cancelled, "callback cancelled"});
    });
    check(!failure && failure.error().code == errc::cancelled, "callback error propagated");
    check(!storage::replay(path, {}), "empty callback rejected");

    auto frame = storage::encode_frame(sample);
    require(frame, "encode tail fixture");
    for (std::size_t tail = 0; tail < frame->size(); ++tail) {
        auto file = *frame;
        file.insert(file.end(), frame->begin(), frame->begin() + static_cast<std::ptrdiff_t>(tail));
        write_file(path, file);
        replayed = storage::replay(path, collect);
        require(replayed, "every truncated tail recovered");
        check(replayed->events == sample.size() && replayed->frames == 1 &&
              replayed->truncated_tail == (tail != 0), "exact prefix recovery counts");
    }
    auto corrupt = *frame;
    corrupt.back() ^= std::byte{0x20};
    auto file = *frame;
    file.insert(file.end(), corrupt.begin(), corrupt.end());
    write_file(path, file);
    failure = storage::replay(path, collect);
    check(!failure && failure.error().code == errc::corrupt &&
          failure.error().offset == frame->size() + 28, "complete corruption rejects with file offset");
    writer = storage::journal_writer::open(path);
    require(writer, "reopen truncates");
    require((*writer)->flush(), "flush truncated file");
    writer->reset();
    check(std::filesystem::file_size(path) == 0, "open is explicitly create/truncate");

#if defined(__linux__)
    if (std::filesystem::exists("/dev/full")) {
        auto full = storage::journal_writer::open("/dev/full");
        require(full, "open full device");
        // A buffered write can succeed; the explicit flush must surface ENOSPC.
        const auto appended = (*full)->append(sample);
        const auto flushed = (*full)->flush();
        check(!appended || !flushed, "write/flush device failure surfaced");
    }
#endif
}

void test_bounded_random_bytes() {
    std::mt19937_64 random(0xc0decafeU);
    for (unsigned iteration = 0; iteration < 10000; ++iteration) {
        std::vector<std::byte> bytes(random() % 256);
        for (auto& byte : bytes) {
            byte = static_cast<std::byte>(random() & 0xffU);
        }
        const auto result = storage::decode_frame(bytes);
        check(!result, "random malformed input rejected");
    }
}
} // namespace

int main() {
    try {
        test_crc_and_roundtrip();
        test_parser_boundaries();
        test_encode_validation();
        test_journal_and_recovery();
        test_bounded_random_bytes();
        std::cout << "storage: 5 groups passed (CRC, codec, hostile input, journal, recovery)\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "storage test failure: " << failure.what() << '\n';
        return 1;
    }
}

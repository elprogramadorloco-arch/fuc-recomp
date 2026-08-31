#include "fuc/port/psp_mpeg.hpp"

namespace fuc::port::psp {
namespace {

[[nodiscard]] std::uint32_t read_be32(const std::span<const std::uint8_t> bytes,
                                      const std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24u) |
           (static_cast<std::uint32_t>(bytes[offset + 1u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 2u]) << 8u) |
           static_cast<std::uint32_t>(bytes[offset + 3u]);
}

[[nodiscard]] std::uint64_t read_timestamp(
    const std::span<const std::uint8_t> bytes,
    const std::size_t offset) noexcept {
    return static_cast<std::uint64_t>(bytes[offset + 5u]) |
           (static_cast<std::uint64_t>(bytes[offset + 4u]) << 8u) |
           (static_cast<std::uint64_t>(bytes[offset + 3u]) << 16u) |
           (static_cast<std::uint64_t>(bytes[offset + 2u]) << 24u) |
           (static_cast<std::uint64_t>(bytes[offset + 1u]) << 32u) |
           (static_cast<std::uint64_t>(bytes[offset]) << 36u);
}

} // namespace

bool parse_psmf_header(const std::span<const std::uint8_t> bytes,
                       PsmfHeader& header) noexcept {
    if (bytes.size() < 2048u || bytes[0] != 'P' || bytes[1] != 'S' ||
        bytes[2] != 'M' || bytes[3] != 'F') {
        return false;
    }

    PsmfHeader parsed{};
    parsed.raw_version = static_cast<std::uint32_t>(bytes[4]) |
                         (static_cast<std::uint32_t>(bytes[5]) << 8u) |
                         (static_cast<std::uint32_t>(bytes[6]) << 16u) |
                         (static_cast<std::uint32_t>(bytes[7]) << 24u);
    const bool supported_version = parsed.raw_version == 0x32313030u ||
                                   parsed.raw_version == 0x33313030u ||
                                   parsed.raw_version == 0x34313030u ||
                                   parsed.raw_version == 0x35313030u;
    if (!supported_version) return false;

    parsed.stream_offset = read_be32(bytes, 8u);
    parsed.stream_size = read_be32(bytes, 12u);
    parsed.first_timestamp = read_timestamp(bytes, 0x54u);
    parsed.last_timestamp = read_timestamp(bytes, 0x5Au);
    parsed.width = static_cast<std::uint32_t>(bytes[142u]) * 16u;
    parsed.height = static_cast<std::uint32_t>(bytes[143u]) * 16u;
    if (parsed.stream_offset < 2048u || parsed.stream_size == 0u ||
        parsed.last_timestamp < parsed.first_timestamp || parsed.width == 0u ||
        parsed.height == 0u || parsed.width > 480u || parsed.height > 272u) {
        return false;
    }

    header = parsed;
    return true;
}

} // namespace fuc::port::psp

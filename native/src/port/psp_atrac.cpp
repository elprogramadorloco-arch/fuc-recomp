#include "fuc/port/psp_atrac.hpp"

#include <cstring>

namespace fuc::port::psp {
namespace {

std::uint32_t read_le32(const std::span<const std::uint8_t> bytes,
                        const std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

} // namespace

AtracRiffMetadata parse_atrac_riff_metadata(
    const std::span<const std::uint8_t> bytes) noexcept {
    AtracRiffMetadata result{};
    if (bytes.size() < 12u || std::memcmp(bytes.data(), "RIFF", 4u) != 0 ||
        std::memcmp(bytes.data() + 8u, "WAVE", 4u) != 0)
        return result;

    for (std::size_t offset = 12u; offset + 8u <= bytes.size();) {
        const std::uint32_t length = read_le32(bytes, offset + 4u);
        const std::uint64_t data_end = static_cast<std::uint64_t>(offset) + 8u + length;
        if (data_end > bytes.size()) break;
        if (std::memcmp(bytes.data() + offset, "fact", 4u) == 0 && length >= 4u) {
            result.total_samples = read_le32(bytes, offset + 8u);
        } else if (std::memcmp(bytes.data() + offset, "smpl", 4u) == 0 &&
                   length >= 60u && read_le32(bytes, offset + 36u) != 0u) {
            const std::uint32_t start = read_le32(bytes, offset + 52u);
            const std::uint32_t end = read_le32(bytes, offset + 56u);
            if (start <= end) {
                result.loop_start = start;
                result.loop_end = end;
                result.has_loop = true;
            }
        }
        offset = static_cast<std::size_t>(data_end + (length & 1u));
    }
    if (result.has_loop && result.total_samples != 0u &&
        result.loop_end >= result.total_samples)
        result.has_loop = false;
    return result;
}

} // namespace fuc::port::psp

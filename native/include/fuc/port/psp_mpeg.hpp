#pragma once

#include <cstdint>
#include <span>

namespace fuc::port::psp {

struct PsmfHeader {
    std::uint32_t raw_version{};
    std::uint32_t stream_offset{};
    std::uint32_t stream_size{};
    std::uint64_t first_timestamp{};
    std::uint64_t last_timestamp{};
    std::uint32_t width{};
    std::uint32_t height{};
};

// Parses the fixed 2048-byte PSP Movie Format header.  Only firmware versions
// documented by the PSP runtime (0012..0015) are accepted.
[[nodiscard]] bool parse_psmf_header(std::span<const std::uint8_t> bytes,
                                     PsmfHeader& header) noexcept;

} // namespace fuc::port::psp

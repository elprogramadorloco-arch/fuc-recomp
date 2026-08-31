#pragma once

#include <cstdint>
#include <span>

namespace fuc::port::psp {

struct AtracRiffMetadata {
    std::uint32_t total_samples{};
    std::uint32_t loop_start{};
    std::uint32_t loop_end{};
    bool has_loop{};
};

// Read the PSP ATRAC RIFF metadata supplied by the game.  The `fact` chunk
// carries the decoded length and the first `smpl` loop record carries the
// inclusive loop points used by sceAtracSetLoopNum.
[[nodiscard]] AtracRiffMetadata parse_atrac_riff_metadata(
    std::span<const std::uint8_t> bytes) noexcept;

} // namespace fuc::port::psp

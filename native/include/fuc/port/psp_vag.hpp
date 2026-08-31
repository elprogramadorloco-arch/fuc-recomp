#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fuc::port::psp {

struct VagDecodeResult {
    std::vector<std::int16_t> samples;
    std::size_t loop_start_sample{};
    bool loop_start_valid{};
    std::uint8_t terminal_flag{};
    std::size_t terminal_block_offset{};
};

// Decode PSP SAS/PSX ADPCM blocks. Marker 6 starts a loop, marker 3 ends it,
// marker 7 is a non-audio terminator, and marker 1 ends after its own samples.
[[nodiscard]] VagDecodeResult decode_vag_adpcm(
    std::span<const std::uint8_t> source);

// The SAS loop-mode argument only enables loop markers embedded in the VAG.
// It does not turn a finite flag-1/flag-7 stream into a whole-buffer loop.
[[nodiscard]] bool vag_should_loop(const VagDecodeResult& decoded,
                                   bool loop_mode) noexcept;

} // namespace fuc::port::psp

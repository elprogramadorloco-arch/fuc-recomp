#include "fuc/port/psp_vag.hpp"

#include <algorithm>
#include <array>

namespace fuc::port::psp {
namespace {

constexpr std::array<std::int32_t, 16> filter0{
    0, 60, 115, 98, 122, 0, 0, 52, 55, 60, 0, 0, 0, 2, 125, 0};
constexpr std::array<std::int32_t, 16> filter1{
    0, 0, -52, -55, -60, 0, 0, 0, -2, -125, 0, -216, -6, -151};

} // namespace

VagDecodeResult decode_vag_adpcm(const std::span<const std::uint8_t> source) {
    VagDecodeResult result{};
    result.samples.reserve((source.size() / 16u) * 28u);
    std::int32_t history_one = 0;
    std::int32_t history_two = 0;
    for (std::size_t block = 0u; block + 16u <= source.size(); block += 16u) {
        const std::uint8_t header = source[block];
        const std::uint8_t flags = source[block + 1u];
        if (flags == 7u) {
            result.terminal_flag = flags;
            result.terminal_block_offset = block;
            break;
        }
        if (flags == 6u && !result.loop_start_valid) {
            result.loop_start_sample = result.samples.size();
            result.loop_start_valid = true;
        }
        const std::uint32_t predictor = (header >> 4u) & 0xFu;
        const std::uint32_t shift = header & 0xFu;
        for (std::size_t index = 0u; index < 28u; ++index) {
            const std::uint8_t packed = source[block + 2u + index / 2u];
            std::int32_t nibble = (index & 1u) != 0u
                ? static_cast<std::int32_t>(packed >> 4u)
                : static_cast<std::int32_t>(packed & 0xFu);
            if ((nibble & 8) != 0) nibble -= 16;
            std::int32_t sample = (nibble << 12u) >> shift;
            sample += (history_one * filter0[predictor] +
                       history_two * filter1[predictor] + 32) / 64;
            sample = std::clamp(sample, -32768, 32767);
            history_two = history_one;
            history_one = sample;
            result.samples.push_back(static_cast<std::int16_t>(sample));
        }
        if (flags == 1u || flags == 3u) {
            result.terminal_flag = flags;
            result.terminal_block_offset = block;
            break;
        }
    }
    return result;
}

bool vag_should_loop(const VagDecodeResult& decoded,
                     const bool loop_mode) noexcept {
    return loop_mode && decoded.terminal_flag == 3u;
}

} // namespace fuc::port::psp

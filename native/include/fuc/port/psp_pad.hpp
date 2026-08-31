#pragma once

#include "fuc/port/player_input.hpp"

#include <cstdint>

namespace fuc::port {

// PSP hardware button bits, as `sceCtrlReadBufferPositive` reports them.
//
// This is the one place the port speaks the guest's language. Everything above
// it uses `Button`, which is device- and platform-neutral; everything below is
// the recompiled module reading its own pad.
enum class PspButton : std::uint32_t {
    select = 0x000001u,
    start = 0x000008u,
    up = 0x000010u,
    right = 0x000020u,
    down = 0x000040u,
    left = 0x000080u,
    left_trigger = 0x000100u,
    right_trigger = 0x000200u,
    triangle = 0x001000u,
    circle = 0x002000u,
    cross = 0x004000u,
    square = 0x008000u,
};

// The PSP analogue stick reports 0..255 with the centre at 128.
inline constexpr std::uint8_t psp_analog_centre = 128u;

// Translates one player's input into the guest's pad word.
//
// The face-button mapping follows the port's existing controls: Square is light,
// Triangle medium, Circle heavy, Cross guard. That matches what the native port
// already does, so a player's muscle memory carries across.
[[nodiscard]] std::uint32_t to_psp_buttons(const PhysicalInputState& input) noexcept;

// Maps an axis in [-1, 1] onto the guest's 0..255 with the centre at 128.
[[nodiscard]] std::uint8_t to_psp_analog(float axis) noexcept;

} // namespace fuc::port

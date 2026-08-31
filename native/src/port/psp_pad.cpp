#include "fuc/port/psp_pad.hpp"

#include <algorithm>
#include <cmath>

namespace fuc::port {
namespace {

constexpr std::uint32_t bit(const PspButton button) noexcept {
    return static_cast<std::uint32_t>(button);
}

} // namespace

std::uint32_t to_psp_buttons(const PhysicalInputState& input) noexcept {
    std::uint32_t buttons = 0u;
    if (input.is_held(Button::up)) buttons |= bit(PspButton::up);
    if (input.is_held(Button::down)) buttons |= bit(PspButton::down);
    if (input.is_held(Button::left)) buttons |= bit(PspButton::left);
    if (input.is_held(Button::right)) buttons |= bit(PspButton::right);
    if (input.is_held(Button::light)) buttons |= bit(PspButton::square);
    if (input.is_held(Button::medium)) buttons |= bit(PspButton::triangle);
    if (input.is_held(Button::heavy)) buttons |= bit(PspButton::circle);
    if (input.is_held(Button::guard)) buttons |= bit(PspButton::cross);
    if (input.is_held(Button::start)) buttons |= bit(PspButton::start);
    if (input.is_held(Button::select)) buttons |= bit(PspButton::select);
    if (input.is_held(Button::shoulder_left)) buttons |= bit(PspButton::left_trigger);
    if (input.is_held(Button::shoulder_right)) buttons |= bit(PspButton::right_trigger);
    // `Button::special` has no PSP equivalent: the hardware has no spare button.
    // It is deliberately dropped here rather than aliased onto one that exists,
    // which would make two different actions indistinguishable to the guest.
    return buttons;
}

std::uint8_t to_psp_analog(const float axis) noexcept {
    if (std::isnan(axis)) return psp_analog_centre;
    const float clamped = std::clamp(axis, -1.0f, 1.0f);
    // 127 either side of the centre keeps the neutral position exactly at 128
    // and the extremes reachable at 1 and 255.
    const float scaled = static_cast<float>(psp_analog_centre) + clamped * 127.0f;
    return static_cast<std::uint8_t>(std::lround(scaled));
}

} // namespace fuc::port

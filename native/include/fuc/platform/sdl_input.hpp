#pragma once

#include "fuc/config/game_config.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <optional>
#include <string>

namespace fuc {

[[nodiscard]] std::optional<InputAction> action_for_keyboard(
    const ControlBindings& bindings, SDL_Scancode scancode) noexcept;
[[nodiscard]] std::optional<InputAction> action_for_gamepad(
    const ControlBindings& bindings, SDL_GamepadButton button) noexcept;
[[nodiscard]] std::string keyboard_binding_name(SDL_Scancode scancode);
[[nodiscard]] std::string gamepad_binding_name(SDL_GamepadButton button);

} // namespace fuc

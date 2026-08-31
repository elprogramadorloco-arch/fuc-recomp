#pragma once

#include "fuc/port/port_battle_state.hpp"

#include <array>
#include <cstdint>

namespace fuc::port {

inline constexpr std::uint8_t no_input_device = 0xFFu;

// Buttons as the game understands them, independent of any device.
//
// The recompiled logic must never see an SDL type. The platform layer turns a
// gamepad or a keyboard into this, and only this crosses into the bridge.
enum class Button : std::uint16_t {
    up = 0,
    down,
    left,
    right,
    light,
    medium,
    heavy,
    guard,
    special,
    start,
    select,
    shoulder_left,
    shoulder_right,
    count,
};

inline constexpr std::size_t button_count = static_cast<std::size_t>(Button::count);

[[nodiscard]] constexpr std::uint16_t button_mask(const Button button) noexcept {
    return static_cast<std::uint16_t>(1u << static_cast<std::uint16_t>(button));
}

// One device's state at one instant, already normalised.
struct PhysicalInputState final {
    std::uint16_t held{};
    // Analogue stick, each axis in [-1, 1]. Kept separate from the digital
    // directions because the guest may read both and collapsing them early
    // would lose the distinction.
    float analog_x{};
    float analog_y{};

    [[nodiscard]] bool is_held(const Button button) const noexcept {
        return (held & button_mask(button)) != 0u;
    }
};

// One player's input for one tick, with the device it came from.
//
// `device` is carried so a bug where two slots read the same pad is visible in
// the data rather than only in behaviour.
struct PlayerInputState final {
    PlayerId player{PlayerId::one};
    std::uint8_t device{no_input_device};
    PhysicalInputState input{};
    // Edges relative to the previous tick, computed by the sampler.
    std::uint16_t pressed{};
    std::uint16_t released{};
};

// Both players' input, frozen for one simulation tick.
//
// The freeze matters: both players are sampled BEFORE the logical step and
// neither can change during it. Sampling inside the step would let whichever
// player is processed first react to a newer world than the other.
struct InputSnapshot final {
    std::uint64_t host_tick{};
    std::array<PlayerInputState, player_count> players{};

    [[nodiscard]] PlayerInputState& player(PlayerId id) noexcept {
        return players[player_index(id)];
    }
    [[nodiscard]] const PlayerInputState& player(PlayerId id) const noexcept {
        return players[player_index(id)];
    }
};

// Turns raw held-button states into a frozen snapshot with edges.
//
// Holds one previous state per player, so the two are genuinely independent:
// there is no shared "last frame" that would let one player's release cancel
// the other's press.
class InputSampler final {
public:
    // Records what a device currently reports for one player. Call once per
    // player per tick, before `freeze`.
    void set_player_state(PlayerId player, std::uint8_t device,
                          const PhysicalInputState& state) noexcept;

    // Produces the snapshot for `host_tick` and rolls the edge state forward.
    [[nodiscard]] InputSnapshot freeze(std::uint64_t host_tick) noexcept;

private:
    std::array<PhysicalInputState, player_count> current_{};
    std::array<PhysicalInputState, player_count> previous_{};
    std::array<std::uint8_t, player_count> devices_{no_input_device,
                                                    no_input_device};
};

} // namespace fuc::port

#include "fuc/port/player_input.hpp"

namespace fuc::port {

void InputSampler::set_player_state(const PlayerId player, const std::uint8_t device,
                                    const PhysicalInputState& state) noexcept {
    const auto index = player_index(player);
    current_[index] = state;
    devices_[index] = device;
}

InputSnapshot InputSampler::freeze(const std::uint64_t host_tick) noexcept {
    InputSnapshot snapshot{};
    snapshot.host_tick = host_tick;
    for (std::size_t index = 0; index < player_count; ++index) {
        const std::uint16_t now = current_[index].held;
        const std::uint16_t before = previous_[index].held;
        auto& slot = snapshot.players[index];
        slot.player = static_cast<PlayerId>(index);
        slot.device = devices_[index];
        slot.input = current_[index];
        // Edges are computed per player against that player's own previous
        // state. A single shared "last frame" would let one player's release
        // mask the other's press.
        slot.pressed = static_cast<std::uint16_t>(now & ~before);
        slot.released = static_cast<std::uint16_t>(~now & before);
        previous_[index] = current_[index];
    }
    return snapshot;
}

} // namespace fuc::port

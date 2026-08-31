#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fuc::port {

enum class PlayerId : std::uint8_t { one = 0, two = 1, count = 2 };

inline constexpr std::size_t player_count = static_cast<std::size_t>(PlayerId::count);

[[nodiscard]] constexpr std::size_t player_index(const PlayerId player) noexcept {
    return static_cast<std::size_t>(player);
}

// Structured diagnostics captured atomically from the recompiled PSP guest.
// This is not the graphics interface: the future renderer consumes PSP render
// commands, not this battle summary.
//
// Versioned on purpose. The guest side of this contract is discovered by
// measurement and will keep changing; a version number makes a mismatch a
// loud failure instead of a silently misread field.
inline constexpr std::uint32_t battle_state_contract_version = 1u;

// How much a field can be trusted.
//
// Most PSP addresses are not located yet, and that must not force either
// inventing them or delaying the contract. A field that is not yet found is
// `unresolved` and readers skip it; a field derived from measurement that has
// not been cross-checked is `provisional`.
enum class Confidence : std::uint8_t { unresolved = 0, provisional = 1, verified = 2 };

template <typename T>
struct Tracked final {
    T value{};
    Confidence confidence{Confidence::unresolved};

    [[nodiscard]] bool usable() const noexcept {
        return confidence != Confidence::unresolved;
    }
};

struct Vec3 final {
    float x{};
    float y{};
    float z{};
};

// One observed fighter. Both fighters always come from the same logical
// instant; see `BattleSnapshot`.
struct FighterState final {
    Tracked<Vec3> position{};
    // +1 facing right, -1 facing left. Kept as a float because the guest may
    // express it as one and rounding it early would lose a turning state.
    Tracked<float> facing{};
    // Opaque identifiers from the guest. Diagnostics never interpret them.
    Tracked<std::uint32_t> action_id{};
    Tracked<std::uint32_t> animation_id{};
    Tracked<float> animation_frame{};
    Tracked<std::int32_t> health{};
    Tracked<std::int32_t> meter{};
    Tracked<std::uint32_t> flags{};
};

// Raw camera values read from guest memory. This diagnostic structure never
// derives, corrects or converts the camera for presentation.
struct CameraState final {
    Tracked<Vec3> position{};
};

enum class MatchPhase : std::uint8_t {
    unknown = 0,
    intro,
    fighting,
    round_over,
    match_over,
};

struct MatchState final {
    Tracked<MatchPhase> phase{};
    Tracked<std::uint8_t> round{};
    // Rounds won, per player, indexed by `player_index`.
    Tracked<std::array<std::uint8_t, player_count>> rounds_won{};
    // Remaining round time in guest units. Absent when the round is untimed.
    Tracked<std::int32_t> timer{};
};

// One complete, self-consistent picture of the battle.
//
// Every field in a snapshot belongs to the SAME logical tick. Mixing fighter
// one from tick N with fighter two from tick N+1 would put the renderer half a
// frame out of step and make collisions look wrong; the cooperative HLE lets
// the host stop the world, so capturing atomically costs nothing.
struct BattleSnapshot final {
    std::uint32_t contract_version{battle_state_contract_version};
    // Host simulation tick that produced this snapshot.
    std::uint64_t host_tick{};
    // The guest's own logical tick, when the port can observe it. Kept
    // separately because a guest subsystem may run at a different rate.
    Tracked<std::uint64_t> guest_tick{};

    std::array<FighterState, player_count> fighters{};
    CameraState camera{};
    MatchState match{};

    [[nodiscard]] FighterState& fighter(PlayerId id) noexcept {
        return fighters[player_index(id)];
    }
    [[nodiscard]] const FighterState& fighter(PlayerId id) const noexcept {
        return fighters[player_index(id)];
    }

    // True when both fighter positions were observed in the same stopped guest
    // tick. This is a diagnostics predicate, not a renderer contract.
    [[nodiscard]] bool has_fighter_positions() const noexcept {
        return fighters[0].position.usable() && fighters[1].position.usable();
    }
};

} // namespace fuc::port

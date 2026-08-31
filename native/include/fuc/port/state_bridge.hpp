#pragma once

#include "fuc/port/player_input.hpp"
#include "fuc/port/port_battle_state.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace fuc::port {

// Where a piece of guest state lives, for one specific executable.
//
// Addresses are NOT part of the contract's meaning. The same field can sit
// somewhere else in another build, so the semantic definition above and the
// address map here are separate on purpose, and a map is only valid for the
// binary whose hash it names.
struct GuestFieldAddress final {
    // Absolute guest address when `base_pointer` is zero; otherwise the offset
    // of the field inside the object.
    std::uint32_t address{};
    Confidence confidence{Confidence::unresolved};

    // Address of a word holding a pointer to the object that owns the field.
    //
    // Fighters live on the heap, so their absolute addresses are only good for
    // as long as the allocator repeats itself. What does not move is the static
    // word the game keeps the pointer in, so the map stores that and the bridge
    // follows it. A field with no `base_pointer` is a plain global.
    std::uint32_t base_pointer{};
};

struct GuestBattleMap final {
    // SHA-256 of the executable this map was measured against. A map must never
    // be applied to a different binary.
    std::string executable_sha256;

    struct FighterFields final {
        GuestFieldAddress position_x{};
        GuestFieldAddress position_y{};
        GuestFieldAddress position_z{};
        GuestFieldAddress facing{};
        GuestFieldAddress action_id{};
        GuestFieldAddress animation_id{};
        GuestFieldAddress animation_frame{};
        GuestFieldAddress health{};
        // Health is not stored anywhere the port can reach durably, but the two
        // numbers it is made of are, inside the fighter object. When both are
        // mapped the bridge derives `health` from them; see
        // `health_per_damage_unit`.
        GuestFieldAddress max_health{};
        GuestFieldAddress damage_taken{};
        GuestFieldAddress meter{};
        GuestFieldAddress flags{};
    };
    std::array<FighterFields, player_count> fighters{};

    struct MatchFields final {
        GuestFieldAddress phase{};
        GuestFieldAddress round{};
        GuestFieldAddress timer{};
    };
    MatchFields match{};

    // Raw camera words written by the PSP game, retained only for diagnostics.
    struct CameraFields final {
        GuestFieldAddress position_x{};
        GuestFieldAddress position_y{};
        GuestFieldAddress position_z{};
    };
    CameraFields camera{};
};

// Reads primitive values out of the guest. Supplied by whatever is hosting the
// recompiled logic, so this header stays free of PSPRecomp types.
struct GuestMemoryReader final {
    std::function<bool(std::uint32_t address, std::uint32_t& out)> read_u32;
    std::function<bool(std::uint32_t address, float& out)> read_f32;
};

// Structured diagnostics reader for stopped guest memory. It is not a graphics
// bridge and does not feed the future renderer; that backend must consume PSP
// render commands produced by the recompiled game.
class PspStateBridge final {
public:
    PspStateBridge(GuestMemoryReader reader, GuestBattleMap map) noexcept;

    // Captures both fighters, the camera and the match state as one atomic
    // picture.
    //
    // Must be called with the guest STOPPED, between logical steps. The
    // cooperative HLE makes that free, and it is what guarantees the two
    // fighters belong to the same instant.
    [[nodiscard]] BattleSnapshot capture(std::uint64_t host_tick) const;

    // Fields whose address is unresolved stay `Confidence::unresolved` in the
    // snapshot; nothing is invented to fill them.
    [[nodiscard]] const GuestBattleMap& map() const noexcept { return map_; }

private:
    GuestMemoryReader reader_;
    GuestBattleMap map_;
};

// How much health one unit of the guest's damage counter costs.
//
// Measured by landing attacks and watching both numbers: the counter rises 700
// per heavy hit while health falls 1400. Nine samples agreed except while the
// on-screen bar was still draining, where the visible word trailed the computed
// value by one unit -- which is itself the evidence that the bar animates and
// the computed value is the real one.
//
// Only confirmed against one character. The maximum differs per fighter (10500
// against 10000 in the pair measured), so this factor being universal is an
// assumption, and that is why derived health is never better than provisional.
inline constexpr std::int32_t health_per_damage_unit = 2;

} // namespace fuc::port

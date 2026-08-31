#include "fuc/port/state_bridge.hpp"

#include <algorithm>
#include <utility>

namespace fuc::port {
namespace {

// Reads one field, leaving it `unresolved` when the map does not know where it
// is or the read fails. Never substitutes a plausible default: an invented
// value would flow into the renderer and look like real state.
template <typename T, typename Read, typename ReadPointer>
void read_field(Tracked<T>& target, const GuestFieldAddress& where, Read&& read,
                ReadPointer&& read_pointer) {
    if (where.confidence == Confidence::unresolved) return;

    std::uint32_t address = where.address;
    if (where.base_pointer != 0u) {
        // The owning object is on the heap. Follow the static word that holds
        // it, and give up quietly if that word is null: between matches there is
        // no fighter, and reading offset 0x460 of nothing would produce a number
        // that looks like a position.
        std::uint32_t base{};
        if (!read_pointer(where.base_pointer, base) || base == 0u) return;
        address = base + where.address;
    } else if (address == 0u) {
        return;
    }

    T value{};
    if (!read(address, value)) return;
    target.value = value;
    target.confidence = where.confidence;
}

} // namespace

PspStateBridge::PspStateBridge(GuestMemoryReader reader, GuestBattleMap map) noexcept
    : reader_(std::move(reader)), map_(std::move(map)) {}

BattleSnapshot PspStateBridge::capture(const std::uint64_t host_tick) const {
    BattleSnapshot snapshot{};
    snapshot.host_tick = host_tick;

    const auto read_f32 = [this](std::uint32_t address, float& out) {
        return reader_.read_f32 && reader_.read_f32(address, out);
    };
    const auto read_u32 = [this](std::uint32_t address, std::uint32_t& out) {
        return reader_.read_u32 && reader_.read_u32(address, out);
    };
    // The guest counts animation frames as an INTEGER. Reinterpreting those
    // bits as a float would be silent nonsense, so the value is converted.
    // `+0x5D4` holds the same progress in seconds and stays unmapped: writing
    // seconds into a field named `animation_frame` would read as a bug forever.
    const auto read_u32_as_f32 = [&](std::uint32_t address, float& out) {
        std::uint32_t raw{};
        if (!read_u32(address, raw)) return false;
        out = static_cast<float>(raw);
        return true;
    };
    const auto read_i32 = [&](std::uint32_t address, std::int32_t& out) {
        std::uint32_t raw{};
        if (!read_u32(address, raw)) return false;
        out = static_cast<std::int32_t>(raw);
        return true;
    };

    for (std::size_t index = 0; index < player_count; ++index) {
        const auto& fields = map_.fighters[index];
        auto& fighter = snapshot.fighters[index];

        // Position is three separate reads, so it is only usable when all three
        // are known. A half-known position would place a fighter somewhere real
        // and wrong, which is worse than not drawing it.
        Tracked<float> x{}, y{}, z{};
        read_field(x, fields.position_x, read_f32, read_u32);
        read_field(y, fields.position_y, read_f32, read_u32);
        read_field(z, fields.position_z, read_f32, read_u32);
        if (x.usable() && y.usable() && z.usable()) {
            fighter.position.value = {x.value, y.value, z.value};
            // Two-argument form on purpose: the initializer_list overload of
            // std::min did not instantiate here under MSVC.
            fighter.position.confidence =
                std::min(x.confidence, std::min(y.confidence, z.confidence));
        }

        read_field(fighter.facing, fields.facing, read_f32, read_u32);
        read_field(fighter.action_id, fields.action_id, read_u32, read_u32);
        read_field(fighter.animation_id, fields.animation_id, read_u32, read_u32);
        read_field(fighter.animation_frame, fields.animation_frame, read_u32_as_f32, read_u32);
        read_field(fighter.health, fields.health, read_i32, read_u32);
        if (!fighter.health.usable()) {
            // Nothing in memory holds the real health, so it is computed the way
            // the guest does. The word the HUD draws from exists, but it lives at
            // a heap address nothing points to, and it lags while the bar drains.
            Tracked<std::int32_t> maximum{}, damage{};
            read_field(maximum, fields.max_health, read_i32, read_u32);
            read_field(damage, fields.damage_taken, read_i32, read_u32);
            if (maximum.usable() && damage.usable()) {
                const std::int32_t remaining =
                    maximum.value - health_per_damage_unit * damage.value;
                fighter.health.value = remaining > 0 ? remaining : 0;
                // The two source fields are verified, but the factor of two has
                // only been measured on one character. Do not accidentally
                // promote the derived result beyond that evidence.
                fighter.health.confidence = std::min(
                    Confidence::provisional,
                    std::min(maximum.confidence, damage.confidence));
            }
        }
        read_field(fighter.meter, fields.meter, read_i32, read_u32);
        read_field(fighter.flags, fields.flags, read_u32, read_u32);
    }

    std::uint32_t phase_raw{};
    Tracked<std::uint32_t> phase{};
    read_field(phase, map_.match.phase, read_u32, read_u32);
    if (phase.usable()) {
        phase_raw = phase.value;
        // The guest's own encoding is not established yet, so the raw value is
        // carried through as `unknown` rather than mapped onto a guess.
        snapshot.match.phase.value = MatchPhase::unknown;
        snapshot.match.phase.confidence = Confidence::provisional;
        (void)phase_raw;
    }
    Tracked<std::uint32_t> round{};
    read_field(round, map_.match.round, read_u32, read_u32);
    if (round.usable()) {
        snapshot.match.round.value = static_cast<std::uint8_t>(round.value);
        snapshot.match.round.confidence = round.confidence;
    }
    read_field(snapshot.match.timer, map_.match.timer, read_i32, read_u32);

    // The camera the game itself computed. Three separate reads, usable only
    // when all three land -- a half-known camera would frame the fight somewhere
    // real and wrong.
    Tracked<float> cx{}, cy{}, cz{};
    read_field(cx, map_.camera.position_x, read_f32, read_u32);
    read_field(cy, map_.camera.position_y, read_f32, read_u32);
    read_field(cz, map_.camera.position_z, read_f32, read_u32);
    if (cx.usable() && cy.usable() && cz.usable()) {
        // Preserve the PSP values exactly as the guest wrote them. Coordinate
        // conversion belongs to the future PSP graphics backend and must be
        // derived from PSP render commands, not inherited from the discarded
        // renderer's conventions.
        snapshot.camera.position.value = {cx.value, cy.value, cz.value};
        snapshot.camera.position.confidence =
            std::min(cx.confidence, std::min(cy.confidence, cz.confidence));
    }

    return snapshot;
}

} // namespace fuc::port

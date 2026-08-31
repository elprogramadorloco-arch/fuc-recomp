#pragma once

#include "fuc/port/state_bridge.hpp"

namespace fuc::port {

// The measured address map for the user's own PSP build of Fate/unlimited codes.
//
// Measured on 2026-08-28 by read-only observation of PPSSPP running the
// interpreter, with the combat save state loaded. The interpreter matters: the
// JIT overwrites the first word of every block it compiles with its own marker,
// so RAM read under the JIT is not a faithful copy of the program.
//
// The map is keyed by the executable's hash and must never be applied to a
// different binary.
[[nodiscard]] GuestBattleMap fate_psp_battle_map();

// SHA-256 of the executable these addresses were measured against.
inline constexpr const char* fate_psp_executable_sha256 =
    "33e8240519a232be40be503f17a23c2dfd5a361177ffb925de444df95dbe6ba7";

// The static table the game keeps its two fighter objects in.
//
// This is the anchor that makes the rest usable. The fighter objects themselves
// are heap allocations whose addresses only repeat because every session starts
// from the same save state; these two words live in the executable image and do
// not move. Entries are 8 bytes apart, the pointer followed by a second word
// that reads 1 for the first fighter and 0 for the second.
inline constexpr std::uint32_t fate_psp_fighter_pointer_p1 = 0x089F4BFCu;
inline constexpr std::uint32_t fate_psp_fighter_pointer_p2 = 0x089F4C04u;

// Field offsets inside a fighter object.
//
//   +0x3C  pointer to the fighter's transform object
//   +0x460 position, three floats, the value gameplay works in
//
// The transform object carries its own copy of the position at +0x110, written
// by `swc1 f0, 0x110(s0)` at 0x088762DC inside sub_08876244 -- a routine shared
// by thirteen call sites, so the instruction identifies the field but not the
// fighter. The port reads the gameplay field instead: the transform copy, and
// the scene-graph matrices further downstream, describe what was last drawn.
inline constexpr std::uint32_t fate_psp_fighter_transform_pointer_offset = 0x3Cu;
inline constexpr std::uint32_t fate_psp_fighter_position_offset = 0x460u;
inline constexpr std::uint32_t fate_psp_transform_position_offset = 0x110u;

// The fighter's current action, as an opaque identifier. Measured by driving one
// fighter through idle, walking, a light attack, a heavy attack and a jump while
// watching the whole object: 144 for the light attack and 146 for the heavy on
// the attacker, and the receiving fighter's own field moves in step. The port
// does not interpret the numbers.
inline constexpr std::uint32_t fate_psp_fighter_action_offset = 0x550u;

// The two numbers real health is made of. Both live in the fighter object, so
// both are reachable through the static pointer.
//
//   +0x234 maximum health, and it differs per character: 10500 against 10000 in
//          the pair measured
//   +0x23C cumulative damage taken
//
// The bridge derives health from them. The word the HUD draws from was found
// too -- 0x0924B380 for the second fighter, with its maximum in the word before
// -- but it is not used: nothing anywhere points at that struct, its address
// shifted by four bytes between two sessions, and it trails the real value while
// the bar drains.
inline constexpr std::uint32_t fate_psp_fighter_max_health_offset = 0x234u;

// Fields established by the same probe but deliberately NOT wired into the map.
//
//   +0x200  the attack identifier this fighter is putting out
//   +0x210  the attack identifier that last connected with it
//   +0x23C  cumulative damage taken, in the guest's own units
//   +0x24C  damage scaling, a float that climbs through a combo
//   +0x5D4  time in the current animation, seconds
//   +0x5D8  the same, counted in frames
//
// The two animation fields are left out because of a unit mismatch worth
// deciding rather than papering over: the contract's `animation_frame` is a
// float, the guest's float counts seconds, and its frame count is an integer.
// Writing seconds into a field named frame would read as a bug forever after.
inline constexpr std::uint32_t fate_psp_fighter_outgoing_attack_offset = 0x200u;
inline constexpr std::uint32_t fate_psp_fighter_incoming_attack_offset = 0x210u;
inline constexpr std::uint32_t fate_psp_fighter_damage_taken_offset = 0x23Cu;
inline constexpr std::uint32_t fate_psp_fighter_damage_scaling_offset = 0x24Cu;
inline constexpr std::uint32_t fate_psp_fighter_animation_seconds_offset = 0x5D4u;
inline constexpr std::uint32_t fate_psp_fighter_animation_frames_offset = 0x5D8u;

// The animating health word the HUD draws from, second fighter, kept only as
// evidence. Its maximum sits in the word before it. Not used by the port: see
// `fate_psp_fighter_max_health_offset`.
inline constexpr std::uint32_t fate_psp_player_two_displayed_health = 0x0924B380u;

// The battle camera's world matrix, as the GAME computes it: 4x4 row-major with
// the translation in the last row, so the position is these three words. Read
// instead of re-derived from the separation law -- the game already works this
// out every frame, and a port replaces the platform layer, not the logic.
inline constexpr std::uint32_t fate_psp_camera_position = 0x08A03FC0u;

// The stage edge, as the guest enforces it: movement is clamped so that the
// position stays within +/- this value.
inline constexpr float fate_psp_stage_bound = 7.0f;

// Address of the horizontal distance between the two fighters, as the guest
// itself stores it.
//
// Not part of `GuestBattleMap`: the bridge derives separation from the two
// fighter positions, and this is the independent witness that proved the
// derivation right -- `position_x[0] - position_x[1]` matched this word to the
// last digit in three separate emulator sessions. Kept so the relation can be
// re-checked after any change.
inline constexpr std::uint32_t fate_psp_separation_address = 0x0969C778u;

} // namespace fuc::port

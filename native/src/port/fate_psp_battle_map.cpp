#include "fuc/port/fate_psp_battle_map.hpp"

namespace fuc::port {
namespace {

// Every field is expressed as an offset from the fighter object, reached through
// the static pointer. Nothing here is a heap address, which is what lets the map
// survive a session that allocates differently.
GuestFieldAddress through(const std::uint32_t pointer, const std::uint32_t offset) noexcept {
    // Verified in PPSSPP and again after a cold AOT boot navigated from Savedata
    // to Stage02. The heap objects moved, while these two static pointer words
    // and every mapped offset continued to resolve coherent fighter state.
    GuestFieldAddress field{};
    field.address = offset;
    field.confidence = Confidence::verified;
    field.base_pointer = pointer;
    return field;
}

GuestBattleMap::FighterFields fighter_through(const std::uint32_t pointer) noexcept {
    GuestBattleMap::FighterFields fields{};
    // The PSP translation row is (depth, height, fighting axis, w=1), not
    // (x, y, z). PSP live traces show the third component changing with
    // horizontal movement while the first remains the shared depth plane.
    //
    // Why the first component is not it: during a live round it holds the SAME
    // value for both fighters (2.17109537 against 2.17109513, apart only in the
    // last bit) for hundreds of frames. Two fighters cannot share a horizontal
    // position; they DO share a depth plane, which is what that component is.
    //
    // The earlier one-shot sample was taken during the round intro, where the
    // first component temporarily looked useful. The live PSP series is the
    // stronger evidence and fixes the map to the third component.
    fields.position_x = through(pointer, fate_psp_fighter_position_offset + 8u);
    fields.position_y = through(pointer, fate_psp_fighter_position_offset + 4u);
    fields.position_z = through(pointer, fate_psp_fighter_position_offset + 0u);
    fields.action_id = through(pointer, fate_psp_fighter_action_offset);
    // Frame counter, mapped from the guest's INTEGER count at +0x5D8 rather
    // than the seconds float at +0x5D4, which is what kept it unmapped. Marked
    // provisional, not verified: the offset comes from the same probe as the
    // rest, but this field has not yet been watched changing across a live
    // battle the way position and action have.
    fields.animation_frame = through(pointer, fate_psp_fighter_animation_frames_offset);
    fields.animation_frame.confidence = Confidence::provisional;
    fields.max_health = through(pointer, fate_psp_fighter_max_health_offset);
    fields.damage_taken = through(pointer, fate_psp_fighter_damage_taken_offset);
    // Facing, animation id and frame, meter and flags are still
    // unresolved. Left at the default so the bridge reports them as such instead
    // of reading a plausible-looking word; see the header for what was measured
    // but deliberately not wired up.
    return fields;
}

} // namespace

GuestBattleMap fate_psp_battle_map() {
    GuestBattleMap map{};
    map.executable_sha256 = fate_psp_executable_sha256;
    map.fighters[0] = fighter_through(fate_psp_fighter_pointer_p1);
    map.fighters[1] = fighter_through(fate_psp_fighter_pointer_p2);
    // A global, not a heap address: no base pointer to follow.
    for (std::size_t axis = 0; axis < 3u; ++axis) {
        GuestFieldAddress field{};
        field.address = fate_psp_camera_position + 4u * static_cast<std::uint32_t>(axis);
        field.confidence = Confidence::verified;
        (axis == 0 ? map.camera.position_x
                   : axis == 1 ? map.camera.position_y
                               : map.camera.position_z) = field;
    }
    return map;
}

} // namespace fuc::port

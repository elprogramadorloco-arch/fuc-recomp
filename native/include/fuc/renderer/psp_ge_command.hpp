#pragma once

#include <cstdint>
#include <span>
#include <array>
#include <vector>

namespace fuc::renderer::psp {

enum class GeOpcode : std::uint8_t {
    nop = 0x00,
    vertex_address = 0x01,
    index_address = 0x02,
    primitive = 0x04,
    jump = 0x08,
    bjump = 0x09,
    call = 0x0a,
    ret = 0x0b,
    end = 0x0c,
    signal = 0x0e,
    finish = 0x0f,
    base = 0x10,
    vertex_type = 0x12,
    offset_address = 0x13,
};

enum class PrimitiveType : std::uint8_t {
    points = 0,
    lines = 1,
    line_strip = 2,
    triangles = 3,
    triangle_strip = 4,
    triangle_fan = 5,
    rectangles = 6,
    keep_previous = 7,
};

struct GeCommand {
    std::uint32_t raw{};
    GeOpcode opcode{GeOpcode::nop};
    std::uint32_t payload{};
};

struct PrimitiveCommand {
    PrimitiveType type{PrimitiveType::points};
    std::uint16_t vertex_count{};
};

struct VertexType {
    std::uint32_t payload{};
    std::uint8_t texture_coordinate_format{};
    std::uint8_t color_format{};
    std::uint8_t normal_format{};
    std::uint8_t position_format{};
    std::uint8_t weight_format{};
    std::uint8_t index_format{};
    bool through{};
};

struct VertexLayout {
    std::uint32_t stride{};
    std::uint32_t weight_offset{};
    std::uint32_t texture_coordinate_offset{};
    std::uint32_t color_offset{};
    std::uint32_t normal_offset{};
    std::uint32_t position_offset{};
};

struct ThroughVertex {
    std::uint32_t color{};
    float u{};
    float v{};
    float x{};
    float y{};
    float z{};
    float inv_w{1.0f};
    float clip_w{1.0f};
    bool has_texture_coordinates{};
    bool valid{true};
    bool needs_projection{};
};

struct GeTransformState {
    std::array<float, 96> bones{};
    std::array<float, 12> world{};
    std::array<float, 12> view{};
    std::array<float, 16> projection{};
    std::uint32_t world_cursor{};
    std::uint32_t view_cursor{};
    std::uint32_t projection_cursor{};
    std::uint32_t bone_cursor{};

    GeTransformState() noexcept;
    void apply(std::uint8_t opcode, std::uint32_t payload) noexcept;
};

enum class TextureDecodeStatus {
    success,
    invalid_dimensions,
    source_too_small,
    palette_too_small,
    unsupported_texture_format,
    unsupported_clut_format,
    unsupported_swizzle,
};

struct DecodedTexture {
    std::uint32_t width{};
    std::uint32_t height{};
    // PSP 8888 order: R in bits 0..7, G in 8..15, B in 16..23, A in 24..31.
    std::vector<std::uint32_t> pixels{};
};

struct PrimitiveDraw {
    std::uint32_t command_address{};
    std::uint32_t list_address{};
    PrimitiveType type{PrimitiveType::points};
    std::uint16_t vertex_count{};
    std::uint32_t vertex_address{};
    std::uint32_t index_address{};
    std::uint32_t vertex_type{};
    std::uint32_t framebuffer_address{};
    bool texture_enabled{};
    std::uint32_t texture_address{};
    std::uint32_t texture_buffer_width{};
    std::uint32_t texture_width{};
    std::uint32_t texture_height{};
    std::uint8_t texture_format{};
    bool texture_swizzled{};
    bool texture_clamp_u{};
    bool texture_clamp_v{};
    std::uint8_t texture_function{};
    bool texture_use_alpha{};
    bool texture_double_color{};
    bool blend_enabled{};
    std::uint8_t blend_equation{};
    std::uint8_t blend_source_factor{};
    std::uint8_t blend_destination_factor{};
    std::uint32_t blend_fixed_source{};
    std::uint32_t blend_fixed_destination{};
    bool alpha_test_enabled{};
    std::uint8_t alpha_function{};
    std::uint8_t alpha_reference{};
    std::uint8_t alpha_mask{};
    bool clear_mode{};
    bool clear_color{};
    bool clear_alpha{};
    bool clear_depth{};
    bool cull_enabled{};
    bool accept_counter_clockwise{};
    bool depth_test_enabled{};
    bool depth_write_enabled{};
    std::uint8_t depth_function{};
    std::uint32_t clut_address{};
    std::uint8_t clut_format{};
    std::uint8_t clut_shift{};
    std::uint8_t clut_mask{};
    std::uint16_t clut_start{};
    GeTransformState transform{};
    float viewport_scale_x{};
    float viewport_scale_y{};
    float viewport_scale_z{};
    float viewport_center_x{};
    float viewport_center_y{};
    float viewport_center_z{};
    float viewport_offset_x{};
    float viewport_offset_y{};
    float texture_scale_u{1.0f};
    float texture_scale_v{1.0f};
    float texture_offset_u{};
    float texture_offset_v{};
    std::uint32_t material_color{0xFFFFFFFFu};
};

enum class GeConsumeStop {
    segment_exhausted,
    end,
    jump_outside_segment,
    invalid_jump,
    loop_detected,
};

struct GeConsumeResult {
    GeConsumeStop stop{GeConsumeStop::segment_exhausted};
    std::uint32_t words_visited{};
    std::uint32_t next_address{};
    std::vector<PrimitiveDraw> draws{};
};

struct GeCommandState {
    std::array<std::uint32_t, 256> command_memory{};
    std::uint32_t base_word{};
    std::uint32_t offset_address{};
    std::uint32_t vertex_address{};
    std::uint32_t index_address{};
    std::uint32_t vertex_type{};
    GeTransformState transform{};

    [[nodiscard]] constexpr std::uint32_t relative_address(
        const std::uint32_t payload) const {
        const std::uint32_t base_extended =
            ((base_word & 0x000F0000u) << 8u) | (payload & 0x00FFFFFFu);
        return (offset_address + base_extended) & 0x0FFFFFFFu;
    }

    bool apply(const GeCommand command, PrimitiveDraw* draw = nullptr) {
        command_memory[static_cast<std::uint8_t>(command.opcode)] = command.raw;
        transform.apply(static_cast<std::uint8_t>(command.opcode), command.payload);
        switch (command.opcode) {
        case GeOpcode::base:
            base_word = command.raw;
            break;
        case GeOpcode::offset_address:
            offset_address = command.raw << 8u;
            break;
        case GeOpcode::vertex_address:
            vertex_address = relative_address(command.payload);
            break;
        case GeOpcode::index_address:
            index_address = relative_address(command.payload);
            break;
        case GeOpcode::vertex_type:
            vertex_type = command.payload;
            break;
        case GeOpcode::primitive: {
            const PrimitiveCommand primitive{
                static_cast<PrimitiveType>((command.payload >> 16u) & 0x7u),
                static_cast<std::uint16_t>(command.payload & 0xFFFFu),
            };
            if (draw != nullptr) {
                draw->type = primitive.type;
                draw->vertex_count = primitive.vertex_count;
                draw->vertex_address = vertex_address;
                draw->index_address = index_address;
                draw->vertex_type = vertex_type;
                draw->framebuffer_address = 0x04000000u |
                    (command_memory[0x9Cu] & 0x001FFFF0u);
                draw->texture_enabled = texture_enabled();
                draw->texture_address = texture_address_0();
                draw->texture_buffer_width = texture_buffer_width_0();
                draw->texture_width = texture_width_0();
                draw->texture_height = texture_height_0();
                draw->texture_format = texture_format();
                draw->texture_swizzled = texture_swizzled();
                draw->texture_clamp_u = texture_clamp_u();
                draw->texture_clamp_v = texture_clamp_v();
                draw->texture_function = texture_function();
                draw->texture_use_alpha = texture_use_alpha();
                draw->texture_double_color = texture_double_color();
                const std::uint32_t blend = command_memory[0xDFu] & 0x00FFFFFFu;
                draw->blend_enabled = (command_memory[0x21u] & 1u) != 0u;
                draw->blend_equation = static_cast<std::uint8_t>((blend >> 8u) & 7u);
                draw->blend_source_factor = static_cast<std::uint8_t>(blend & 0xFu);
                draw->blend_destination_factor = static_cast<std::uint8_t>((blend >> 4u) & 0xFu);
                draw->blend_fixed_source = (command_memory[0xE0u] & 0x00FFFFFFu) |
                                           0xFF000000u;
                draw->blend_fixed_destination = (command_memory[0xE1u] & 0x00FFFFFFu) |
                                                0xFF000000u;
                const std::uint32_t alpha = command_memory[0xDBu] & 0x00FFFFFFu;
                draw->alpha_test_enabled = (command_memory[0x22u] & 1u) != 0u;
                draw->alpha_function = static_cast<std::uint8_t>(alpha & 7u);
                draw->alpha_reference = static_cast<std::uint8_t>((alpha >> 8u) & 0xFFu);
                draw->alpha_mask = static_cast<std::uint8_t>((alpha >> 16u) & 0xFFu);
                const std::uint32_t clear = command_memory[0xD3u] & 0x00FFFFFFu;
                draw->clear_mode = (clear & 1u) != 0u;
                draw->clear_color = (clear & 0x100u) != 0u;
                draw->clear_alpha = (clear & 0x200u) != 0u;
                draw->clear_depth = (clear & 0x400u) != 0u;
                draw->cull_enabled = (command_memory[0x1Du] & 1u) != 0u;
                draw->accept_counter_clockwise = (command_memory[0x9Bu] & 1u) != 0u;
                draw->depth_test_enabled = (command_memory[0x23u] & 1u) != 0u;
                draw->depth_write_enabled = (command_memory[0xE7u] & 1u) == 0u;
                draw->depth_function = static_cast<std::uint8_t>(command_memory[0xDEu] & 7u);
                draw->clut_address = clut_address_value();
                draw->clut_format = clut_format();
                draw->clut_shift = clut_shift();
                draw->clut_mask = clut_mask();
                draw->clut_start = clut_start();
                draw->transform = transform;
                draw->viewport_scale_x = float24(0x42u);
                draw->viewport_scale_y = float24(0x43u);
                draw->viewport_scale_z = float24(0x44u);
                draw->viewport_center_x = float24(0x45u);
                draw->viewport_center_y = float24(0x46u);
                draw->viewport_center_z = float24(0x47u);
                draw->viewport_offset_x = static_cast<float>(command_memory[0x4Cu] & 0xFFFFu) / 16.0f;
                draw->viewport_offset_y = static_cast<float>(command_memory[0x4Du] & 0xFFFFu) / 16.0f;
                draw->texture_scale_u = float24(0x48u);
                draw->texture_scale_v = float24(0x49u);
                draw->texture_offset_u = float24(0x4Au);
                draw->texture_offset_v = float24(0x4Bu);
                draw->material_color = (command_memory[0x55u] & 0x00FFFFFFu) |
                    ((command_memory[0x58u] & 0xFFu) << 24u);
            }
            advance_vertices(primitive.vertex_count);
            return true;
        }
        default:
            break;
        }
        return false;
    }

    [[nodiscard]] float float24(std::uint8_t opcode) const noexcept;
    void advance_vertices(std::uint16_t count) noexcept;

    [[nodiscard]] constexpr bool texture_enabled() const {
        return (command_memory[0x1Eu] & 1u) != 0u;
    }

    [[nodiscard]] constexpr std::uint32_t texture_address_0() const {
        const std::uint32_t address =
            (command_memory[0xA0u] & 0x00FFFFF0u) |
            ((command_memory[0xA8u] << 8u) & 0x0F000000u);
        // GE accepts VRAM as a 2 MiB offset. Address zero is therefore the
        // first EDRAM texel, not a null host pointer.
        return address < 0x00200000u ? 0x04000000u | address : address;
    }

    [[nodiscard]] constexpr std::uint32_t texture_width_0() const {
        return 1u << (command_memory[0xB8u] & 0xFu);
    }

    [[nodiscard]] constexpr std::uint32_t texture_buffer_width_0() const {
        const std::uint32_t width = command_memory[0xA8u] & 0x7FFu;
        return width == 0u ? 1u : width;
    }

    [[nodiscard]] constexpr std::uint32_t texture_height_0() const {
        return 1u << ((command_memory[0xB8u] >> 8u) & 0xFu);
    }

    [[nodiscard]] constexpr std::uint8_t texture_format() const {
        return static_cast<std::uint8_t>(command_memory[0xC3u] & 0xFu);
    }

    [[nodiscard]] constexpr bool texture_swizzled() const {
        return (command_memory[0xC2u] & 1u) != 0u;
    }

    [[nodiscard]] constexpr bool texture_clamp_u() const {
        return (command_memory[0xC7u] & 1u) != 0u;
    }

    [[nodiscard]] constexpr bool texture_clamp_v() const {
        return (command_memory[0xC7u] & 0x100u) != 0u;
    }

    [[nodiscard]] constexpr std::uint8_t texture_function() const {
        return static_cast<std::uint8_t>(command_memory[0xC9u] & 0x7u);
    }

    [[nodiscard]] constexpr bool texture_use_alpha() const {
        return (command_memory[0xC9u] & 0x100u) != 0u;
    }

    [[nodiscard]] constexpr bool texture_double_color() const {
        return (command_memory[0xC9u] & 0x10000u) != 0u;
    }

    [[nodiscard]] constexpr std::uint32_t clut_address_value() const {
        return (command_memory[0xB0u] & 0x00FFFFF0u) |
               ((command_memory[0xB1u] << 8u) & 0x0F000000u);
    }

    [[nodiscard]] constexpr std::uint8_t clut_format() const {
        return static_cast<std::uint8_t>(command_memory[0xC5u] & 0x3u);
    }

    [[nodiscard]] constexpr std::uint8_t clut_shift() const {
        return static_cast<std::uint8_t>((command_memory[0xC5u] >> 2u) & 0x1Fu);
    }

    [[nodiscard]] constexpr std::uint8_t clut_mask() const {
        return static_cast<std::uint8_t>((command_memory[0xC5u] >> 8u) & 0xFFu);
    }

    [[nodiscard]] constexpr std::uint16_t clut_start() const {
        return static_cast<std::uint16_t>(((command_memory[0xC5u] >> 16u) & 0x1Fu) << 4u);
    }
};

// Consumes only the command words made visible by one GE stall update. State is
// deliberately retained between calls because Fate grows a display list in
// segments. Control flow is bounded to the supplied segment; an out-of-range
// jump is reported to the caller instead of reading arbitrary guest memory.
class GeListConsumer {
public:
    [[nodiscard]] GeConsumeResult consume_segment(
        std::uint32_t segment_address,
        std::span<const std::uint32_t> words);

    [[nodiscard]] const GeCommandState& state() const noexcept { return state_; }
    [[nodiscard]] bool consume_command(
        std::uint32_t word, PrimitiveDraw* draw = nullptr) {
        return state_.apply(
            {word, static_cast<GeOpcode>(word >> 24u), word & 0x00FFFFFFu},
            draw);
    }
    void reset() noexcept { state_ = {}; }

private:
    GeCommandState state_{};
};

[[nodiscard]] constexpr GeCommand decode_command(const std::uint32_t word) {
    return {word, static_cast<GeOpcode>(word >> 24u), word & 0x00FFFFFFu};
}

[[nodiscard]] constexpr PrimitiveCommand decode_primitive(const GeCommand command) {
    return {
        static_cast<PrimitiveType>((command.payload >> 16u) & 0x7u),
        static_cast<std::uint16_t>(command.payload & 0xFFFFu),
    };
}

[[nodiscard]] constexpr VertexType decode_vertex_type(const GeCommand command) {
    const std::uint32_t payload = command.payload;
    return {
        payload,
        static_cast<std::uint8_t>(payload & 0x3u),
        static_cast<std::uint8_t>((payload >> 2u) & 0x7u),
        static_cast<std::uint8_t>((payload >> 5u) & 0x3u),
        static_cast<std::uint8_t>((payload >> 7u) & 0x3u),
        static_cast<std::uint8_t>((payload >> 9u) & 0x3u),
        static_cast<std::uint8_t>((payload >> 11u) & 0x3u),
        (payload & (1u << 23u)) != 0u,
    };
}

[[nodiscard]] VertexLayout vertex_layout(VertexType type);
[[nodiscard]] bool decode_through_vertex(VertexType type,
                                         std::span<const std::uint8_t> bytes,
                                         ThroughVertex& vertex,
                                         std::uint32_t material_color = 0xFFFFFFFFu);
[[nodiscard]] bool decode_projected_vertex(VertexType type,
                                           const PrimitiveDraw& draw,
                                           std::span<const std::uint8_t> bytes,
                                           ThroughVertex& vertex);
[[nodiscard]] TextureDecodeStatus decode_texture_8888(
    const PrimitiveDraw& draw,
    std::span<const std::uint8_t> texture_bytes,
    std::span<const std::uint8_t> clut_bytes,
    DecodedTexture& texture);

} // namespace fuc::renderer::psp

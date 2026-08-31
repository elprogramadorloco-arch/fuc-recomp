#include "fuc/renderer/psp_ge_command.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace fuc::renderer::psp {
namespace {

constexpr std::uint32_t align_to(const std::uint32_t value,
                                 const std::uint32_t alignment) {
    return alignment == 0u ? value : (value + alignment - 1u) & ~(alignment - 1u);
}

constexpr std::uint32_t sizes[5][8] = {
    {0u, 1u, 2u, 4u, 0u, 0u, 0u, 0u}, // weights, per component
    {0u, 2u, 4u, 8u, 0u, 0u, 0u, 0u}, // texture coordinates
    {0u, 0u, 0u, 0u, 2u, 2u, 2u, 4u}, // color
    {0u, 3u, 6u, 12u, 0u, 0u, 0u, 0u}, // normal
    {3u, 3u, 6u, 12u, 0u, 0u, 0u, 0u}, // position
};

constexpr std::uint32_t aligns[5][8] = {
    {0u, 1u, 2u, 4u, 0u, 0u, 0u, 0u},
    {0u, 1u, 2u, 4u, 0u, 0u, 0u, 0u},
    {0u, 0u, 0u, 0u, 2u, 2u, 2u, 4u},
    {0u, 1u, 2u, 4u, 0u, 0u, 0u, 0u},
    {1u, 1u, 2u, 4u, 0u, 0u, 0u, 0u},
};

std::uint16_t read_u16(const std::span<const std::uint8_t> bytes,
                       const std::uint32_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1u] << 8u);
}

std::uint32_t read_u32(const std::span<const std::uint8_t> bytes,
                       const std::uint32_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

float read_float(const std::span<const std::uint8_t> bytes,
                 const std::uint32_t offset) {
    return std::bit_cast<float>(read_u32(bytes, offset));
}

std::uint32_t decode_vertex_color(const std::uint8_t format,
                                  const std::span<const std::uint8_t> bytes,
                                  const std::uint32_t offset) noexcept {
    if (format == 7u) return read_u32(bytes, offset);
    const std::uint16_t packed = read_u16(bytes, offset);
    const auto expand4 = [](const std::uint32_t value) {
        return static_cast<std::uint32_t>((value & 15u) * 17u);
    };
    const auto expand5 = [](const std::uint32_t value) {
        return static_cast<std::uint32_t>(((value & 31u) << 3u) |
                                          ((value & 31u) >> 2u));
    };
    const auto expand6 = [](const std::uint32_t value) {
        return static_cast<std::uint32_t>(((value & 63u) << 2u) |
                                          ((value & 63u) >> 4u));
    };
    if (format == 4u) {
        return expand5(packed) | (expand6(packed >> 5u) << 8u) |
               (expand5(packed >> 11u) << 16u) | 0xFF000000u;
    }
    if (format == 5u) {
        return expand5(packed) | (expand5(packed >> 5u) << 8u) |
               (expand5(packed >> 10u) << 16u) |
               ((packed & 0x8000u) != 0u ? 0xFF000000u : 0u);
    }
    if (format == 6u) {
        return expand4(packed) | (expand4(packed >> 4u) << 8u) |
               (expand4(packed >> 8u) << 16u) |
               (expand4(packed >> 12u) << 24u);
    }
    return 0xFFFFFFFFu;
}

float decode_float24(const std::uint32_t payload) noexcept {
    return std::bit_cast<float>((payload & 0x00FFFFFFu) << 8u);
}

struct Vec3 { float x{}, y{}, z{}; };
struct Vec4 { float x{}, y{}, z{}, w{}; };

Vec3 transform_4x3(const std::array<float, 12>& matrix, const Vec3 value) noexcept {
    return {
        matrix[0] * value.x + matrix[3] * value.y + matrix[6] * value.z + matrix[9],
        matrix[1] * value.x + matrix[4] * value.y + matrix[7] * value.z + matrix[10],
        matrix[2] * value.x + matrix[5] * value.y + matrix[8] * value.z + matrix[11],
    };
}

Vec4 transform_4x4(const std::array<float, 16>& matrix, const Vec3 value) noexcept {
    return {
        matrix[0] * value.x + matrix[4] * value.y + matrix[8] * value.z + matrix[12],
        matrix[1] * value.x + matrix[5] * value.y + matrix[9] * value.z + matrix[13],
        matrix[2] * value.x + matrix[6] * value.y + matrix[10] * value.z + matrix[14],
        matrix[3] * value.x + matrix[7] * value.y + matrix[11] * value.z + matrix[15],
    };
}

} // namespace

GeTransformState::GeTransformState() noexcept {
    for (std::size_t bone = 0u; bone < 8u; ++bone) {
        bones[bone * 12u + 0u] = 1.0f;
        bones[bone * 12u + 4u] = 1.0f;
        bones[bone * 12u + 8u] = 1.0f;
    }
    world[0] = world[4] = world[8] = 1.0f;
    view[0] = view[4] = view[8] = 1.0f;
    projection[0] = projection[5] = projection[10] = projection[15] = 1.0f;
}

void GeTransformState::apply(const std::uint8_t opcode,
                             const std::uint32_t payload) noexcept {
    auto write = [payload](auto& matrix, std::uint32_t& cursor) {
        const std::uint32_t index = cursor & 0xFu;
        if (index < matrix.size()) matrix[index] = decode_float24(payload);
        cursor = (index + 1u) & 0xFu;
    };
    switch (opcode) {
    case 0x2Au: bone_cursor = payload & 0x7Fu; break;
    case 0x2Bu: {
        const std::uint32_t index = bone_cursor & 0x7Fu;
        if (index < bones.size()) bones[index] = decode_float24(payload);
        bone_cursor = (index + 1u) & 0x7Fu;
        break;
    }
    case 0x3Au: world_cursor = payload & 0xFu; break;
    case 0x3Bu: write(world, world_cursor); break;
    case 0x3Cu: view_cursor = payload & 0xFu; break;
    case 0x3Du: write(view, view_cursor); break;
    case 0x3Eu: projection_cursor = payload & 0xFu; break;
    case 0x3Fu: write(projection, projection_cursor); break;
    default: break;
    }
}

float GeCommandState::float24(const std::uint8_t opcode) const noexcept {
    return decode_float24(command_memory[opcode]);
}

VertexLayout vertex_layout(const VertexType type) {
    std::uint32_t size = 0u;
    std::uint32_t biggest = 1u;
    VertexLayout layout{};
    const std::uint32_t weight_count = ((type.payload >> 14u) & 0x7u) + 1u;
    const std::uint32_t morph_count = ((type.payload >> 18u) & 0x7u) + 1u;

    if (type.weight_format != 0u) {
        layout.weight_offset = size;
        size += sizes[0][type.weight_format] * weight_count;
        biggest = std::max(biggest, aligns[0][type.weight_format]);
    }
    if (type.texture_coordinate_format != 0u) {
        size = align_to(size, aligns[1][type.texture_coordinate_format]);
        layout.texture_coordinate_offset = size;
        size += sizes[1][type.texture_coordinate_format];
        biggest = std::max(biggest, aligns[1][type.texture_coordinate_format]);
    }
    if (type.color_format != 0u) {
        size = align_to(size, aligns[2][type.color_format]);
        layout.color_offset = size;
        size += sizes[2][type.color_format];
        biggest = std::max(biggest, aligns[2][type.color_format]);
    }
    if (type.normal_format != 0u) {
        size = align_to(size, aligns[3][type.normal_format]);
        layout.normal_offset = size;
        size += sizes[3][type.normal_format];
        biggest = std::max(biggest, aligns[3][type.normal_format]);
    }
    size = align_to(size, aligns[4][type.position_format]);
    layout.position_offset = size;
    size += sizes[4][type.position_format];
    biggest = std::max(biggest, aligns[4][type.position_format]);
    layout.stride = align_to(size, biggest) * morph_count;
    return layout;
}

void GeCommandState::advance_vertices(const std::uint16_t count) noexcept {
    const VertexType type = decode_vertex_type(
        decode_command(0x12000000u | vertex_type));
    if (type.index_format != 0u) {
        const std::uint32_t index_size = type.index_format == 1u ? 1u
            : (type.index_format == 2u ? 2u : 4u);
        index_address += index_size * count;
    } else {
        vertex_address += vertex_layout(type).stride * count;
    }
}

bool decode_through_vertex(const VertexType type,
                           const std::span<const std::uint8_t> bytes,
                           ThroughVertex& vertex,
                           const std::uint32_t material_color) {
    const bool supported = type.through &&
                           (type.texture_coordinate_format == 0u ||
                            type.texture_coordinate_format == 2u) &&
                           (type.color_format == 0u || type.color_format == 7u) &&
                           type.normal_format == 0u &&
                           type.position_format == 2u && type.weight_format == 0u &&
                           ((type.payload >> 18u) & 0x7u) == 0u;
    if (!supported) return false;

    const VertexLayout layout = vertex_layout(type);
    if (bytes.size() < layout.stride) return false;
    vertex.color = type.color_format == 0u
        ? material_color
        : static_cast<std::uint32_t>(bytes[layout.color_offset]) |
              (static_cast<std::uint32_t>(bytes[layout.color_offset + 1u]) << 8u) |
              (static_cast<std::uint32_t>(bytes[layout.color_offset + 2u]) << 16u) |
              (static_cast<std::uint32_t>(bytes[layout.color_offset + 3u]) << 24u);
    vertex.has_texture_coordinates = type.texture_coordinate_format != 0u;
    if (vertex.has_texture_coordinates) {
        vertex.u = static_cast<float>(read_u16(bytes, layout.texture_coordinate_offset));
        vertex.v = static_cast<float>(read_u16(bytes, layout.texture_coordinate_offset + 2u));
    }
    vertex.x = static_cast<std::int16_t>(read_u16(bytes, layout.position_offset));
    vertex.y = static_cast<std::int16_t>(read_u16(bytes, layout.position_offset + 2u));
    vertex.z = read_u16(bytes, layout.position_offset + 4u);
    return true;
}

bool decode_projected_vertex(const VertexType type, const PrimitiveDraw& draw,
                             const std::span<const std::uint8_t> bytes,
                             ThroughVertex& vertex) {
    const bool simple_supported = !type.through &&
        (type.texture_coordinate_format == 0u || type.texture_coordinate_format == 3u) &&
        type.color_format == 7u && type.normal_format == 0u &&
        type.position_format == 3u && type.weight_format == 0u &&
        ((type.payload >> 18u) & 0x7u) == 0u;
    const bool model_supported = !type.through &&
        (type.color_format == 0u ||
         (type.color_format >= 4u && type.color_format <= 7u)) &&
        (type.texture_coordinate_format == 1u || type.texture_coordinate_format == 3u) &&
        (type.normal_format == 1u || type.normal_format == 3u) &&
        (type.position_format == 1u || type.position_format == 3u) &&
        (type.weight_format == 0u || type.weight_format == 1u ||
         type.weight_format == 3u) && ((type.payload >> 18u) & 0x7u) == 0u;
    if (!simple_supported && !model_supported) return false;
    const VertexLayout layout = vertex_layout(type);
    if (bytes.size() < layout.stride) return false;

    vertex.color = type.color_format == 0u
        ? draw.material_color
        : decode_vertex_color(type.color_format, bytes, layout.color_offset);
    vertex.has_texture_coordinates = type.texture_coordinate_format != 0u;
    if (vertex.has_texture_coordinates) {
        const float raw_u = type.texture_coordinate_format == 3u
            ? read_float(bytes, layout.texture_coordinate_offset)
            : bytes[layout.texture_coordinate_offset] / 128.0f;
        const float raw_v = type.texture_coordinate_format == 3u
            ? read_float(bytes, layout.texture_coordinate_offset + 4u)
            : bytes[layout.texture_coordinate_offset + 1u] / 128.0f;
        vertex.u = (raw_u * draw.texture_scale_u + draw.texture_offset_u) *
                   static_cast<float>(draw.texture_width);
        vertex.v = (raw_v * draw.texture_scale_v + draw.texture_offset_v) *
                   static_cast<float>(draw.texture_height);
    }

    Vec3 model{};
    if (type.position_format == 3u) {
        model = {read_float(bytes, layout.position_offset),
                 read_float(bytes, layout.position_offset + 4u),
                 read_float(bytes, layout.position_offset + 8u)};
    } else {
        model = {
            static_cast<float>(static_cast<std::int8_t>(bytes[layout.position_offset])) / 128.0f,
            static_cast<float>(static_cast<std::int8_t>(bytes[layout.position_offset + 1u])) / 128.0f,
            static_cast<float>(static_cast<std::int8_t>(bytes[layout.position_offset + 2u])) / 128.0f,
        };
    }
    if (type.weight_format != 0u) {
        std::array<float, 12> skin{};
        const std::uint32_t weight_count = ((type.payload >> 14u) & 0x7u) + 1u;
        for (std::uint32_t bone = 0u; bone < weight_count; ++bone) {
            const float weight = type.weight_format == 1u
                ? bytes[layout.weight_offset + bone] / 128.0f
                : read_float(bytes, layout.weight_offset + bone * 4u);
            for (std::size_t element = 0u; element < skin.size(); ++element)
                skin[element] += draw.transform.bones[bone * 12u + element] * weight;
        }
        model = transform_4x3(skin, model);
    }
    const Vec3 world = transform_4x3(draw.transform.world, model);
    const Vec3 view = transform_4x3(draw.transform.view, world);
    const Vec4 clip = transform_4x4(draw.transform.projection, view);
    if (!std::isfinite(clip.x) || !std::isfinite(clip.y) ||
        !std::isfinite(clip.z) || !std::isfinite(clip.w)) {
        vertex.valid = false;
        return true;
    }
    vertex.x = clip.x;
    vertex.y = clip.y;
    vertex.z = clip.z;
    vertex.clip_w = clip.w;
    vertex.inv_w = 0.0f;
    vertex.needs_projection = true;
    return true;
}

TextureDecodeStatus decode_texture_8888(
    const PrimitiveDraw& draw,
    const std::span<const std::uint8_t> texture_bytes,
    const std::span<const std::uint8_t> clut_bytes,
    DecodedTexture& texture) {
    texture = {};
    if (draw.texture_width == 0u || draw.texture_height == 0u ||
        draw.texture_buffer_width == 0u) {
        return TextureDecodeStatus::invalid_dimensions;
    }
    if (draw.texture_format > 5u)
        return TextureDecodeStatus::unsupported_texture_format;
    if (draw.texture_format >= 4u && draw.clut_format > 3u)
        return TextureDecodeStatus::unsupported_clut_format;
    const std::uint64_t row_bytes = draw.texture_format == 3u
        ? draw.texture_buffer_width * 4ull
        : (draw.texture_format <= 2u
            ? draw.texture_buffer_width * 2ull
            : (draw.texture_format == 4u
            ? (draw.texture_buffer_width + 1ull) / 2ull
            : draw.texture_buffer_width));
    // PSP swizzle stores 16-byte-wide by 8-row tiles consecutively.  TBW is
    // still the logical row width; incomplete edge tiles occupy a complete
    // 128-byte tile in guest memory.
    const std::uint64_t swizzle_blocks_per_row = (row_bytes + 15ull) / 16ull;
    const std::uint64_t required_source = draw.texture_swizzled
        ? swizzle_blocks_per_row * ((draw.texture_height + 7ull) / 8ull) * 128ull
        : row_bytes * draw.texture_height;
    if (required_source > texture_bytes.size())
        return TextureDecodeStatus::source_too_small;

    const auto source_byte = [&](const std::uint32_t byte_x,
                                 const std::uint32_t y) -> std::uint8_t {
        std::uint64_t offset = static_cast<std::uint64_t>(y) * row_bytes + byte_x;
        if (draw.texture_swizzled) {
            const std::uint64_t block_x = byte_x / 16u;
            const std::uint64_t block_y = y / 8u;
            offset = (block_y * swizzle_blocks_per_row + block_x) * 128ull +
                     static_cast<std::uint64_t>(y & 7u) * 16ull +
                     (byte_x & 15u);
        }
        return texture_bytes[static_cast<std::size_t>(offset)];
    };

    texture.width = draw.texture_width;
    texture.height = draw.texture_height;
    texture.pixels.resize(static_cast<std::size_t>(draw.texture_width) *
                          draw.texture_height);
    const std::uint32_t wrap_mask = 0xFFu;
    for (std::uint32_t y = 0u; y < draw.texture_height; ++y) {
        for (std::uint32_t x = 0u; x < draw.texture_width; ++x) {
            if (draw.texture_format == 3u) {
                const std::uint32_t byte_x = x * 4u;
                texture.pixels[static_cast<std::size_t>(y) * draw.texture_width + x] =
                    static_cast<std::uint32_t>(source_byte(byte_x, y)) |
                    (static_cast<std::uint32_t>(source_byte(byte_x + 1u, y)) << 8u) |
                    (static_cast<std::uint32_t>(source_byte(byte_x + 2u, y)) << 16u) |
                    (static_cast<std::uint32_t>(source_byte(byte_x + 3u, y)) << 24u);
                continue;
            }
            if (draw.texture_format <= 2u) {
                const std::uint32_t byte_x = x * 2u;
                const std::uint16_t packed = static_cast<std::uint16_t>(
                    source_byte(byte_x, y) |
                    (static_cast<std::uint16_t>(source_byte(byte_x + 1u, y)) << 8u));
                const auto expand5 = [](const std::uint32_t value) {
                    return static_cast<std::uint32_t>(((value & 31u) << 3u) |
                                                      ((value & 31u) >> 2u));
                };
                if (draw.texture_format == 0u) { // PSP GU_PSM_5650
                    const auto expand6 = [](const std::uint32_t value) {
                        return static_cast<std::uint32_t>(((value & 63u) << 2u) |
                                                          ((value & 63u) >> 4u));
                    };
                    texture.pixels[static_cast<std::size_t>(y) *
                                       draw.texture_width + x] =
                        expand5(packed) | (expand6(packed >> 5u) << 8u) |
                        (expand5(packed >> 11u) << 16u) | 0xFF000000u;
                } else if (draw.texture_format == 1u) { // PSP GU_PSM_5551
                    texture.pixels[static_cast<std::size_t>(y) *
                                       draw.texture_width + x] =
                        expand5(packed) | (expand5(packed >> 5u) << 8u) |
                        (expand5(packed >> 10u) << 16u) |
                        ((packed & 0x8000u) != 0u ? 0xFF000000u : 0u);
                } else { // PSP GU_PSM_4444
                    const auto expand4 = [](const std::uint32_t value) {
                        return static_cast<std::uint32_t>((value & 15u) * 17u);
                    };
                    texture.pixels[static_cast<std::size_t>(y) *
                                       draw.texture_width + x] =
                        expand4(packed) | (expand4(packed >> 4u) << 8u) |
                        (expand4(packed >> 8u) << 16u) |
                        (expand4(packed >> 12u) << 24u);
                }
                continue;
            }
            const std::uint32_t byte_x = draw.texture_format == 4u ? x >> 1u : x;
            const std::uint8_t packed = source_byte(byte_x, y);
            const std::uint32_t raw_index = draw.texture_format == 4u
                ? ((x & 1u) != 0u ? packed >> 4u : packed & 0xFu)
                : packed;
            const std::uint32_t palette_index =
                (((raw_index >> draw.clut_shift) & draw.clut_mask) |
                 (draw.clut_start & wrap_mask)) & wrap_mask;
            const std::uint32_t palette_entry_size = draw.clut_format == 3u ? 4u : 2u;
            const std::uint32_t palette_offset = palette_index * palette_entry_size;
            if (palette_offset + palette_entry_size > clut_bytes.size()) {
                texture = {};
                return TextureDecodeStatus::palette_too_small;
            }
            std::uint32_t color{};
            if (draw.clut_format == 3u) {
                color = read_u32(clut_bytes, palette_offset);
            } else {
                const std::uint16_t palette_packed = read_u16(clut_bytes, palette_offset);
                const auto expand4 = [](const std::uint32_t value) {
                    return static_cast<std::uint32_t>((value & 15u) * 17u);
                };
                const auto expand5 = [](const std::uint32_t value) {
                    return static_cast<std::uint32_t>(((value & 31u) << 3u) |
                                                      ((value & 31u) >> 2u));
                };
                const auto expand6 = [](const std::uint32_t value) {
                    return static_cast<std::uint32_t>(((value & 63u) << 2u) |
                                                      ((value & 63u) >> 4u));
                };
                if (draw.clut_format == 0u) { // PSP GU_PSM_5650
                    color = expand5(palette_packed) |
                            (expand6(palette_packed >> 5u) << 8u) |
                            (expand5(palette_packed >> 11u) << 16u) | 0xFF000000u;
                } else if (draw.clut_format == 1u) { // PSP GU_PSM_5551
                    color = expand5(palette_packed) |
                            (expand5(palette_packed >> 5u) << 8u) |
                            (expand5(palette_packed >> 10u) << 16u) |
                            ((palette_packed & 0x8000u) != 0u ? 0xFF000000u : 0u);
                } else { // PSP GU_PSM_4444
                    color = expand4(palette_packed) |
                            (expand4(palette_packed >> 4u) << 8u) |
                            (expand4(palette_packed >> 8u) << 16u) |
                            (expand4(palette_packed >> 12u) << 24u);
                }
            }
            texture.pixels[static_cast<std::size_t>(y) * draw.texture_width + x] = color;
        }
    }
    return TextureDecodeStatus::success;
}

GeConsumeResult GeListConsumer::consume_segment(
    const std::uint32_t segment_address,
    const std::span<const std::uint32_t> words) {
    GeConsumeResult result{};
    result.next_address = segment_address;
    std::vector<bool> visited(words.size(), false);
    std::size_t index = 0u;

    while (index < words.size()) {
        if (visited[index]) {
            result.stop = GeConsumeStop::loop_detected;
            result.next_address = segment_address + static_cast<std::uint32_t>(index * 4u);
            return result;
        }
        visited[index] = true;
        ++result.words_visited;

        const GeCommand command = decode_command(words[index]);
        PrimitiveDraw draw{};
        if (consume_command(command.raw, &draw)) result.draws.push_back(draw);

        if (command.opcode == GeOpcode::end) {
            result.stop = GeConsumeStop::end;
            result.next_address = segment_address + static_cast<std::uint32_t>((index + 1u) * 4u);
            return result;
        }
        if (command.opcode == GeOpcode::jump) {
            const std::uint32_t target = state_.relative_address(command.payload);
            if ((target & 3u) != 0u) {
                result.stop = GeConsumeStop::invalid_jump;
                result.next_address = target;
                return result;
            }
            const std::uint64_t segment_end =
                static_cast<std::uint64_t>(segment_address) + words.size() * 4ull;
            if (target < segment_address || static_cast<std::uint64_t>(target) >= segment_end) {
                result.stop = GeConsumeStop::jump_outside_segment;
                result.next_address = target;
                return result;
            }
            index = (target - segment_address) / 4u;
            continue;
        }
        ++index;
    }

    result.stop = GeConsumeStop::segment_exhausted;
    result.next_address = segment_address + static_cast<std::uint32_t>(words.size() * 4u);
    return result;
}

} // namespace fuc::renderer::psp

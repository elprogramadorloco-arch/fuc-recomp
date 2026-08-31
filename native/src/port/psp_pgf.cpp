#include "fuc/port/psp_pgf.hpp"

#include <bit>
#include <limits>
#include <utility>

namespace fuc::port::psp {
namespace {

[[nodiscard]] std::uint16_t read_u16(const std::span<const std::uint8_t> bytes,
                                     const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1u] << 8u);
}

[[nodiscard]] std::uint32_t read_u32(const std::span<const std::uint8_t> bytes,
                                     const std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

[[nodiscard]] std::int32_t read_i32(const std::span<const std::uint8_t> bytes,
                                    const std::size_t offset) noexcept {
    return std::bit_cast<std::int32_t>(read_u32(bytes, offset));
}

[[nodiscard]] bool sane_fixed(const std::int32_t value) noexcept {
    // The PSP registry uses small 26.6 values. This generous bound rejects
    // corrupt offsets interpreted as metrics without excluding firmware fonts.
    return value > -(1 << 24) && value < (1 << 24);
}

[[nodiscard]] bool add_bytes(std::size_t& offset, const std::size_t amount,
                             const std::size_t limit) noexcept {
    if (amount > limit || offset > limit - amount) return false;
    offset += amount;
    return true;
}

[[nodiscard]] bool packed_bytes(const std::uint32_t length,
                                const std::uint32_t bits,
                                std::size_t& result) noexcept {
    if (bits == 0u || bits > 32u) return false;
    const std::uint64_t total_bits = static_cast<std::uint64_t>(length) * bits;
    const std::uint64_t padded_words = (total_bits + 31u) / 32u;
    if (padded_words > std::numeric_limits<std::size_t>::max() / 4u) return false;
    result = static_cast<std::size_t>(padded_words * 4u);
    return true;
}

[[nodiscard]] bool read_bits(const std::span<const std::uint8_t> bytes,
                             const std::size_t bit_offset,
                             const std::uint32_t count,
                             std::uint32_t& result) noexcept {
    if (count == 0u || count > 32u || bit_offset > bytes.size() * 8u ||
        count > bytes.size() * 8u - bit_offset)
        return false;
    std::uint32_t value = 0u;
    for (std::uint32_t bit = 0u; bit < count; ++bit) {
        const std::size_t source = bit_offset + bit;
        value |= static_cast<std::uint32_t>((bytes[source / 8u] >> (source & 7u)) & 1u)
                 << bit;
    }
    result = value;
    return true;
}

[[nodiscard]] std::int32_t signed_seven(const std::uint32_t value) noexcept {
    return value >= 64u ? static_cast<std::int32_t>(value) - 128
                        : static_cast<std::int32_t>(value);
}

} // namespace

bool parse_pgf_header(const std::span<const std::uint8_t> bytes,
                      PgfHeader& header) noexcept {
    if (bytes.size() < pgf_header_size || bytes[4] != 'P' || bytes[5] != 'G' ||
        bytes[6] != 'F' || bytes[7] != '0')
        return false;

    const std::uint16_t header_offset = read_u16(bytes, 0x00u);
    const std::uint16_t header_bytes = read_u16(bytes, 0x02u);
    if (header_offset > bytes.size() || header_bytes < pgf_header_size ||
        static_cast<std::size_t>(header_offset) + header_bytes > bytes.size())
        return false;

    PgfHeader parsed{};
    parsed.revision = read_u32(bytes, 0x08u);
    parsed.version = read_u32(bytes, 0x0Cu);
    parsed.char_map_length = read_u32(bytes, 0x10u);
    parsed.char_pointer_length = read_u32(bytes, 0x14u);
    parsed.char_map_bits = read_u32(bytes, 0x18u);
    parsed.char_pointer_bits = read_u32(bytes, 0x1Cu);
    parsed.bits_per_pixel = bytes[0x22u];
    parsed.horizontal_size_26_6 = read_i32(bytes, 0x24u);
    parsed.vertical_size_26_6 = read_i32(bytes, 0x28u);
    parsed.horizontal_resolution_26_6 = read_i32(bytes, 0x2Cu);
    parsed.vertical_resolution_26_6 = read_i32(bytes, 0x30u);
    parsed.first_glyph = read_u16(bytes, 0xB6u);
    parsed.last_glyph = read_u16(bytes, 0xB8u);
    parsed.max_ascender_26_6 = read_i32(bytes, 0xD4u);
    parsed.max_descender_26_6 = read_i32(bytes, 0xD8u);
    parsed.max_left_x_adjust_26_6 = read_i32(bytes, 0xDCu);
    parsed.max_base_y_adjust_26_6 = read_i32(bytes, 0xE0u);
    parsed.min_center_x_adjust_26_6 = read_i32(bytes, 0xE4u);
    parsed.max_top_y_adjust_26_6 = read_i32(bytes, 0xE8u);
    parsed.max_advance_horizontal_26_6 = read_i32(bytes, 0xECu);
    parsed.max_advance_vertical_26_6 = read_i32(bytes, 0xF0u);
    parsed.max_width_26_6 = read_i32(bytes, 0xF4u);
    parsed.max_height_26_6 = read_i32(bytes, 0xF8u);
    parsed.max_glyph_width = read_u16(bytes, 0xFCu);
    parsed.max_glyph_height = read_u16(bytes, 0xFEu);
    parsed.dimension_table_length = bytes[0x102u];
    parsed.x_adjust_table_length = bytes[0x103u];
    parsed.y_adjust_table_length = bytes[0x104u];
    parsed.advance_table_length = bytes[0x105u];
    parsed.shadow_map_length = read_u32(bytes, 0x16Cu);
    parsed.shadow_map_bits = read_u32(bytes, 0x170u);

    const std::array<std::int32_t, 14> fixed{{
        parsed.horizontal_size_26_6, parsed.vertical_size_26_6,
        parsed.horizontal_resolution_26_6, parsed.vertical_resolution_26_6,
        parsed.max_ascender_26_6, parsed.max_descender_26_6,
        parsed.max_left_x_adjust_26_6, parsed.max_base_y_adjust_26_6,
        parsed.min_center_x_adjust_26_6, parsed.max_top_y_adjust_26_6,
        parsed.max_advance_horizontal_26_6, parsed.max_advance_vertical_26_6,
        parsed.max_width_26_6, parsed.max_height_26_6}};
    if ((parsed.revision != 2u && parsed.revision != 3u) || parsed.version == 0u ||
        parsed.char_map_length == 0u || parsed.char_pointer_length == 0u ||
        parsed.char_map_bits == 0u || parsed.char_map_bits > 32u ||
        parsed.char_pointer_bits == 0u || parsed.char_pointer_bits > 32u ||
        parsed.bits_per_pixel == 0u || parsed.bits_per_pixel > 8u ||
        parsed.first_glyph > parsed.last_glyph || parsed.max_glyph_width == 0u ||
        parsed.max_glyph_height == 0u || parsed.dimension_table_length == 0u ||
        parsed.x_adjust_table_length == 0u || parsed.y_adjust_table_length == 0u ||
        parsed.advance_table_length == 0u)
        return false;
    for (const std::int32_t value : fixed)
        if (!sane_fixed(value)) return false;

    header = parsed;
    return true;
}

PgfFontMetrics pgf_font_metrics(const PgfHeader& header) noexcept {
    PgfFontMetrics metrics{};
    metrics.fixed_26_6 = {{
        header.max_width_26_6, header.max_height_26_6,
        header.max_ascender_26_6, header.max_descender_26_6,
        header.max_left_x_adjust_26_6, header.max_base_y_adjust_26_6,
        header.min_center_x_adjust_26_6, header.max_top_y_adjust_26_6,
        header.max_advance_horizontal_26_6, header.max_advance_vertical_26_6}};
    for (std::size_t index = 0u; index < metrics.floating.size(); ++index)
        metrics.floating[index] = static_cast<float>(metrics.fixed_26_6[index]) / 64.0f;
    metrics.max_glyph_width = header.max_glyph_width;
    metrics.max_glyph_height = header.max_glyph_height;
    metrics.glyph_count = header.char_pointer_length;
    metrics.shadow_map_length = header.shadow_map_length;
    metrics.bits_per_pixel = header.bits_per_pixel;
    return metrics;
}

bool PgfFont::load(const std::span<const std::uint8_t> bytes) {
    PgfHeader parsed{};
    if (!parse_pgf_header(bytes, parsed) || parsed.char_map_length > 0x100000u ||
        parsed.char_pointer_length > 0x100000u || parsed.shadow_map_length > 0x10000u)
        return false;

    std::size_t offset = pgf_header_size;
    std::uint32_t compression_length_1 = 0u;
    std::uint32_t compression_length_2 = 0u;
    if (parsed.revision == 3u) {
        if (!add_bytes(offset, 20u, bytes.size())) return false;
        compression_length_1 = read_u32(bytes, pgf_header_size + 4u) & 0xFFFFu;
        compression_length_2 = read_u32(bytes, pgf_header_size + 12u) & 0xFFFFu;
    }

    const auto read_metric_table = [&](const std::uint8_t length,
                                       std::vector<MetricPair>& table) {
        const std::size_t table_bytes = static_cast<std::size_t>(length) * 8u;
        if (table_bytes > bytes.size() || offset > bytes.size() - table_bytes)
            return false;
        table.clear();
        table.reserve(length);
        for (std::size_t index = 0u; index < length; ++index) {
            table.emplace_back(read_i32(bytes, offset + index * 8u),
                               read_i32(bytes, offset + index * 8u + 4u));
        }
        offset += table_bytes;
        return true;
    };

    std::vector<MetricPair> dimensions;
    std::vector<MetricPair> x_adjust;
    std::vector<MetricPair> y_adjust;
    std::vector<MetricPair> advances;
    if (!read_metric_table(parsed.dimension_table_length, dimensions) ||
        !read_metric_table(parsed.x_adjust_table_length, x_adjust) ||
        !read_metric_table(parsed.y_adjust_table_length, y_adjust) ||
        !read_metric_table(parsed.advance_table_length, advances))
        return false;

    std::size_t shadow_bytes = 0u;
    std::size_t char_map_bytes = 0u;
    std::size_t pointer_bytes = 0u;
    if (!packed_bytes(parsed.shadow_map_length, parsed.shadow_map_bits, shadow_bytes) ||
        !packed_bytes(parsed.char_map_length, parsed.char_map_bits, char_map_bytes) ||
        !packed_bytes(parsed.char_pointer_length, parsed.char_pointer_bits, pointer_bytes))
        return false;
    if (!add_bytes(offset, shadow_bytes, bytes.size())) return false;
    const std::uint64_t compression_entries =
        static_cast<std::uint64_t>(compression_length_1) + compression_length_2;
    if (compression_entries > std::numeric_limits<std::size_t>::max() / 4u ||
        !add_bytes(offset, static_cast<std::size_t>(compression_entries * 4u), bytes.size()))
        return false;

    const std::size_t char_map_offset = offset;
    if (!add_bytes(offset, char_map_bytes, bytes.size())) return false;
    const std::size_t pointer_offset = offset;
    if (!add_bytes(offset, pointer_bytes, bytes.size()) || offset >= bytes.size()) return false;

    std::vector<std::uint32_t> char_map(parsed.char_map_length);
    const auto char_map_span = bytes.subspan(char_map_offset, char_map_bytes);
    for (std::size_t index = 0u; index < char_map.size(); ++index) {
        if (!read_bits(char_map_span, index * parsed.char_map_bits,
                       parsed.char_map_bits, char_map[index]))
            return false;
        if (char_map[index] >= parsed.char_pointer_length)
            char_map[index] = std::numeric_limits<std::uint32_t>::max();
    }

    std::vector<std::uint32_t> pointers(parsed.char_pointer_length);
    const auto pointer_span = bytes.subspan(pointer_offset, pointer_bytes);
    for (std::size_t index = 0u; index < pointers.size(); ++index)
        if (!read_bits(pointer_span, index * parsed.char_pointer_bits,
                       parsed.char_pointer_bits, pointers[index]))
            return false;

    header_ = parsed;
    bytes_.assign(bytes.begin(), bytes.end());
    dimensions_ = std::move(dimensions);
    x_adjust_ = std::move(x_adjust);
    y_adjust_ = std::move(y_adjust);
    advances_ = std::move(advances);
    char_map_ = std::move(char_map);
    char_pointers_ = std::move(pointers);
    font_data_offset_ = offset;
    return true;
}

bool PgfFont::decode_glyph(const std::uint32_t index, Glyph& glyph) const noexcept {
    if (index >= char_pointers_.size() || font_data_offset_ >= bytes_.size()) return false;
    const auto data = std::span<const std::uint8_t>(bytes_).subspan(font_data_offset_);
    const std::uint64_t start = static_cast<std::uint64_t>(char_pointers_[index]) * 32u;
    if (start > std::numeric_limits<std::size_t>::max()) return false;
    std::size_t cursor = static_cast<std::size_t>(start);
    std::uint32_t value = 0u;
    const auto take = [&](const std::uint32_t count, std::uint32_t& out) {
        if (!read_bits(data, cursor, count, out)) return false;
        cursor += count;
        return true;
    };
    if (!take(14u, value)) return false;
    Glyph decoded{};
    if (!take(7u, decoded.width) || !take(7u, decoded.height) || !take(7u, value))
        return false;
    decoded.left = signed_seven(value);
    if (!take(7u, value)) return false;
    decoded.top = signed_seven(value);
    if (!take(6u, value)) return false;
    decoded.flags = static_cast<std::uint8_t>(value);
    std::uint32_t shadow = 0u;
    if (!take(2u, value)) return false;
    shadow = value << 5u;
    if (!take(2u, value)) return false;
    shadow |= value << 3u;
    if (!take(3u, value)) return false;
    decoded.shadow_flags = static_cast<std::int16_t>(shadow | value);
    if (!take(9u, value)) return false;
    decoded.shadow_id = static_cast<std::int16_t>(value);

    const auto metric = [&](const std::uint8_t indexed_flag,
                            const std::vector<MetricPair>& table,
                            MetricPair& output) {
        std::uint32_t first = 0u;
        std::uint32_t second = 0u;
        if ((decoded.flags & indexed_flag) != 0u) {
            if (!take(8u, first) || first >= table.size()) return false;
            output = table[first];
            return true;
        }
        if (!take(32u, first) || !take(32u, second)) return false;
        output = {std::bit_cast<std::int32_t>(first), std::bit_cast<std::int32_t>(second)};
        return true;
    };
    if (!metric(0x04u, dimensions_, decoded.dimensions) ||
        !metric(0x08u, x_adjust_, decoded.x_adjust) ||
        !metric(0x10u, y_adjust_, decoded.y_adjust) ||
        !metric(0x20u, advances_, decoded.advance))
        return false;
    // The PSP decoder begins the nibble stream at the containing byte.
    decoded.bitmap_bit_offset = cursor & ~std::size_t{7u};
    glyph = decoded;
    return true;
}

bool PgfFont::glyph_for(const std::uint16_t character, Glyph& glyph) const noexcept {
    if (character < header_.first_glyph) return false;
    std::uint32_t index = static_cast<std::uint32_t>(character - header_.first_glyph);
    if (index < char_map_.size()) index = char_map_[index];
    return decode_glyph(index, glyph);
}

bool PgfFont::character_info(const std::uint16_t character,
                             const std::uint16_t alternate,
                             PgfCharInfo& info) const noexcept {
    info = {};
    Glyph glyph{};
    if (!glyph_for(character, glyph)) {
        if (character < header_.first_glyph || !glyph_for(alternate, glyph)) return false;
    }
    info.bitmap_width = glyph.width;
    info.bitmap_height = glyph.height;
    info.bitmap_left = glyph.left;
    info.bitmap_top = glyph.top;
    info.width_26_6 = glyph.dimensions.first;
    info.height_26_6 = glyph.dimensions.second;
    info.ascender_26_6 = glyph.y_adjust.first;
    info.descender_26_6 = glyph.y_adjust.first - glyph.dimensions.second;
    info.bearing_h_x_26_6 = glyph.x_adjust.first;
    info.bearing_h_y_26_6 = glyph.y_adjust.first;
    info.bearing_v_x_26_6 = glyph.x_adjust.second;
    info.bearing_v_y_26_6 = glyph.y_adjust.second;
    info.advance_h_26_6 = glyph.advance.first;
    info.advance_v_26_6 = glyph.advance.second;
    info.shadow_flags = glyph.shadow_flags;
    info.shadow_id = glyph.shadow_id;
    return true;
}

bool PgfFont::glyph_bitmap(const std::uint16_t character,
                           const std::uint16_t alternate,
                           PgfGlyphBitmap& bitmap) const {
    bitmap = {};
    Glyph glyph{};
    if (!glyph_for(character, glyph)) {
        if (character < header_.first_glyph || !glyph_for(alternate, glyph)) return false;
    }
    if (glyph.width == 0u || glyph.height == 0u || glyph.width > 0x1000u ||
        glyph.height > 0x1000u || (glyph.flags & 0x03u) == 0u ||
        (glyph.flags & 0x03u) == 0x03u)
        return false;
    const std::size_t count = static_cast<std::size_t>(glyph.width) * glyph.height;
    std::vector<std::uint8_t> encoded(count);
    const auto data = std::span<const std::uint8_t>(bytes_).subspan(font_data_offset_);
    std::size_t cursor = glyph.bitmap_bit_offset;
    std::size_t output = 0u;
    while (output < count) {
        std::uint32_t nibble = 0u;
        if (!read_bits(data, cursor, 4u, nibble)) return false;
        cursor += 4u;
        const std::uint32_t run = nibble < 8u ? nibble + 1u : 16u - nibble;
        std::uint32_t repeated = 0u;
        if (nibble < 8u) {
            if (!read_bits(data, cursor, 4u, repeated)) return false;
            cursor += 4u;
        }
        for (std::uint32_t index = 0u; index < run && output < count; ++index) {
            std::uint32_t value = repeated;
            if (nibble >= 8u) {
                if (!read_bits(data, cursor, 4u, value)) return false;
                cursor += 4u;
            }
            encoded[output++] = static_cast<std::uint8_t>(value);
        }
    }
    bitmap.width = glyph.width;
    bitmap.height = glyph.height;
    bitmap.pixels.resize(count);
    const bool horizontal = (glyph.flags & 0x03u) == 0x01u;
    for (std::uint32_t y = 0u; y < glyph.height; ++y)
        for (std::uint32_t x = 0u; x < glyph.width; ++x) {
            const std::size_t source = horizontal
                ? static_cast<std::size_t>(y) * glyph.width + x
                : static_cast<std::size_t>(x) * glyph.height + y;
            bitmap.pixels[static_cast<std::size_t>(y) * glyph.width + x] =
                encoded[source];
        }
    return true;
}

} // namespace fuc::port::psp

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace fuc::port::psp {

// Fixed PGF0 header fields used by the PSP libfont ABI. Values ending in
// `26_6` are signed fixed-point quantities with six fractional bits.
struct PgfHeader {
    std::uint32_t revision{};
    std::uint32_t version{};
    std::uint32_t char_map_length{};
    std::uint32_t char_pointer_length{};
    std::uint32_t char_map_bits{};
    std::uint32_t char_pointer_bits{};
    std::uint8_t bits_per_pixel{};
    std::int32_t horizontal_size_26_6{};
    std::int32_t vertical_size_26_6{};
    std::int32_t horizontal_resolution_26_6{};
    std::int32_t vertical_resolution_26_6{};
    std::uint16_t first_glyph{};
    std::uint16_t last_glyph{};
    std::int32_t max_ascender_26_6{};
    std::int32_t max_descender_26_6{};
    std::int32_t max_left_x_adjust_26_6{};
    std::int32_t max_base_y_adjust_26_6{};
    std::int32_t min_center_x_adjust_26_6{};
    std::int32_t max_top_y_adjust_26_6{};
    std::int32_t max_advance_horizontal_26_6{};
    std::int32_t max_advance_vertical_26_6{};
    std::int32_t max_width_26_6{};
    std::int32_t max_height_26_6{};
    std::uint16_t max_glyph_width{};
    std::uint16_t max_glyph_height{};
    std::uint8_t dimension_table_length{};
    std::uint8_t x_adjust_table_length{};
    std::uint8_t y_adjust_table_length{};
    std::uint8_t advance_table_length{};
    std::uint32_t shadow_map_length{};
    std::uint32_t shadow_map_bits{};
};

struct PgfFontMetrics {
    std::array<std::int32_t, 10> fixed_26_6{};
    std::array<float, 10> floating{};
    std::uint16_t max_glyph_width{};
    std::uint16_t max_glyph_height{};
    std::uint32_t glyph_count{};
    std::uint32_t shadow_map_length{};
    std::uint8_t bits_per_pixel{};
};

// Binary layout returned by sceFontGetCharInfo, expressed as host values.
// Signed bitmap offsets are kept as signed values even though the PSP ABI
// stores the four leading fields in 32-bit words.
struct PgfCharInfo {
    std::uint32_t bitmap_width{};
    std::uint32_t bitmap_height{};
    std::int32_t bitmap_left{};
    std::int32_t bitmap_top{};
    std::int32_t width_26_6{};
    std::int32_t height_26_6{};
    std::int32_t ascender_26_6{};
    std::int32_t descender_26_6{};
    std::int32_t bearing_h_x_26_6{};
    std::int32_t bearing_h_y_26_6{};
    std::int32_t bearing_v_x_26_6{};
    std::int32_t bearing_v_y_26_6{};
    std::int32_t advance_h_26_6{};
    std::int32_t advance_v_26_6{};
    std::int16_t shadow_flags{};
    std::int16_t shadow_id{};
};

struct PgfGlyphBitmap {
    std::uint32_t width{};
    std::uint32_t height{};
    // Normalized row-major 4-bit coverage values (0..15).
    std::vector<std::uint8_t> pixels{};
};

class PgfFont {
public:
    [[nodiscard]] bool load(std::span<const std::uint8_t> bytes);
    [[nodiscard]] const PgfHeader& header() const noexcept { return header_; }
    [[nodiscard]] bool character_info(std::uint16_t character,
                                      std::uint16_t alternate,
                                      PgfCharInfo& info) const noexcept;
    [[nodiscard]] bool glyph_bitmap(std::uint16_t character,
                                    std::uint16_t alternate,
                                    PgfGlyphBitmap& bitmap) const;

private:
    using MetricPair = std::pair<std::int32_t, std::int32_t>;

    struct Glyph {
        std::uint32_t width{};
        std::uint32_t height{};
        std::int32_t left{};
        std::int32_t top{};
        std::uint8_t flags{};
        std::int16_t shadow_flags{};
        std::int16_t shadow_id{};
        MetricPair dimensions{};
        MetricPair x_adjust{};
        MetricPair y_adjust{};
        MetricPair advance{};
        std::size_t bitmap_bit_offset{};
    };

    [[nodiscard]] bool glyph_for(std::uint16_t character, Glyph& glyph) const noexcept;
    [[nodiscard]] bool decode_glyph(std::uint32_t index, Glyph& glyph) const noexcept;

    PgfHeader header_{};
    std::vector<std::uint8_t> bytes_{};
    std::vector<MetricPair> dimensions_{};
    std::vector<MetricPair> x_adjust_{};
    std::vector<MetricPair> y_adjust_{};
    std::vector<MetricPair> advances_{};
    std::vector<std::uint32_t> char_map_{};
    std::vector<std::uint32_t> char_pointers_{};
    std::size_t font_data_offset_{};
};

inline constexpr std::size_t pgf_header_size = 0x188u;

[[nodiscard]] bool parse_pgf_header(std::span<const std::uint8_t> bytes,
                                    PgfHeader& header) noexcept;
[[nodiscard]] PgfFontMetrics pgf_font_metrics(const PgfHeader& header) noexcept;

} // namespace fuc::port::psp

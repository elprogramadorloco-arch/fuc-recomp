#include "fuc/renderer/psp_ge_surface.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace fuc::renderer::psp {
namespace {

float edge(const ThroughVertex& a, const ThroughVertex& b, float x, float y) {
    return (x - static_cast<float>(a.x)) *
               (static_cast<float>(b.y) - static_cast<float>(a.y)) -
           (y - static_cast<float>(a.y)) *
               (static_cast<float>(b.x) - static_cast<float>(a.x));
}

std::uint8_t channel(std::uint32_t color, unsigned shift) {
    return static_cast<std::uint8_t>(color >> shift);
}

std::uint32_t interpolate_color(const ThroughVertex& a, const ThroughVertex& b,
                                const ThroughVertex& c, float wa, float wb,
                                float wc) {
    std::uint32_t result = 0u;
    for (unsigned shift : {0u, 8u, 16u, 24u}) {
        const float value = channel(a.color, shift) * wa +
                            channel(b.color, shift) * wb +
                            channel(c.color, shift) * wc;
        result |= static_cast<std::uint32_t>(std::clamp(std::lround(value), 0l, 255l))
                  << shift;
    }
    return result;
}

std::uint32_t modulate(std::uint32_t vertex, std::uint32_t texel,
                       bool use_alpha, bool double_color) {
    std::uint32_t result = 0u;
    const unsigned factor = double_color ? 2u : 1u;
    for (unsigned shift : {0u, 8u, 16u}) {
        const unsigned value = channel(vertex, shift) * channel(texel, shift) * factor;
        result |= std::min(255u, (value + 127u) / 255u) << shift;
    }
    const std::uint8_t alpha = use_alpha
        ? static_cast<std::uint8_t>((channel(vertex, 24u) * channel(texel, 24u) + 127u) / 255u)
        : channel(vertex, 24u);
    return result | (static_cast<std::uint32_t>(alpha) << 24u);
}

bool compare_alpha(const std::uint8_t function, const std::uint8_t value,
                   const std::uint8_t reference) noexcept {
    switch (function & 7u) {
    case 0u: return false;
    case 1u: return true;
    case 2u: return value == reference;
    case 3u: return value != reference;
    case 4u: return value < reference;
    case 5u: return value <= reference;
    case 6u: return value > reference;
    case 7u: return value >= reference;
    }
    return true;
}

std::array<float, 4> color_float(const std::uint32_t color) noexcept {
    return {channel(color, 0u) / 255.0f, channel(color, 8u) / 255.0f,
            channel(color, 16u) / 255.0f, channel(color, 24u) / 255.0f};
}

std::array<float, 4> blend_factor(const std::uint8_t factor,
                                  const bool source_factor,
                                  const std::array<float, 4>& source,
                                  const std::array<float, 4>& destination,
                                  const std::uint32_t fixed) noexcept {
    switch (factor) {
    case 0u: return source_factor ? destination : source;
    case 1u: return source_factor
        ? std::array<float,4>{1-destination[0],1-destination[1],1-destination[2],1-destination[3]}
        : std::array<float,4>{1-source[0],1-source[1],1-source[2],1-source[3]};
    case 2u: return {source[3], source[3], source[3], source[3]};
    case 3u: return {1-source[3], 1-source[3], 1-source[3], 1-source[3]};
    case 4u: return {destination[3], destination[3], destination[3], destination[3]};
    case 5u: return {1-destination[3], 1-destination[3], 1-destination[3], 1-destination[3]};
    case 6u: { const float a = std::min(1.0f, 2*source[3]); return {a,a,a,a}; }
    case 7u: { const float a = std::min(1.0f, 2*(1-source[3])); return {a,a,a,a}; }
    case 8u: { const float a = std::min(1.0f, 2*destination[3]); return {a,a,a,a}; }
    case 9u: { const float a = std::min(1.0f, 2*(1-destination[3])); return {a,a,a,a}; }
    case 10u: return color_float(fixed);
    default: return {1,1,1,1};
    }
}

float clip_distance(const ThroughVertex& vertex, const std::uint32_t plane) noexcept {
    switch (plane) {
    case 0u: return vertex.x + vertex.clip_w;
    case 1u: return vertex.clip_w - vertex.x;
    case 2u: return vertex.y + vertex.clip_w;
    case 3u: return vertex.clip_w - vertex.y;
    case 4u: return vertex.z + vertex.clip_w;
    default: return vertex.clip_w - vertex.z;
    }
}

ThroughVertex interpolate_clip_vertex(const ThroughVertex& a,
                                      const ThroughVertex& b,
                                      const float t) noexcept {
    ThroughVertex out{};
    const auto lerp = [t](const float x, const float y) { return x + (y - x) * t; };
    out.u = lerp(a.u, b.u);
    out.v = lerp(a.v, b.v);
    out.x = lerp(a.x, b.x);
    out.y = lerp(a.y, b.y);
    out.z = lerp(a.z, b.z);
    out.clip_w = lerp(a.clip_w, b.clip_w);
    out.has_texture_coordinates = a.has_texture_coordinates || b.has_texture_coordinates;
    out.needs_projection = true;
    out.valid = a.valid && b.valid;
    for (const unsigned shift : {0u, 8u, 16u, 24u}) {
        const auto value = static_cast<std::uint32_t>(std::clamp(
            std::lround(lerp(static_cast<float>(channel(a.color, shift)),
                             static_cast<float>(channel(b.color, shift)))),
            0l, 255l));
        out.color |= value << shift;
    }
    return out;
}

bool project_vertex(ThroughVertex& vertex, const PrimitiveDraw& draw) noexcept {
    if (!vertex.needs_projection) return vertex.valid;
    if (!vertex.valid || !std::isfinite(vertex.clip_w) ||
        std::fabs(vertex.clip_w) < 1.0e-12f)
        return false;
    const float inv_w = 1.0f / vertex.clip_w;
    vertex.x = vertex.x * inv_w * draw.viewport_scale_x + draw.viewport_center_x -
               draw.viewport_offset_x;
    vertex.y = vertex.y * inv_w * draw.viewport_scale_y + draw.viewport_center_y -
               draw.viewport_offset_y;
    vertex.z = vertex.z * inv_w * draw.viewport_scale_z + draw.viewport_center_z;
    vertex.inv_w = inv_w;
    vertex.needs_projection = false;
    vertex.valid = std::isfinite(vertex.x) && std::isfinite(vertex.y) &&
                   std::isfinite(vertex.z);
    return vertex.valid;
}

const std::array<std::uint32_t, 2>& software_pixel_test() noexcept {
    static const auto result = [] {
        std::array<std::uint32_t, 2> parsed{0xFFFFFFFFu, 0xFFFFFFFFu};
        std::string value;
#if defined(_MSC_VER)
        char* environment = nullptr;
        std::size_t length = 0u;
        if (_dupenv_s(&environment, &length,
                      "PSPRECOMP_GE_SOFTWARE_PIXEL_TEST") == 0 &&
            environment != nullptr) {
            value.assign(environment);
            std::free(environment);
        }
#else
        if (const char* environment =
                std::getenv("PSPRECOMP_GE_SOFTWARE_PIXEL_TEST"))
            value.assign(environment);
#endif
        if (value.empty()) return parsed;
        char* end = nullptr;
        const unsigned long px = std::strtoul(value.c_str(), &end, 0);
        if (end == value.c_str() || *end != ':') return parsed;
        char* y_end = nullptr;
        const unsigned long py = std::strtoul(end + 1, &y_end, 0);
        if (y_end == end + 1 || *y_end != '\0' || px >= 480u || py >= 272u)
            return parsed;
        parsed[0] = static_cast<std::uint32_t>(px);
        parsed[1] = static_cast<std::uint32_t>(py);
        return parsed;
    }();
    return result;
}

} // namespace

GeSoftwareSurface::GeSoftwareSurface(const std::uint32_t width,
                                     const std::uint32_t height)
    : width_(width), height_(height),
      pixels_(static_cast<std::size_t>(width) * height),
      depth_(static_cast<std::size_t>(width) * height, 0u) {}

void GeSoftwareSurface::clear(const std::uint32_t color) noexcept {
    std::fill(pixels_.begin(), pixels_.end(), color);
    std::fill(depth_.begin(), depth_.end(), std::uint16_t{0u});
}

SurfaceDrawStatus GeSoftwareSurface::draw(
    const PrimitiveDraw& descriptor,
    const std::span<const std::uint8_t> vertex_bytes,
    const std::span<const std::uint8_t> texture_bytes,
    const std::span<const std::uint8_t> clut_bytes) {
    const VertexType type = decode_vertex_type(
        decode_command(0x12000000u | descriptor.vertex_type));
    const VertexLayout layout = vertex_layout(type);
    if (layout.stride == 0u || descriptor.vertex_count == 0u ||
        vertex_bytes.size() < static_cast<std::size_t>(layout.stride) * descriptor.vertex_count)
        return SurfaceDrawStatus::invalid_vertex_data;

    std::vector<ThroughVertex> vertices(descriptor.vertex_count);
    for (std::uint32_t i = 0u; i < descriptor.vertex_count; ++i) {
        const auto bytes = vertex_bytes.subspan(i * layout.stride, layout.stride);
        if (!decode_through_vertex(type, bytes, vertices[i],
                                   descriptor.material_color) &&
            !decode_projected_vertex(type, descriptor, bytes, vertices[i]))
            return SurfaceDrawStatus::unsupported_vertex_format;
    }

    DecodedTexture decoded{};
    const DecodedTexture* texture = nullptr;
    if (descriptor.texture_enabled) {
        if (decode_texture_8888(descriptor, texture_bytes, clut_bytes, decoded) !=
            TextureDecodeStatus::success)
            return SurfaceDrawStatus::texture_decode_failed;
        texture = &decoded;
    }

    if (descriptor.type == PrimitiveType::rectangles && vertices.size() == 2u) {
        ThroughVertex top_left = vertices[0];
        ThroughVertex top_right = vertices[0];
        ThroughVertex bottom_left = vertices[1];
        ThroughVertex bottom_right = vertices[1];
        top_right.x = vertices[1].x; top_right.y = vertices[0].y;
        top_right.u = vertices[1].u; top_right.v = vertices[0].v;
        bottom_left.x = vertices[0].x; bottom_left.y = vertices[1].y;
        bottom_left.u = vertices[0].u; bottom_left.v = vertices[1].v;
        raster_triangle(top_left, bottom_left, top_right, descriptor, texture);
        raster_triangle(top_right, bottom_left, bottom_right, descriptor, texture);
        return SurfaceDrawStatus::success;
    }
    if (descriptor.type == PrimitiveType::triangle_strip && vertices.size() >= 3u) {
        for (std::size_t i = 2u; i < vertices.size(); ++i) {
            if ((i & 1u) == 0u)
                raster_ge_triangle(vertices[i - 2u], vertices[i - 1u], vertices[i], descriptor, texture);
            else
                raster_ge_triangle(vertices[i - 1u], vertices[i - 2u], vertices[i], descriptor, texture);
        }
        return SurfaceDrawStatus::success;
    }
    if (descriptor.type == PrimitiveType::triangles && vertices.size() >= 3u) {
        for (std::size_t i = 2u; i < vertices.size(); i += 3u)
            raster_ge_triangle(vertices[i - 2u], vertices[i - 1u], vertices[i],
                               descriptor, texture);
        return SurfaceDrawStatus::success;
    }
    if (descriptor.type == PrimitiveType::triangle_fan && vertices.size() >= 3u) {
        for (std::size_t i = 2u; i < vertices.size(); ++i)
            raster_ge_triangle(vertices[0], vertices[i - 1u], vertices[i],
                               descriptor, texture);
        return SurfaceDrawStatus::success;
    }
    if (descriptor.type == PrimitiveType::lines && vertices.size() >= 2u) {
        for (std::size_t i = 1u; i < vertices.size(); i += 2u)
            raster_line(vertices[i - 1u], vertices[i], descriptor, texture);
        return SurfaceDrawStatus::success;
    }
    return SurfaceDrawStatus::unsupported_primitive;
}

void GeSoftwareSurface::raster_ge_triangle(
    const ThroughVertex& a, const ThroughVertex& b, const ThroughVertex& c,
    const PrimitiveDraw& draw, const DecodedTexture* texture) {
    if (!a.needs_projection && !b.needs_projection && !c.needs_projection) {
        raster_triangle(a, b, c, draw, texture);
        return;
    }
    if (!a.valid || !b.valid || !c.valid) return;

    std::vector<ThroughVertex> polygon{a, b, c};
    for (std::uint32_t plane = 0u; plane < 6u && !polygon.empty(); ++plane) {
        std::vector<ThroughVertex> output;
        output.reserve(polygon.size() + 1u);
        ThroughVertex previous = polygon.back();
        float previous_distance = clip_distance(previous, plane);
        bool previous_inside = previous_distance >= 0.0f;
        for (const ThroughVertex& current : polygon) {
            const float current_distance = clip_distance(current, plane);
            const bool current_inside = current_distance >= 0.0f;
            if (current_inside != previous_inside) {
                const float denominator = previous_distance - current_distance;
                const float t = std::fabs(denominator) < 1.0e-20f
                    ? 0.0f : previous_distance / denominator;
                output.push_back(interpolate_clip_vertex(
                    previous, current, std::clamp(t, 0.0f, 1.0f)));
            }
            if (current_inside) output.push_back(current);
            previous = current;
            previous_distance = current_distance;
            previous_inside = current_inside;
        }
        polygon = std::move(output);
    }
    if (polygon.size() < 3u) return;
    for (ThroughVertex& vertex : polygon)
        if (!project_vertex(vertex, draw)) return;
    for (std::size_t i = 2u; i < polygon.size(); ++i)
        raster_triangle(polygon[0], polygon[i - 1u], polygon[i], draw, texture);
}

void GeSoftwareSurface::raster_line(const ThroughVertex& a, const ThroughVertex& b,
                                    const PrimitiveDraw& draw,
                                    const DecodedTexture* texture) {
    if (!a.valid || !b.valid) return;
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const int steps = static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dy))));
    for (int step = 0; step <= steps; ++step) {
        const float t = steps == 0 ? 0.0f : static_cast<float>(step) / steps;
        const int x = static_cast<int>(std::lround(a.x + dx * t));
        const int y = static_cast<int>(std::lround(a.y + dy * t));
        if (x < 0 || y < 0 || x >= int(width_) || y >= int(height_)) continue;
        ThroughVertex c = b;
        std::uint32_t color = interpolate_color(a, b, c, 1.0f - t, t, 0.0f);
        if (texture != nullptr) {
            int u = static_cast<int>(std::floor(a.u + (b.u - a.u) * t));
            int v = static_cast<int>(std::floor(a.v + (b.v - a.v) * t));
            if (draw.texture_clamp_u) u = std::clamp(u, 0, int(texture->width) - 1);
            else u &= int(texture->width - 1u);
            if (draw.texture_clamp_v) v = std::clamp(v, 0, int(texture->height) - 1);
            else v &= int(texture->height - 1u);
            const std::uint32_t texel = texture->pixels[
                static_cast<std::size_t>(v) * texture->width + static_cast<std::size_t>(u)];
            color = draw.texture_function == 3u
                ? texel
                : modulate(color, texel, draw.texture_use_alpha,
                           draw.texture_double_color);
        }
        const float z = a.z + (b.z - a.z) * t;
        write_pixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                    z, color, draw);
    }
}

void GeSoftwareSurface::raster_triangle(
    const ThroughVertex& a, const ThroughVertex& b, const ThroughVertex& c,
    const PrimitiveDraw& draw, const DecodedTexture* texture) {
    if (!a.valid || !b.valid || !c.valid) return;
    const float area = edge(a, b, static_cast<float>(c.x), static_cast<float>(c.y));
    if (area == 0.0f) return;
    const auto& coverage_pixel = software_pixel_test();
    const float diagnostic_x = static_cast<float>(coverage_pixel[0]) + 0.5f;
    const float diagnostic_y = static_cast<float>(coverage_pixel[1]) + 0.5f;
    const float diagnostic_wa = edge(b, c, diagnostic_x, diagnostic_y) / area;
    const float diagnostic_wb = edge(c, a, diagnostic_x, diagnostic_y) / area;
    const float diagnostic_wc = edge(a, b, diagnostic_x, diagnostic_y) / area;
    const bool covers_diagnostic = coverage_pixel[0] != 0xFFFFFFFFu &&
        diagnostic_wa >= 0.0f && diagnostic_wb >= 0.0f && diagnostic_wc >= 0.0f;
    // GE 0x1D enables face culling; 0x9B selects the accepted winding.
    // Rectangle primitives are PSP sprites and are not mesh-culled here.
    if (!draw.clear_mode && draw.type != PrimitiveType::rectangles &&
        draw.cull_enabled) {
        const bool counter_clockwise = area < 0.0f;
        if (counter_clockwise != draw.accept_counter_clockwise) {
            if (covers_diagnostic)
                std::printf(
                    "GECOVER xy=%u,%u list=%08X cmd=%08X CULL area=%.9g "
                    "winding=%u accepted=%u tri=(%.4g,%.4g)(%.4g,%.4g)(%.4g,%.4g)\n",
                    coverage_pixel[0], coverage_pixel[1], draw.list_address,
                    draw.command_address, area, counter_clockwise ? 1u : 0u,
                    draw.accept_counter_clockwise ? 1u : 0u, a.x, a.y, b.x, b.y,
                    c.x, c.y);
            return;
        }
    }
    const int min_x = std::max(0, std::min({int(a.x), int(b.x), int(c.x)}));
    const int min_y = std::max(0, std::min({int(a.y), int(b.y), int(c.y)}));
    const int max_x = std::min(int(width_) - 1, std::max({int(a.x), int(b.x), int(c.x)}) - 1);
    const int max_y = std::min(int(height_) - 1, std::max({int(a.y), int(b.y), int(c.y)}) - 1);
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float wa = edge(b, c, px, py) / area;
            const float wb = edge(c, a, px, py) / area;
            const float wc = edge(a, b, px, py) / area;
            if (wa < 0.0f || wb < 0.0f || wc < 0.0f) continue;
            const float denominator = wa * a.inv_w + wb * b.inv_w + wc * c.inv_w;
            if (!std::isfinite(denominator) || std::fabs(denominator) < 1.0e-20f)
                continue;
            const float pa = wa * a.inv_w / denominator;
            const float pb = wb * b.inv_w / denominator;
            const float pc = wc * c.inv_w / denominator;
            std::uint32_t color = interpolate_color(a, b, c, pa, pb, pc);
            if (texture != nullptr) {
                int u = static_cast<int>(std::floor(a.u * pa + b.u * pb + c.u * pc));
                int v = static_cast<int>(std::floor(a.v * pa + b.v * pb + c.v * pc));
                if (draw.texture_clamp_u) u = std::clamp(u, 0, int(texture->width) - 1);
                else u &= int(texture->width - 1u);
                if (draw.texture_clamp_v) v = std::clamp(v, 0, int(texture->height) - 1);
                else v &= int(texture->height - 1u);
                const std::uint32_t texel = texture->pixels[
                    static_cast<std::size_t>(v) * texture->width + static_cast<std::size_t>(u)];
                if (draw.texture_function == 3u) color = texel;
                else color = modulate(color, texel, draw.texture_use_alpha,
                                      draw.texture_double_color);
            }
            const float z = a.z * wa + b.z * wb + c.z * wc;
            write_pixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                        z, color, draw);
        }
    }
}

void GeSoftwareSurface::write_pixel(const std::uint32_t x, const std::uint32_t y,
                                    const float zf, const std::uint32_t color,
    const PrimitiveDraw& draw) noexcept {
    const std::uint8_t source_alpha = channel(color, 24u);
    const bool alpha_passed = draw.clear_mode || !draw.alpha_test_enabled ||
        compare_alpha(draw.alpha_function, source_alpha & draw.alpha_mask,
                      draw.alpha_reference & draw.alpha_mask);
    const auto& diagnostic_pixel = software_pixel_test();
    const bool diagnose = x == diagnostic_pixel[0] && y == diagnostic_pixel[1];
    if (!alpha_passed) {
        if (diagnose)
            std::printf("GEPIXELTEST xy=%u,%u list=%08X cmd=%08X alpha=FAIL "
                        "src=%08X func=%u ref=%u mask=%u\n", x, y,
                        draw.list_address, draw.command_address, color,
                        draw.alpha_function, draw.alpha_reference, draw.alpha_mask);
        return;
    }

    const std::size_t index = static_cast<std::size_t>(y) * width_ + x;
    const std::uint16_t z = static_cast<std::uint16_t>(
        std::clamp(std::lround(zf), 0l, 65535l));
    if (draw.clear_mode && draw.clear_depth) {
        depth_[index] = z;
    } else {
        bool depth_passed = true;
        if (draw.depth_test_enabled) {
            const std::uint16_t stored = depth_[index];
            switch (draw.depth_function & 7u) {
            case 0u: depth_passed = false; break;
            case 1u: depth_passed = true; break;
            case 2u: depth_passed = z == stored; break;
            case 3u: depth_passed = z != stored; break;
            case 4u: depth_passed = z < stored; break;
            case 5u: depth_passed = z <= stored; break;
            case 6u: depth_passed = z > stored; break;
            case 7u: depth_passed = z >= stored; break;
            }
        }
        if (diagnose)
            std::printf("GEPIXELTEST xy=%u,%u list=%08X cmd=%08X src=%08X "
                        "z=%u stored=%u depth=%s/%u write=%u blend=%u/%u,%u,%u\n",
                        x, y, draw.list_address, draw.command_address, color, z,
                        depth_[index], depth_passed ? "PASS" : "FAIL",
                        draw.depth_function, draw.depth_write_enabled ? 1u : 0u,
                        draw.blend_enabled ? 1u : 0u, draw.blend_equation,
                        draw.blend_source_factor, draw.blend_destination_factor);
        if (!depth_passed) return;
        if (draw.depth_write_enabled) depth_[index] = z;
    }

    if (draw.clear_mode) {
        const std::uint32_t destination = pixels_[index];
        std::uint32_t cleared = color;
        if (!draw.clear_color)
            cleared = (cleared & 0xFF000000u) | (destination & 0x00FFFFFFu);
        if (!draw.clear_alpha)
            cleared = (cleared & 0x00FFFFFFu) | (destination & 0xFF000000u);
        pixels_[index] = cleared;
        return;
    }
    if (!draw.blend_enabled) {
        pixels_[index] = color;
        return;
    }

    const std::uint32_t destination = pixels_[index];
    // Fate's observed UI/actor path uses the PSP's ordinary ADD,
    // SRC_ALPHA, ONE_MINUS_SRC_ALPHA blend. Keep unsupported factor pairs
    // conservative until a captured draw proves another equation is needed.
    if (draw.blend_equation == 0u && draw.blend_source_factor == 2u &&
        draw.blend_destination_factor == 3u) {
        const std::uint32_t inverse = 255u - source_alpha;
        std::uint32_t result = 0u;
        for (const unsigned shift : {0u, 8u, 16u, 24u}) {
            const std::uint32_t value =
                (static_cast<std::uint32_t>(channel(color, shift)) * source_alpha +
                 static_cast<std::uint32_t>(channel(destination, shift)) * inverse + 127u) /
                255u;
            result |= value << shift;
        }
        pixels_[index] = result;
        return;
    }
    const auto source_float = color_float(color);
    const auto destination_float = color_float(destination);
    const auto source_factor = blend_factor(draw.blend_source_factor, true,
        source_float, destination_float, draw.blend_fixed_source);
    const auto destination_factor = blend_factor(draw.blend_destination_factor, false,
        source_float, destination_float, draw.blend_fixed_destination);
    std::uint32_t result = 0u;
    for (std::size_t component = 0u; component < 4u; ++component) {
        const float a = source_float[component] * source_factor[component];
        const float b = destination_float[component] * destination_factor[component];
        float value = 0.0f;
        switch (draw.blend_equation & 7u) {
        case 0u: value = a + b; break;
        case 1u: value = a - b; break;
        case 2u: value = b - a; break;
        case 3u: value = std::min(source_float[component], destination_float[component]); break;
        case 4u: value = std::max(source_float[component], destination_float[component]); break;
        case 5u: value = std::fabs(source_float[component] - destination_float[component]); break;
        default: value = a + b; break;
        }
        result |= static_cast<std::uint32_t>(std::clamp(
            std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f), 0l, 255l))
            << static_cast<unsigned>(component * 8u);
    }
    pixels_[index] = result;
}

} // namespace fuc::renderer::psp

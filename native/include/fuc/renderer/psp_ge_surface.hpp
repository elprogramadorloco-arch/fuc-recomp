#pragma once

#include "fuc/renderer/psp_ge_command.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace fuc::renderer::psp {

enum class SurfaceDrawStatus {
    success,
    unsupported_vertex_format,
    unsupported_primitive,
    invalid_vertex_data,
    texture_decode_failed,
};

class GeSurface {
public:
    virtual ~GeSurface() = default;
    [[nodiscard]] virtual SurfaceDrawStatus draw(
        const PrimitiveDraw& draw,
        std::span<const std::uint8_t> vertex_bytes,
        std::span<const std::uint8_t> texture_bytes = {},
        std::span<const std::uint8_t> clut_bytes = {}) = 0;
    [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
    virtual void finish_frame() {}
    [[nodiscard]] virtual std::span<const std::uint32_t> pixels() = 0;
    [[nodiscard]] virtual bool gpu_accelerated() const noexcept { return false; }
};

class GeSoftwareSurface final : public GeSurface {
public:
    explicit GeSoftwareSurface(std::uint32_t width = 480u,
                               std::uint32_t height = 272u);

    void clear(std::uint32_t color = 0u) noexcept;
    [[nodiscard]] SurfaceDrawStatus draw(
        const PrimitiveDraw& draw,
        std::span<const std::uint8_t> vertex_bytes,
        std::span<const std::uint8_t> texture_bytes = {},
        std::span<const std::uint8_t> clut_bytes = {}) override;
    [[nodiscard]] std::uint32_t width() const noexcept override { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept override { return height_; }
    [[nodiscard]] std::span<const std::uint32_t> pixels() override {
        return pixels_;
    }

private:
    void raster_triangle(const ThroughVertex& a, const ThroughVertex& b,
                         const ThroughVertex& c, const PrimitiveDraw& draw,
                         const DecodedTexture* texture);
    void raster_ge_triangle(const ThroughVertex& a, const ThroughVertex& b,
                            const ThroughVertex& c, const PrimitiveDraw& draw,
                            const DecodedTexture* texture);
    void raster_line(const ThroughVertex& a, const ThroughVertex& b,
                     const PrimitiveDraw& draw, const DecodedTexture* texture);
    void write_pixel(std::uint32_t x, std::uint32_t y, float z,
                     std::uint32_t color,
                     const PrimitiveDraw& draw) noexcept;

    std::uint32_t width_{};
    std::uint32_t height_{};
    std::vector<std::uint32_t> pixels_{};
    std::vector<std::uint16_t> depth_{};
};

} // namespace fuc::renderer::psp

#pragma once

#include "fuc/renderer/psp_ge_surface.hpp"

#include <memory>

namespace fuc::renderer::psp {

class GeD3D11Surface final : public GeSurface {
public:
    explicit GeD3D11Surface(std::uint32_t width = 480u,
                            std::uint32_t height = 272u);
    ~GeD3D11Surface() override;
    GeD3D11Surface(const GeD3D11Surface&) = delete;
    GeD3D11Surface& operator=(const GeD3D11Surface&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] SurfaceDrawStatus draw(
        const PrimitiveDraw& draw,
        std::span<const std::uint8_t> vertex_bytes,
        std::span<const std::uint8_t> texture_bytes = {},
        std::span<const std::uint8_t> clut_bytes = {}) override;
    [[nodiscard]] std::uint32_t width() const noexcept override;
    [[nodiscard]] std::uint32_t height() const noexcept override;
    void finish_frame() override;
    [[nodiscard]] std::span<const std::uint32_t> pixels() override;
    [[nodiscard]] bool gpu_accelerated() const noexcept override { return available(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::unique_ptr<GeSurface> make_ge_surface(
    bool prefer_gpu, std::uint32_t width = 480u, std::uint32_t height = 272u);

} // namespace fuc::renderer::psp

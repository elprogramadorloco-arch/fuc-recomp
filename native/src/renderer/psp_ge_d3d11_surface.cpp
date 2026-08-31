#include "fuc/renderer/psp_ge_d3d11_surface.hpp"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace fuc::renderer::psp {
namespace {

using Microsoft::WRL::ComPtr;

struct GpuVertex {
    float position[4]{};
    float uv[2]{};
    std::uint32_t color{};
};

struct PixelConstants {
    std::uint32_t texture_enabled{};
    std::uint32_t texture_width{};
    std::uint32_t texture_height{};
    std::uint32_t clamp_bits{};
    std::uint32_t texture_function{};
    std::uint32_t texture_use_alpha{};
    std::uint32_t texture_double_color{};
    std::uint32_t alpha_test_enabled{};
    std::uint32_t alpha_function{};
    std::uint32_t alpha_reference{};
    std::uint32_t alpha_mask{};
    std::uint32_t padding{};
};

constexpr char kShader[] = R"(
struct VSIn { float4 position : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
VSOut vs_main(VSIn input) {
    VSOut output;
    output.position = input.position;
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
Texture2D<float4> psp_texture : register(t0);
cbuffer PixelConstants : register(b0) {
    uint texture_enabled, texture_width, texture_height, clamp_bits;
    uint texture_function, texture_use_alpha, texture_double_color, alpha_test_enabled;
    uint alpha_function, alpha_reference, alpha_mask, padding;
};
bool alpha_pass(uint value, uint reference) {
    switch (alpha_function & 7) {
    case 0: return false; case 1: return true; case 2: return value == reference;
    case 3: return value != reference; case 4: return value < reference;
    case 5: return value <= reference; case 6: return value > reference;
    default: return value >= reference;
    }
}
float4 ps_main(VSOut input) : SV_TARGET {
    float4 result = input.color;
    if (texture_enabled != 0) {
        int2 coordinate = int2(floor(input.uv));
        if ((clamp_bits & 1) != 0) coordinate.x = clamp(coordinate.x, 0, int(texture_width) - 1);
        else coordinate.x = coordinate.x & (int(texture_width) - 1);
        if ((clamp_bits & 2) != 0) coordinate.y = clamp(coordinate.y, 0, int(texture_height) - 1);
        else coordinate.y = coordinate.y & (int(texture_height) - 1);
        float4 texel = psp_texture.Load(int3(coordinate, 0));
        if (texture_function == 3) result = texel;
        else {
            float factor = texture_double_color != 0 ? 2.0 : 1.0;
            result.rgb = saturate(result.rgb * texel.rgb * factor);
            if (texture_use_alpha != 0) result.a *= texel.a;
        }
    }
    if (alpha_test_enabled != 0) {
        uint value = (uint)round(saturate(result.a) * 255.0) & alpha_mask;
        if (!alpha_pass(value, alpha_reference & alpha_mask)) discard;
    }
    return result;
}
)";

std::uint64_t hash_bytes(std::span<const std::uint8_t> bytes,
                         std::uint64_t hash = 1469598103934665603ull) noexcept {
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

D3D11_COMPARISON_FUNC comparison(const std::uint8_t value) noexcept {
    constexpr D3D11_COMPARISON_FUNC table[] = {
        D3D11_COMPARISON_NEVER, D3D11_COMPARISON_ALWAYS,
        D3D11_COMPARISON_EQUAL, D3D11_COMPARISON_NOT_EQUAL,
        D3D11_COMPARISON_LESS, D3D11_COMPARISON_LESS_EQUAL,
        D3D11_COMPARISON_GREATER, D3D11_COMPARISON_GREATER_EQUAL,
    };
    return table[value & 7u];
}

D3D11_BLEND blend_factor(const std::uint8_t value, const bool source) noexcept {
    switch (value) {
    case 0u: return source ? D3D11_BLEND_DEST_COLOR : D3D11_BLEND_SRC_COLOR;
    case 1u: return source ? D3D11_BLEND_INV_DEST_COLOR : D3D11_BLEND_INV_SRC_COLOR;
    case 2u: return D3D11_BLEND_SRC_ALPHA;
    case 3u: return D3D11_BLEND_INV_SRC_ALPHA;
    case 4u: return D3D11_BLEND_DEST_ALPHA;
    case 5u: return D3D11_BLEND_INV_DEST_ALPHA;
    case 10u: return D3D11_BLEND_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
    }
}

D3D11_BLEND_OP blend_operation(const std::uint8_t value) noexcept {
    switch (value & 7u) {
    case 1u: return D3D11_BLEND_OP_SUBTRACT;
    case 2u: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 3u: return D3D11_BLEND_OP_MIN;
    case 4u: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
    }
}

bool decode_gpu_vertex(const VertexType type, const PrimitiveDraw& draw,
                       const std::span<const std::uint8_t> bytes,
                       const std::uint32_t width, const std::uint32_t height,
                       GpuVertex& output) {
    ThroughVertex vertex{};
    if (!decode_through_vertex(type, bytes, vertex, draw.material_color) &&
        !decode_projected_vertex(type, draw, bytes, vertex))
        return false;
    if (!vertex.valid) return false;

    float screen_x = vertex.x;
    float screen_y = vertex.y;
    float screen_z = vertex.z;
    float clip_w = 1.0f;
    if (vertex.needs_projection) {
        if (!std::isfinite(vertex.clip_w) || std::fabs(vertex.clip_w) < 1.0e-12f)
            return false;
        clip_w = vertex.clip_w;
        const float inverse_w = 1.0f / clip_w;
        screen_x = vertex.x * inverse_w * draw.viewport_scale_x +
                   draw.viewport_center_x - draw.viewport_offset_x;
        screen_y = vertex.y * inverse_w * draw.viewport_scale_y +
                   draw.viewport_center_y - draw.viewport_offset_y;
        screen_z = vertex.z * inverse_w * draw.viewport_scale_z +
                   draw.viewport_center_z;
    }
    // Keep vertices behind a clip plane.  D3D clips the assembled primitive;
    // rejecting an individual negative-W vertex here incorrectly discarded
    // PSP triangles that cross the near plane (the software path clips them).
    if (!std::isfinite(screen_x) || !std::isfinite(screen_y) ||
        !std::isfinite(screen_z))
        return false;
    const float ndc_x = screen_x * (2.0f / static_cast<float>(width)) - 1.0f;
    const float ndc_y = 1.0f - screen_y * (2.0f / static_cast<float>(height));
    const float ndc_z = std::clamp(screen_z / 65535.0f, 0.0f, 1.0f);
    output.position[0] = ndc_x * clip_w;
    output.position[1] = ndc_y * clip_w;
    output.position[2] = ndc_z * clip_w;
    output.position[3] = clip_w;
    output.uv[0] = vertex.u;
    output.uv[1] = vertex.v;
    output.color = vertex.color;
    return true;
}

} // namespace

struct GeD3D11Surface::Impl {
    struct TextureEntry {
        std::uint64_t content_hash{};
        ComPtr<ID3D11ShaderResourceView> view{};
    };
    struct PendingDraw {
        PrimitiveDraw descriptor{};
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        std::vector<GpuVertex> vertices{};
        ComPtr<ID3D11ShaderResourceView> texture{};
    };

    std::uint32_t width{};
    std::uint32_t height{};
    bool ready{};
    bool dirty{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> color;
    ComPtr<ID3D11RenderTargetView> render_target;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> depth_view;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11InputLayout> input_layout;
    ComPtr<ID3D11Buffer> vertex_buffer;
    std::size_t vertex_capacity{};
    std::array<ComPtr<ID3D11RasterizerState>, 3> rasterizers;
    std::unordered_map<std::uint64_t, ComPtr<ID3D11BlendState>> blend_states;
    std::unordered_map<std::uint32_t, ComPtr<ID3D11DepthStencilState>> depth_states;
    std::unordered_map<std::uint64_t, TextureEntry> textures;
    std::unordered_map<std::uint64_t, ComPtr<ID3D11Buffer>> constant_buffers;
    std::vector<PendingDraw> pending;
    std::vector<GpuVertex> upload_vertices;
    std::vector<std::uint32_t> readback;

    explicit Impl(const std::uint32_t surface_width,
                  const std::uint32_t surface_height)
        : width(surface_width), height(surface_height),
          readback(static_cast<std::size_t>(surface_width) * surface_height) {
        initialize();
    }

    void initialize() {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL obtained{};
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        HRESULT result = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &device, &obtained, &context);
#if defined(_DEBUG)
        if (FAILED(result)) {
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            result = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                &device, &obtained, &context);
        }
#endif
        if (FAILED(result)) return;

        D3D11_TEXTURE2D_DESC color_desc{};
        color_desc.Width = width; color_desc.Height = height;
        color_desc.MipLevels = 1; color_desc.ArraySize = 1;
        color_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        color_desc.SampleDesc.Count = 1;
        color_desc.Usage = D3D11_USAGE_DEFAULT;
        color_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&color_desc, nullptr, &color)) ||
            FAILED(device->CreateRenderTargetView(color.Get(), nullptr, &render_target)))
            return;
        D3D11_TEXTURE2D_DESC staging_desc = color_desc;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.BindFlags = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &staging))) return;

        D3D11_TEXTURE2D_DESC depth_desc = color_desc;
        depth_desc.Format = DXGI_FORMAT_D16_UNORM;
        depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(device->CreateTexture2D(&depth_desc, nullptr, &depth)) ||
            FAILED(device->CreateDepthStencilView(depth.Get(), nullptr, &depth_view)))
            return;

        ComPtr<ID3DBlob> vertex_blob, pixel_blob, errors;
        if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1u, nullptr, nullptr, nullptr,
                              "vs_main", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                              &vertex_blob, &errors)) ||
            FAILED(D3DCompile(kShader, sizeof(kShader) - 1u, nullptr, nullptr, nullptr,
                              "ps_main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                              &pixel_blob, &errors)))
            return;
        if (FAILED(device->CreateVertexShader(vertex_blob->GetBufferPointer(),
                                               vertex_blob->GetBufferSize(), nullptr,
                                               &vertex_shader)) ||
            FAILED(device->CreatePixelShader(pixel_blob->GetBufferPointer(),
                                              pixel_blob->GetBufferSize(), nullptr,
                                              &pixel_shader)))
            return;
        constexpr D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
             D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16,
             D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24,
             D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        if (FAILED(device->CreateInputLayout(elements, static_cast<UINT>(std::size(elements)),
                                              vertex_blob->GetBufferPointer(),
                                              vertex_blob->GetBufferSize(), &input_layout)))
            return;
        D3D11_RASTERIZER_DESC raster_desc{};
        raster_desc.FillMode = D3D11_FILL_SOLID;
        raster_desc.CullMode = D3D11_CULL_NONE;
        raster_desc.DepthClipEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&raster_desc, &rasterizers[0]))) return;
        raster_desc.CullMode = D3D11_CULL_BACK;
        raster_desc.FrontCounterClockwise = FALSE;
        if (FAILED(device->CreateRasterizerState(&raster_desc, &rasterizers[1]))) return;
        raster_desc.FrontCounterClockwise = TRUE;
        if (FAILED(device->CreateRasterizerState(&raster_desc, &rasterizers[2]))) return;

        const float transparent[4]{};
        context->ClearRenderTargetView(render_target.Get(), transparent);
        context->ClearDepthStencilView(depth_view.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0u);
        ready = true;
    }

    bool ensure_vertex_buffer(const std::size_t bytes) {
        if (vertex_buffer && vertex_capacity >= bytes) return true;
        vertex_capacity = std::max<std::size_t>(bytes, 64u * 1024u);
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(vertex_capacity);
        description.Usage = D3D11_USAGE_DYNAMIC;
        description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        vertex_buffer.Reset();
        return SUCCEEDED(device->CreateBuffer(&description, nullptr, &vertex_buffer));
    }

    ID3D11Buffer* constant_buffer(const PrimitiveDraw& draw) {
        PixelConstants constants{};
        constants.texture_enabled = draw.texture_enabled;
        constants.texture_width = draw.texture_width;
        constants.texture_height = draw.texture_height;
        constants.clamp_bits = (draw.texture_clamp_u ? 1u : 0u) |
                               (draw.texture_clamp_v ? 2u : 0u);
        constants.texture_function = draw.texture_function;
        constants.texture_use_alpha = draw.texture_use_alpha;
        constants.texture_double_color = draw.texture_double_color;
        constants.alpha_test_enabled = draw.clear_mode ? 0u : draw.alpha_test_enabled;
        constants.alpha_function = draw.alpha_function;
        constants.alpha_reference = draw.alpha_reference;
        constants.alpha_mask = draw.alpha_mask;
        const auto bytes = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(&constants), sizeof(constants));
        const std::uint64_t key = hash_bytes(bytes);
        const auto found = constant_buffers.find(key);
        if (found != constant_buffers.end()) return found->second.Get();
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = sizeof(PixelConstants);
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = &constants;
        ComPtr<ID3D11Buffer> buffer;
        if (FAILED(device->CreateBuffer(&description, &initial, &buffer))) return nullptr;
        return constant_buffers.emplace(key, std::move(buffer)).first->second.Get();
    }

    ID3D11BlendState* blend_state(const PrimitiveDraw& draw) {
        std::uint64_t key = draw.blend_enabled ? 1ull : 0ull;
        key |= static_cast<std::uint64_t>(draw.blend_equation) << 8u;
        key |= static_cast<std::uint64_t>(draw.blend_source_factor) << 16u;
        key |= static_cast<std::uint64_t>(draw.blend_destination_factor) << 24u;
        key |= static_cast<std::uint64_t>(draw.clear_mode) << 32u;
        key |= static_cast<std::uint64_t>(draw.clear_color) << 33u;
        key |= static_cast<std::uint64_t>(draw.clear_alpha) << 34u;
        const auto found = blend_states.find(key);
        if (found != blend_states.end()) return found->second.Get();
        D3D11_BLEND_DESC description{};
        auto& target = description.RenderTarget[0];
        target.RenderTargetWriteMask = draw.clear_mode
            ? static_cast<UINT8>((draw.clear_color ? D3D11_COLOR_WRITE_ENABLE_RED |
                D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE : 0u) |
                (draw.clear_alpha ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0u))
            : D3D11_COLOR_WRITE_ENABLE_ALL;
        target.BlendEnable = draw.blend_enabled && !draw.clear_mode;
        target.SrcBlend = blend_factor(draw.blend_source_factor, true);
        target.DestBlend = blend_factor(draw.blend_destination_factor, false);
        target.BlendOp = blend_operation(draw.blend_equation);
        target.SrcBlendAlpha = target.SrcBlend;
        target.DestBlendAlpha = target.DestBlend;
        target.BlendOpAlpha = target.BlendOp;
        ComPtr<ID3D11BlendState> state;
        if (FAILED(device->CreateBlendState(&description, &state))) return nullptr;
        return blend_states.emplace(key, std::move(state)).first->second.Get();
    }

    ID3D11DepthStencilState* depth_state(const PrimitiveDraw& draw) {
        const std::uint32_t key = static_cast<std::uint32_t>(draw.clear_mode) |
            (static_cast<std::uint32_t>(draw.clear_depth) << 1u) |
            (static_cast<std::uint32_t>(draw.depth_test_enabled) << 2u) |
            (static_cast<std::uint32_t>(draw.depth_write_enabled) << 3u) |
            (static_cast<std::uint32_t>(draw.depth_function) << 8u);
        const auto found = depth_states.find(key);
        if (found != depth_states.end()) return found->second.Get();
        D3D11_DEPTH_STENCIL_DESC description{};
        description.DepthEnable = draw.clear_mode ? draw.clear_depth : draw.depth_test_enabled;
        description.DepthWriteMask = (draw.clear_mode ? draw.clear_depth : draw.depth_write_enabled)
            ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        description.DepthFunc = draw.clear_mode ? D3D11_COMPARISON_ALWAYS
                                                : comparison(draw.depth_function);
        ComPtr<ID3D11DepthStencilState> state;
        if (FAILED(device->CreateDepthStencilState(&description, &state))) return nullptr;
        return depth_states.emplace(key, std::move(state)).first->second.Get();
    }

    ID3D11ShaderResourceView* texture_view(
        const PrimitiveDraw& draw, const std::span<const std::uint8_t> texture_bytes,
        const std::span<const std::uint8_t> clut_bytes, SurfaceDrawStatus& status) {
        if (!draw.texture_enabled) return nullptr;
        // CLUT animation is common and is always validated. Direct 8888
        // framebuffers are dynamic too. Indexed maps reached here remain stable
        // in the measured Fate routes; hashing every large map on every draw was
        // measured at 17.35 ms/frame and did not alter the divergent stage pixel.
        std::uint64_t content_hash = hash_bytes(clut_bytes);
        if (draw.texture_format == 3u)
            content_hash ^= hash_bytes(texture_bytes) + 0x9E3779B97F4A7C15ull +
                            (content_hash << 6u) + (content_hash >> 2u);
        std::uint64_t key = 1469598103934665603ull;
        const auto mix = [&key](const std::uint64_t value) {
            key ^= value;
            key *= 1099511628211ull;
        };
        mix(draw.texture_address); mix(draw.clut_address);
        mix(draw.texture_buffer_width); mix(draw.texture_width);
        mix(draw.texture_height); mix(draw.texture_format);
        mix(draw.texture_swizzled); mix(draw.clut_format);
        mix(draw.clut_shift); mix(draw.clut_mask); mix(draw.clut_start);
        auto found = textures.find(key);
        if (found != textures.end() && found->second.content_hash == content_hash)
            return found->second.view.Get();

        DecodedTexture decoded{};
        if (decode_texture_8888(draw, texture_bytes, clut_bytes, decoded) !=
            TextureDecodeStatus::success) {
            status = SurfaceDrawStatus::texture_decode_failed;
            return nullptr;
        }
        D3D11_TEXTURE2D_DESC description{};
        description.Width = decoded.width; description.Height = decoded.height;
        description.MipLevels = 1; description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = decoded.pixels.data();
        initial.SysMemPitch = decoded.width * sizeof(std::uint32_t);
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(device->CreateTexture2D(&description, &initial, &texture)) ||
            FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &view))) {
            status = SurfaceDrawStatus::texture_decode_failed;
            return nullptr;
        }
        TextureEntry entry{content_hash, std::move(view)};
        return textures.insert_or_assign(key, std::move(entry)).first->second.view.Get();
    }

    SurfaceDrawStatus draw(const PrimitiveDraw& descriptor,
                           const std::span<const std::uint8_t> vertex_bytes,
                           const std::span<const std::uint8_t> texture_bytes,
                           const std::span<const std::uint8_t> clut_bytes) {
        if (!ready) return SurfaceDrawStatus::invalid_vertex_data;
        const VertexType type = decode_vertex_type(
            decode_command(0x12000000u | descriptor.vertex_type));
        const VertexLayout layout = vertex_layout(type);
        if (layout.stride == 0u || descriptor.vertex_count == 0u ||
            vertex_bytes.size() < static_cast<std::size_t>(layout.stride) *
                                    descriptor.vertex_count)
            return SurfaceDrawStatus::invalid_vertex_data;

        std::vector<GpuVertex> source(descriptor.vertex_count);
        for (std::uint32_t index = 0u; index < descriptor.vertex_count; ++index) {
            if (!decode_gpu_vertex(type, descriptor,
                    vertex_bytes.subspan(static_cast<std::size_t>(index) * layout.stride,
                                         layout.stride),
                    width, height, source[index]))
                return SurfaceDrawStatus::unsupported_vertex_format;
        }
        std::vector<GpuVertex> vertices;
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        const auto triangle = [&vertices](const GpuVertex& a, const GpuVertex& b,
                                          const GpuVertex& c) {
            vertices.push_back(a); vertices.push_back(b); vertices.push_back(c);
        };
        switch (descriptor.type) {
        case PrimitiveType::rectangles:
            if (source.size() != 2u) return SurfaceDrawStatus::unsupported_primitive;
            {
                GpuVertex top_right = source[0], bottom_left = source[1];
                top_right.position[0] = source[1].position[0];
                top_right.uv[0] = source[1].uv[0];
                bottom_left.position[0] = source[0].position[0];
                bottom_left.uv[0] = source[0].uv[0];
                triangle(source[0], bottom_left, top_right);
                triangle(top_right, bottom_left, source[1]);
            }
            break;
        case PrimitiveType::triangles:
            for (std::size_t i = 2u; i < source.size(); i += 3u)
                triangle(source[i - 2u], source[i - 1u], source[i]);
            break;
        case PrimitiveType::triangle_strip:
            for (std::size_t i = 2u; i < source.size(); ++i) {
                if ((i & 1u) == 0u) triangle(source[i - 2u], source[i - 1u], source[i]);
                else triangle(source[i - 1u], source[i - 2u], source[i]);
            }
            break;
        case PrimitiveType::triangle_fan:
            for (std::size_t i = 2u; i < source.size(); ++i)
                triangle(source[0], source[i - 1u], source[i]);
            break;
        case PrimitiveType::lines:
            topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
            vertices = std::move(source);
            break;
        default:
            return SurfaceDrawStatus::unsupported_primitive;
        }
        if (vertices.empty()) return SurfaceDrawStatus::success;
        SurfaceDrawStatus texture_status = SurfaceDrawStatus::success;
        ID3D11ShaderResourceView* texture = texture_view(
            descriptor, texture_bytes, clut_bytes, texture_status);
        if (descriptor.texture_enabled && texture == nullptr) return texture_status;
        PendingDraw queued{};
        queued.descriptor = descriptor;
        queued.topology = topology;
        queued.vertices = std::move(vertices);
        queued.texture = texture;
        pending.push_back(std::move(queued));
        return SurfaceDrawStatus::success;
    }

    void finish_frame() {
        if (!ready || pending.empty()) return;
        std::size_t vertex_count = 0u;
        for (const auto& draw : pending) vertex_count += draw.vertices.size();
        upload_vertices.clear();
        upload_vertices.reserve(vertex_count);
        for (const auto& draw : pending)
            upload_vertices.insert(upload_vertices.end(), draw.vertices.begin(),
                                   draw.vertices.end());
        const std::size_t upload_size = upload_vertices.size() * sizeof(GpuVertex);
        if (!ensure_vertex_buffer(upload_size)) { pending.clear(); return; }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(vertex_buffer.Get(), 0u, D3D11_MAP_WRITE_DISCARD, 0u,
                                &mapped))) {
            pending.clear();
            return;
        }
        std::memcpy(mapped.pData, upload_vertices.data(), upload_size);
        context->Unmap(vertex_buffer.Get(), 0u);
        const UINT stride = sizeof(GpuVertex), offset = 0u;
        ID3D11Buffer* buffer = vertex_buffer.Get();
        context->IASetInputLayout(input_layout.Get());
        context->IASetVertexBuffers(0u, 1u, &buffer, &stride, &offset);
        context->VSSetShader(vertex_shader.Get(), nullptr, 0u);
        context->PSSetShader(pixel_shader.Get(), nullptr, 0u);
        D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width),
                                static_cast<float>(height), 0.0f, 1.0f};
        context->RSSetViewports(1u, &viewport);
        context->OMSetRenderTargets(1u, render_target.GetAddressOf(), depth_view.Get());
        UINT first_vertex = 0u;
        for (const auto& queued : pending) {
            const PrimitiveDraw& descriptor = queued.descriptor;
            const std::size_t raster_index =
                !descriptor.cull_enabled || descriptor.clear_mode ||
                    descriptor.type == PrimitiveType::rectangles
                // The software path measures winding after the PSP viewport Y
                // transform. D3D's rasterizer observes the opposite screen-Y
                // convention, so its front-face selection is inverted here.
                ? 0u : (descriptor.accept_counter_clockwise ? 1u : 2u);
            context->RSSetState(rasterizers[raster_index].Get());
            ID3D11BlendState* blend = blend_state(descriptor);
            ID3D11DepthStencilState* depth_stencil = depth_state(descriptor);
            ID3D11Buffer* constants = constant_buffer(descriptor);
            if (blend == nullptr || depth_stencil == nullptr || constants == nullptr) {
                first_vertex += static_cast<UINT>(queued.vertices.size());
                continue;
            }
            // D3D11 exposes one blend constant. When exactly one PSP side uses
            // FIX, supply that side's value. Survival stage draw 0x092A073C is
            // SRC_ALPHA/FIX (2,10); passing FIXA instead of FIXB made its floor
            // triangle nearly white on GPU while software used FIXB correctly.
            const std::uint32_t fixed =
                descriptor.blend_destination_factor == 10u &&
                    descriptor.blend_source_factor != 10u
                ? descriptor.blend_fixed_destination
                : descriptor.blend_fixed_source;
            float blend_constant[4] = {
                static_cast<float>(fixed & 0xFFu) / 255.0f,
                static_cast<float>((fixed >> 8u) & 0xFFu) / 255.0f,
                static_cast<float>((fixed >> 16u) & 0xFFu) / 255.0f,
                static_cast<float>((fixed >> 24u) & 0xFFu) / 255.0f,
            };
            ID3D11ShaderResourceView* texture = queued.texture.Get();
            context->IASetPrimitiveTopology(queued.topology);
            context->PSSetConstantBuffers(0u, 1u, &constants);
            context->PSSetShaderResources(0u, 1u, &texture);
            context->OMSetBlendState(blend, blend_constant, 0xFFFFFFFFu);
            context->OMSetDepthStencilState(depth_stencil, 0u);
            context->Draw(static_cast<UINT>(queued.vertices.size()), first_vertex);
            first_vertex += static_cast<UINT>(queued.vertices.size());
        }
        ID3D11ShaderResourceView* null_view = nullptr;
        context->PSSetShaderResources(0u, 1u, &null_view);
        pending.clear();
        dirty = true;
    }

    std::span<const std::uint32_t> pixels() {
        finish_frame();
        if (!ready || !dirty) return readback;
        context->CopyResource(staging.Get(), color.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging.Get(), 0u, D3D11_MAP_READ, 0u, &mapped))) {
            for (std::uint32_t y = 0u; y < height; ++y) {
                std::memcpy(readback.data() + static_cast<std::size_t>(y) * width,
                            static_cast<const std::uint8_t*>(mapped.pData) +
                                static_cast<std::size_t>(y) * mapped.RowPitch,
                            static_cast<std::size_t>(width) * sizeof(std::uint32_t));
            }
            context->Unmap(staging.Get(), 0u);
            dirty = false;
        }
        return readback;
    }
};

GeD3D11Surface::GeD3D11Surface(const std::uint32_t width,
                               const std::uint32_t height)
    : impl_(std::make_unique<Impl>(width, height)) {}
GeD3D11Surface::~GeD3D11Surface() = default;
bool GeD3D11Surface::available() const noexcept { return impl_ && impl_->ready; }
SurfaceDrawStatus GeD3D11Surface::draw(
    const PrimitiveDraw& draw, const std::span<const std::uint8_t> vertex_bytes,
    const std::span<const std::uint8_t> texture_bytes,
    const std::span<const std::uint8_t> clut_bytes) {
    return impl_->draw(draw, vertex_bytes, texture_bytes, clut_bytes);
}
std::uint32_t GeD3D11Surface::width() const noexcept { return impl_->width; }
std::uint32_t GeD3D11Surface::height() const noexcept { return impl_->height; }
void GeD3D11Surface::finish_frame() { impl_->finish_frame(); }
std::span<const std::uint32_t> GeD3D11Surface::pixels() { return impl_->pixels(); }

std::unique_ptr<GeSurface> make_ge_surface(const bool prefer_gpu,
                                           const std::uint32_t width,
                                           const std::uint32_t height) {
    if (prefer_gpu) {
        auto gpu = std::make_unique<GeD3D11Surface>(width, height);
        if (gpu->available()) return gpu;
    }
    return std::make_unique<GeSoftwareSurface>(width, height);
}

} // namespace fuc::renderer::psp

#else

namespace fuc::renderer::psp {
struct GeD3D11Surface::Impl {};
GeD3D11Surface::GeD3D11Surface(std::uint32_t, std::uint32_t)
    : impl_(std::make_unique<Impl>()) {}
GeD3D11Surface::~GeD3D11Surface() = default;
bool GeD3D11Surface::available() const noexcept { return false; }
SurfaceDrawStatus GeD3D11Surface::draw(const PrimitiveDraw&,
    std::span<const std::uint8_t>, std::span<const std::uint8_t>,
    std::span<const std::uint8_t>) { return SurfaceDrawStatus::invalid_vertex_data; }
std::uint32_t GeD3D11Surface::width() const noexcept { return 0u; }
std::uint32_t GeD3D11Surface::height() const noexcept { return 0u; }
void GeD3D11Surface::finish_frame() {}
std::span<const std::uint32_t> GeD3D11Surface::pixels() { return {}; }
std::unique_ptr<GeSurface> make_ge_surface(bool, std::uint32_t width,
                                           std::uint32_t height) {
    return std::make_unique<GeSoftwareSurface>(width, height);
}
} // namespace fuc::renderer::psp

#endif

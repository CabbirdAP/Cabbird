#include "cabbird/liquid_glass.hpp"

#include "cabbird/cabbird_ui_theme.hpp"

#include <imgui_internal.h>
#include <backends/imgui_impl_dx11.h>

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <d3dcommon.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <unordered_map>
#include <vector>

// Shader bytecode is compiled by fxc at build time; Present never compiles HLSL.
// Adapted from Anomaly's Liquid Glass renderer, retaining only the D3D11 backend.
#include "cabbird/shaders/kGlassBlurPs.h"
#include "cabbird/shaders/kGlassDownsamplePs.h"
#include "cabbird/shaders/kGlassFullScreenVs.h"
#include "cabbird/shaders/kGlassSurfacePs.h"
#include "cabbird/shaders/kGlassSurfaceVs.h"

namespace cabbird::glass {
namespace {

using Microsoft::WRL::ComPtr;

struct alignas(16) GlassConstants final {
    float screen[4]{};
    float rect[4]{};
    float shape[4]{};
    float tint[4]{};
    float optics[4]{};
    float light[4]{};
    float shadow[4]{};
    float extra[4]{};
    float edge[4]{};
    float sector[4]{};  // ring center, mid angle, half aperture; zero aperture = box
    float ring[4]{};    // inner radius, outer radius
};

struct alignas(16) PassConstants final {
    float texel[4]{};
    float pass[4]{};
};

struct RenderTarget final {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> view;
    ComPtr<ID3D11ShaderResourceView> resource;
    UINT width{};
    UINT height{};
};

struct Renderer final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> full_screen;
    ComPtr<ID3D11VertexShader> quad;
    ComPtr<ID3D11PixelShader> downsample;
    ComPtr<ID3D11PixelShader> blur;
    ComPtr<ID3D11PixelShader> glass;
    ComPtr<ID3D11Buffer> glass_constants;
    ComPtr<ID3D11Buffer> pass_constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> opaque;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> pass_rasterizer;
    ComPtr<ID3D11DepthStencilState> depth_off;
    ComPtr<ID3D11Texture2D> backdrop;
    ComPtr<ID3D11ShaderResourceView> backdrop_view;
    UINT backdrop_width{};
    UINT backdrop_height{};
    DXGI_FORMAT backdrop_format{DXGI_FORMAT_UNKNOWN};
    RenderTarget half;
    RenderTarget half_scratch;
    RenderTarget quarter;
    RenderTarget scratch;
    int captured_frame{-1};
    std::array<ComPtr<ID3D11Texture2D>, 4> desktop_layer;
    std::array<ComPtr<ID3D11ShaderResourceView>, 3> desktop_views;
    bool desktop_sharp{};
    bool desktop_blur{};
};

std::unique_ptr<Renderer> g_renderer;
bool g_reduce_transparency{};
// Hosts that should keep the palette tint (the shells) leave this on; a neutral
// host such as the launcher turns it off.
bool g_palette_tint{true};
bool g_reduce_motion{};

enum class CommandKind : std::uint8_t { Capture, Surface };

struct Command final {
    CommandKind kind{CommandKind::Surface};
    int frame{};
    bool clip{true};
    bool blur{true};
    bool deep{false};
    GlassConstants constants;
    ImRect sampled_region;
    ImRect blurred_region;
    bool bounded_copy{};
};

// The same schedule drives both backends and the calculation of each filter's input pixels.
enum BackdropLevel : std::size_t { Sharp, Half, HalfScratch, Quarter, QuarterScratch, LevelCount };
struct BlurPass final {
    BackdropLevel source;
    BackdropLevel target;
    float dx, dy, spread;  // zero spread selects the 2x2 downsample
};
constexpr BlurPass kBlurPasses[]{
    {Sharp, Half, 0, 0, 0}, {Half, HalfScratch, 1, 0, 1}, {HalfScratch, Half, 0, 1, 1},
    {Half, Quarter, 0, 0, 0},
    {Quarter, QuarterScratch, 1, 0, 1.2f}, {QuarterScratch, Quarter, 0, 1, 1.2f},
    {Quarter, QuarterScratch, 1, 0, 1.8f}, {QuarterScratch, Quarter, 0, 1, 1.8f},
    {Quarter, QuarterScratch, 1, 0, 2.6f}, {QuarterScratch, Quarter, 0, 1, 2.6f},
    {Quarter, QuarterScratch, 1, 0, 3.8f}, {QuarterScratch, Quarter, 0, 1, 3.8f},
};
struct Extent final { UINT width, height; };
struct CapturePlan final {
    RECT copy{};
    std::array<RECT, std::size(kBlurPasses)> passes{};
    std::size_t count{};
};

bool HasPixels(const RECT& r) noexcept { return r.left < r.right && r.top < r.bottom; }

// Project the destination's pixel edges into the source texture. Keep the viewport and
// UVs at their original sizes, including for odd dimensions. Only the scissor changes.
RECT PixelBounds(const ImRect& area, const Extent from, const Extent to,
    const float border_x = 0.0f, const float border_y = 0.0f) noexcept {
    if (area.Min.x >= area.Max.x || area.Min.y >= area.Max.y) return {};
    const float sx = static_cast<float>(to.width) / static_cast<float>(from.width);
    const float sy = static_cast<float>(to.height) / static_cast<float>(from.height);
    const auto pixel = [](const float value, const UINT limit) {
        return static_cast<LONG>(std::clamp(value, 0.0f, static_cast<float>(limit)));
    };
    return {pixel(std::floor(area.Min.x * sx - border_x), to.width),
        pixel(std::floor(area.Min.y * sy - border_y), to.height),
        pixel(std::ceil(area.Max.x * sx + border_x), to.width),
        pixel(std::ceil(area.Max.y * sy + border_y), to.height)};
}

CapturePlan PlanCapture(const Command& command, const Extent full, const Extent half,
    const Extent quarter) noexcept {
    CapturePlan plan;
    const Extent sizes[LevelCount]{full, half, half, quarter, quarter};
    plan.count = command.blur ? (command.deep ? std::size(kBlurPasses) : 8) : 0;
    if (!command.bounded_copy) {
        plan.copy = {0, 0, static_cast<LONG>(full.width), static_cast<LONG>(full.height)};
        for (std::size_t i = 0; i < plan.count; ++i) {
            const Extent target = sizes[kBlurPasses[i].target];
            plan.passes[i] = {0, 0, static_cast<LONG>(target.width), static_cast<LONG>(target.height)};
        }
        return plan;
    }
    RECT needed[LevelCount]{};
    needed[Sharp] = PixelBounds(command.sampled_region, full, full);
    if (plan.count) {
        // The surface shader samples each level with a bilinear sampler.
        needed[Half] = PixelBounds(command.blurred_region, full, half, 1, 1);
        needed[Quarter] = PixelBounds(command.blurred_region, full, quarter, 1, 1);
    }
    for (std::size_t i = plan.count; i-- > 0;) {
        const BlurPass& pass = kBlurPasses[i];
        const RECT region = needed[pass.target];
        plan.passes[i] = region;
        needed[pass.target] = {}; // An earlier write to this ping-pong level has different readers.
        if (!HasPixels(region)) continue;
        // ps_down samples +/-1 texel on both axes. ps_blur's outermost sample is
        // 3.23077 * spread on one axis. Include the footprint of linear filtering.
        constexpr float outer_tap = 3.231f;
        const float x = pass.spread ? outer_tap * pass.spread * pass.dx : 1.0f;
        const float y = pass.spread ? outer_tap * pass.spread * pass.dy : 1.0f;
        const RECT reads = PixelBounds(ImRect(static_cast<float>(region.left), static_cast<float>(region.top),
            static_cast<float>(region.right), static_cast<float>(region.bottom)),
            sizes[pass.target], sizes[pass.source], x + 1.0f, y + 1.0f);
        UnionRect(&needed[pass.source], &needed[pass.source], &reads);
    }
    plan.copy = needed[Sharp];
    return plan;
}


// Callback payloads live here until the frame that recorded them has been
// rendered; the next frame's first command recycles the storage. A deque keeps
// the addresses stable while it grows.
std::deque<Command> g_commands;
std::size_t g_command_count{};
int g_commands_frame{-1};

struct WindowSurface final {
    int frame{};
    SurfaceOptions options;
    float alpha{1.0f};
};

std::unordered_map<ImGuiID, WindowSurface> g_window_surfaces;
std::vector<std::unique_ptr<ImDrawList>> g_underlays;

Command& NewCommand() {
    const int frame = ImGui::GetFrameCount();
    if (frame != g_commands_frame) {
        g_command_count = 0;
        g_commands_frame = frame;
    }
    if (g_command_count == g_commands.size()) g_commands.emplace_back();
    Command& command = g_commands[g_command_count++];
    command = {};
    command.frame = frame;
    return command;
}

bool CreateTarget(ID3D11Device* device, RenderTarget& target,
    const UINT width, const UINT height) noexcept {
    if (target.texture != nullptr && target.width == width && target.height == height) {
        return true;
    }
    target = {};
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &target.texture)) ||
        FAILED(device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.view)) ||
        FAILED(device->CreateShaderResourceView(
            target.texture.Get(), nullptr, &target.resource))) {
        target = {};
        return false;
    }
    target.width = width;
    target.height = height;
    return true;
}

bool EnsureTargets(Renderer& renderer, const D3D11_TEXTURE2D_DESC& source) noexcept {
    if (renderer.backdrop == nullptr || renderer.backdrop_width != source.Width ||
        renderer.backdrop_height != source.Height ||
        renderer.backdrop_format != source.Format) {
        renderer.backdrop.Reset();
        renderer.backdrop_view.Reset();
        D3D11_TEXTURE2D_DESC description{};
        description.Width = source.Width;
        description.Height = source.Height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = source.Format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(renderer.device->CreateTexture2D(
                &description, nullptr, &renderer.backdrop)) ||
            FAILED(renderer.device->CreateShaderResourceView(
                renderer.backdrop.Get(), nullptr, &renderer.backdrop_view))) {
            renderer.backdrop.Reset();
            renderer.backdrop_view.Reset();
            return false;
        }
        renderer.backdrop_width = source.Width;
        renderer.backdrop_height = source.Height;
        renderer.backdrop_format = source.Format;
    }
    const UINT half_width = (std::max)(1U, (source.Width + 1U) / 2U);
    const UINT half_height = (std::max)(1U, (source.Height + 1U) / 2U);
    const UINT quarter_width = (std::max)(1U, (source.Width + 3U) / 4U);
    const UINT quarter_height = (std::max)(1U, (source.Height + 3U) / 4U);
    return CreateTarget(renderer.device.Get(), renderer.half, half_width, half_height) &&
        CreateTarget(renderer.device.Get(), renderer.half_scratch, half_width, half_height) &&
        CreateTarget(renderer.device.Get(), renderer.quarter, quarter_width, quarter_height) &&
        CreateTarget(renderer.device.Get(), renderer.scratch, quarter_width, quarter_height);
}

template <typename T>
void Upload(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const T& value) noexcept {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &value, sizeof(T));
        context->Unmap(buffer, 0);
    }
}

void CaptureCallback(const ImDrawList*, const ImDrawCmd* draw_command) {
    const auto* command = static_cast<const Command*>(draw_command->UserCallbackData);
    if (command == nullptr) return;
    Renderer* const renderer = g_renderer.get();
    if (renderer == nullptr) return;
    ID3D11DeviceContext* const context = renderer->context.Get();
    ComPtr<ID3D11RenderTargetView> output;
    ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(1, &output, &depth);
    if (output == nullptr) return;
    ComPtr<ID3D11Resource> resource;
    output->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (resource == nullptr || FAILED(resource.As(&texture))) return;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.SampleDesc.Count != 1 || !EnsureTargets(*renderer, description)) return;

    const CapturePlan plan = PlanCapture(*command, {description.Width, description.Height},
        {renderer->half.width, renderer->half.height}, {renderer->quarter.width, renderer->quarter.height});
    if (!HasPixels(plan.copy)) return;
    ID3D11ShaderResourceView* const unbound[3]{};
    context->PSSetShaderResources(0, 3, unbound);
    const RECT& r = plan.copy;
    if (r.left == 0 && r.top == 0 && r.right == static_cast<LONG>(description.Width) &&
        r.bottom == static_cast<LONG>(description.Height)) {
        context->CopyResource(renderer->backdrop.Get(), texture.Get());
    } else {
        const D3D11_BOX box{static_cast<UINT>(r.left), static_cast<UINT>(r.top), 0,
            static_cast<UINT>(r.right), static_cast<UINT>(r.bottom), 1};
        context->CopySubresourceRegion(renderer->backdrop.Get(), 0, box.left, box.top, 0, texture.Get(), 0, &box);
    }
    renderer->desktop_sharp = false;
    if (!command->blur) {
        renderer->captured_frame = command->frame;
        return;
    }

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(renderer->full_screen.Get(), nullptr, 0);
    context->PSSetSamplers(0, 1, renderer->sampler.GetAddressOf());
    const float blend_factor[4]{};
    context->OMSetBlendState(renderer->opaque.Get(), blend_factor, 0xffffffffU);
    context->OMSetDepthStencilState(renderer->depth_off.Get(), 0);
    context->RSSetState(renderer->pass_rasterizer.Get());
    context->PSSetConstantBuffers(0, 1, renderer->pass_constants.GetAddressOf());
    const RenderTarget* const levels[LevelCount]{nullptr, &renderer->half, &renderer->half_scratch,
        &renderer->quarter, &renderer->scratch};
    for (std::size_t i = 0; i < plan.count; ++i) {
        if (!HasPixels(plan.passes[i])) continue;
        const BlurPass& pass = kBlurPasses[i];
        const RenderTarget* source = levels[pass.source];
        const RenderTarget& target = *levels[pass.target];
        ID3D11ShaderResourceView* srv = source ? source->resource.Get() : renderer->backdrop_view.Get();
        context->PSSetShaderResources(0, 3, unbound);
        context->OMSetRenderTargets(1, target.view.GetAddressOf(), nullptr);
        const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(target.width),
            static_cast<float>(target.height), 0.0f, 1.0f};
        context->RSSetViewports(1, &viewport);
        PassConstants constants;
        constants.texel[0] = 1.0f / static_cast<float>(source ? source->width : description.Width);
        constants.texel[1] = 1.0f / static_cast<float>(source ? source->height : description.Height);
        constants.texel[2] = pass.dx;
        constants.texel[3] = pass.dy;
        constants.pass[0] = pass.spread;
        Upload(context, renderer->pass_constants.Get(), constants);
        context->PSSetShader(pass.spread ? renderer->blur.Get() : renderer->downsample.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &srv);
        context->RSSetScissorRects(1, &plan.passes[i]);
        context->Draw(3, 0);
    }
    context->PSSetShaderResources(0, 3, unbound);
    context->OMSetRenderTargets(1, output.GetAddressOf(), depth.Get());
    renderer->captured_frame = command->frame;
    renderer->desktop_blur = false;
}

void SurfaceCallback(const ImDrawList*, const ImDrawCmd* draw_command) {
    const auto* const recorded = static_cast<const Command*>(draw_command->UserCallbackData);
    if (recorded == nullptr) return;
    Renderer* const renderer = g_renderer.get();
    const Command* const command = recorded;
    if (renderer == nullptr) return;
    ID3D11DeviceContext* const context = renderer->context.Get();
    UINT viewport_count = 1;
    D3D11_VIEWPORT viewport{};
    context->RSGetViewports(&viewport_count, &viewport);
    if (viewport_count == 0 || viewport.Width <= 0.0f || viewport.Height <= 0.0f) return;
    GlassConstants constants = command->constants;
    constants.screen[0] = viewport.Width;
    constants.screen[1] = viewport.Height;
    constants.screen[2] = 1.0f / viewport.Width;
    constants.screen[3] = 1.0f / viewport.Height;
    const bool backdrop_ready = renderer->captured_frame == command->frame &&
        renderer->backdrop_view != nullptr &&
        static_cast<float>(renderer->backdrop_width) == viewport.Width &&
        static_cast<float>(renderer->backdrop_height) == viewport.Height;
    if (!backdrop_ready) {
        // Nothing was captured for this frame: draw the tint alone rather than
        // refracting a stale picture.
        constants.tint[3] = 1.0f;
        constants.light[3] = 1.0f;
    }
    Upload(context, renderer->glass_constants.Get(), constants);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(renderer->quad.Get(), nullptr, 0);
    context->PSSetShader(renderer->glass.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, renderer->glass_constants.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, renderer->glass_constants.GetAddressOf());
    ID3D11ShaderResourceView* resources[3]{};
    if (backdrop_ready) {
        resources[0] = renderer->desktop_sharp ? renderer->desktop_views[0].Get() : renderer->backdrop_view.Get();
        resources[1] = renderer->desktop_blur ? renderer->desktop_views[2].Get() : renderer->quarter.resource.Get();
        resources[2] = renderer->desktop_blur ? renderer->desktop_views[1].Get() : renderer->half.resource.Get();
    }
    context->PSSetShaderResources(0, 3, resources);
    context->PSSetSamplers(0, 1, renderer->sampler.GetAddressOf());
    const float blend_factor[4]{};
    context->OMSetBlendState(renderer->blend.Get(), blend_factor, 0xffffffffU);
    D3D11_RECT scissor{0, 0, static_cast<LONG>(viewport.Width),
        static_cast<LONG>(viewport.Height)};
    if (command->clip) {
        scissor.left = (std::max)(0L, static_cast<LONG>(draw_command->ClipRect.x));
        scissor.top = (std::max)(0L, static_cast<LONG>(draw_command->ClipRect.y));
        scissor.right = (std::min)(scissor.right, static_cast<LONG>(draw_command->ClipRect.z));
        scissor.bottom = (std::min)(scissor.bottom, static_cast<LONG>(draw_command->ClipRect.w));
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) return;
    }
    context->RSSetScissorRects(1, &scissor);
    context->Draw(4, 0);
    ID3D11ShaderResourceView* const unbound[3]{};
    context->PSSetShaderResources(0, 3, unbound);
}


// ---------------------------------------------------------------------------
ImVec4 Mix(const ImVec4& from, const ImVec4& to, const float amount) noexcept {
    return {from.x + (to.x - from.x) * amount, from.y + (to.y - from.y) * amount,
        from.z + (to.z - from.z) * amount, from.w + (to.w - from.w) * amount};
}

ImVec4 ToVec4(const cabbird::CabbirdUiColor& color) noexcept {
    return {color.red, color.green, color.blue, color.alpha};
}

struct Look final {
    ImVec4 tint{};
    float tint_strength{};
    float refraction{};
    float chroma{};
    float blur_mix{1.0f};
    float saturation{1.0f};
    float rim{1.0f};
    float lift{};
    float minimum_alpha{};
    float shadow_alpha{};
    float shadow_offset{};
    float shadow_blur{};
    float gloss{};
    float bevel{12.0f};
    float glow{};
    float magnify{};
    float contact{};
    // How much the inner shoulder darkens, mostly on the side facing away
    // from the light. This is what gives a pane its thickness.
    float shade{};
    // The sharpest blur the shoulder may sample. A deeply frosted face needs a
    // shoulder that is still soft, or the bevel turns into a hard ring inside
    // the pane.
    float rim_blur{0.25f};
    float fallback_alpha{0.9f};
};

// Over a flat wallpaper there is little for refraction to bend, so the glass
// has to be carried by its edge: the rim line, the glow inside the bevel and
// the gloss on the upper half. Shadows stay faint and only floating panes get
// one at all.
Look ResolveLook(const SurfaceOptions& options) noexcept {
    const bool dark = DarkAppearance();
    const ImVec4 surface = ToVec4(cabbird::CabbirdUiTheme().window_background);
    Look look;
    look.tint = !g_palette_tint
        ? (dark ? ImVec4(0.10f, 0.10f, 0.10f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f))
        : (dark ? Mix(surface, ImVec4(0.0f, 0.0f, 0.0f, 1.0f), 0.15f)
                : Mix(surface, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.70f));
    look.tint.w = 1.0f;
    // blur_mix is the face blur: 0 sharp, 1 light, 2 heavy.
    switch (options.variant) {
    case Variant::Regular:
        look.tint_strength = dark ? 0.26f : 0.12f;
        look.refraction = 40.0f;
        look.chroma = 0.14f;
        look.blur_mix = 1.45f;
        look.saturation = 1.80f;
        look.rim = dark ? 0.75f : 1.0f;
        look.lift = dark ? 0.01f : 0.03f;
        look.minimum_alpha = 0.90f;
        look.shadow_alpha = dark ? 0.20f : 0.05f;
        look.shadow_offset = 3.0f;
        look.shadow_blur = 14.0f;
        look.gloss = dark ? 0.03f : 0.07f;
        look.bevel = 26.0f;
        look.glow = dark ? 0.12f : 0.22f;
        look.magnify = 0.0f;
        look.contact = dark ? 0.20f : 0.07f;
        look.shade = dark ? 0.16f : 0.10f;
        look.fallback_alpha = 0.84f;
        break;
    case Variant::Clear:
        look.tint_strength = 0.02f;
        look.refraction = 22.0f;
        look.chroma = 0.20f;
        look.blur_mix = 0.15f;
        look.saturation = 1.25f;
        look.rim = 1.0f;
        look.lift = 0.02f;
        look.minimum_alpha = 0.0f;
        look.shadow_alpha = dark ? 0.18f : 0.06f;
        look.shadow_offset = 2.0f;
        look.shadow_blur = 6.0f;
        look.gloss = 0.10f;
        look.bevel = 12.0f;
        look.glow = 0.24f;
        look.magnify = 0.08f;
        look.contact = 0.10f;
        look.shade = 0.10f;
        look.fallback_alpha = 0.35f;
        break;
    case Variant::Material:
        look.tint_strength = dark ? 0.38f : 0.24f;
        look.refraction = 26.0f;
        look.chroma = 0.08f;
        look.blur_mix = 1.85f;
        look.saturation = 1.50f;
        look.rim = dark ? 0.55f : 0.75f;
        look.lift = dark ? 0.01f : 0.05f;
        look.minimum_alpha = 0.94f;
        look.shadow_alpha = 0.0f;
        look.gloss = dark ? 0.02f : 0.04f;
        look.bevel = 20.0f;
        look.glow = dark ? 0.08f : 0.14f;
        look.magnify = 0.0f;
        look.contact = dark ? 0.16f : 0.06f;
        look.shade = dark ? 0.12f : 0.07f;
        look.fallback_alpha = 0.90f;
        break;
    case Variant::Thumb:
        // The selection lens. It is drawn under its label, so it can bend and
        // magnify the glass it sits on freely: a bright two-sided rim, a glow
        // in the lit corner, a shaded far shoulder and a gloss on the face.
        look.tint = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        look.tint_strength = dark ? 0.10f : 0.30f;
        look.refraction = 16.0f;
        look.chroma = 0.30f;
        look.blur_mix = 0.0f;
        look.saturation = 1.30f;
        look.rim = dark ? 0.90f : 1.20f;
        look.lift = dark ? 0.03f : 0.10f;
        look.minimum_alpha = 0.0f;
        look.shadow_alpha = 0.0f;
        look.gloss = dark ? 0.06f : 0.18f;
        look.bevel = 12.0f;
        look.glow = dark ? 0.25f : 0.45f;
        look.magnify = 0.05f;
        look.contact = 0.0f;
        look.shade = dark ? 0.18f : 0.12f;
        look.fallback_alpha = 0.55f;
        break;
    }
    if (!g_palette_tint) {
        // A neutral host frosts without a colour cast: the veil keeps the shipped
        // transparency and only the hue changes (no palette tint, no saturation
        // boost), so the pane stays as see-through as it always was.
        look.saturation = 1.0f;
    }
    if (options.tint.w > 0.0f) {
        look.tint = Mix(look.tint, ImVec4(options.tint.x, options.tint.y, options.tint.z, 1.0f),
            options.tint.w);
        look.tint.w = 1.0f;
    }
    if (options.tint_strength >= 0.0f) look.tint_strength = options.tint_strength;
    if (options.saturation >= 0.0f) look.saturation = options.saturation;
    if (options.minimum_alpha >= 0.0f) look.minimum_alpha = options.minimum_alpha;
    if (options.face_blur >= 0.0f) look.blur_mix = options.face_blur;
    if (options.rim_blur >= 0.0f) look.rim_blur = options.rim_blur;
    if (!options.rim) {
        look.rim = 0.0f;
        look.contact = 0.0f;
    }
    if (options.refraction_scale > 0.0f) look.refraction *= options.refraction_scale;
    if (!options.shadow) {
        look.shadow_alpha = 0.0f;
        look.contact = 0.0f;
    }
    return look;
}

ImU32 Color(const ImVec4& color, const float alpha) noexcept {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, color.w * alpha));
}

void DrawFallback(ImDrawList* list, const ImVec2 min, const ImVec2 max,
    const SurfaceOptions& options, const Look& look) noexcept {
    const float alpha = ImGui::GetStyle().Alpha;
    const float scale = options.scale;
    const float rounding = (std::min)(options.rounding * 1.0f,
        (std::min)(max.x - min.x, max.y - min.y) * 0.5f);
    if (!options.clip) list->PushClipRect(ImVec2(-16384.0f, -16384.0f),
        ImVec2(16384.0f, 16384.0f), false);
    if (look.shadow_alpha > 0.0f) {
        constexpr int kLayers = 5;
        for (int layer = 0; layer < kLayers; ++layer) {
            const float grow = look.shadow_blur * scale *
                static_cast<float>(layer + 1) / static_cast<float>(kLayers) * 0.6f;
            const float layer_alpha = look.shadow_alpha * 0.30f *
                (1.0f - static_cast<float>(layer) / static_cast<float>(kLayers));
            list->AddRectFilled(
                ImVec2(min.x - grow, min.y - grow + look.shadow_offset * scale),
                ImVec2(max.x + grow, max.y + grow + look.shadow_offset * scale),
                Color(ImVec4(0.0f, 0.0f, 0.0f, 1.0f), layer_alpha * alpha), rounding + grow);
        }
    }
    const float fill = g_reduce_transparency ? 0.98f : look.fallback_alpha;
    list->AddRectFilled(min, max, Color(look.tint, fill * alpha), rounding);
    const bool dark = DarkAppearance();
    list->AddRect(ImVec2(min.x + 0.5f, min.y + 0.5f), ImVec2(max.x - 0.5f, max.y - 0.5f),
        Color(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), (dark ? 0.12f : 0.70f) * look.rim * alpha),
        rounding, 0, (std::max)(1.0f, scale));
    if (!options.clip) list->PopClipRect();
}

}  // namespace

bool InitializeRenderer(ID3D11Device* device, ID3D11DeviceContext* context,
    const bool palette_tint) noexcept {
    ShutdownRenderer();
    if (device == nullptr || context == nullptr) return false;
    try {
        auto renderer = std::make_unique<Renderer>();
        renderer->device = device;
        renderer->context = context;
        g_palette_tint = palette_tint;
        if (FAILED(device->CreateVertexShader(kGlassFullScreenVs, sizeof(kGlassFullScreenVs),
                nullptr, &renderer->full_screen)) ||
            FAILED(device->CreateVertexShader(kGlassSurfaceVs, sizeof(kGlassSurfaceVs),
                nullptr, &renderer->quad)) ||
            FAILED(device->CreatePixelShader(kGlassDownsamplePs, sizeof(kGlassDownsamplePs),
                nullptr, &renderer->downsample)) ||
            FAILED(device->CreatePixelShader(kGlassBlurPs, sizeof(kGlassBlurPs),
                nullptr, &renderer->blur)) ||
            FAILED(device->CreatePixelShader(kGlassSurfacePs, sizeof(kGlassSurfacePs),
                nullptr, &renderer->glass))) {
            return false;
        }
        D3D11_BUFFER_DESC buffer{};
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        buffer.ByteWidth = sizeof(GlassConstants);
        if (FAILED(device->CreateBuffer(&buffer, nullptr, &renderer->glass_constants))) return false;
        buffer.ByteWidth = sizeof(PassConstants);
        if (FAILED(device->CreateBuffer(&buffer, nullptr, &renderer->pass_constants))) return false;

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sampler, &renderer->sampler))) return false;

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&blend, &renderer->opaque))) return false;
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        if (FAILED(device->CreateBlendState(&blend, &renderer->blend))) return false;

        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        raster.ScissorEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&raster, &renderer->pass_rasterizer))) return false;

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if (FAILED(device->CreateDepthStencilState(&depth, &renderer->depth_off))) return false;
        g_renderer = std::move(renderer);
        return true;
    } catch (...) {
        return false;
    }
}


void ShutdownRenderer() noexcept {
    g_renderer.reset();
    g_commands.clear();
    g_commands_frame = -1;
    g_window_surfaces.clear();
    g_underlays.clear();
}

bool RendererAvailable() noexcept { return g_renderer != nullptr; }

bool Active() noexcept { return RendererAvailable() && !g_reduce_transparency; }

void SetReduceTransparency(const bool enabled) noexcept { g_reduce_transparency = enabled; }

bool ReduceTransparency() noexcept { return g_reduce_transparency; }

void SetReduceMotion(const bool enabled) noexcept { g_reduce_motion = enabled; }

bool ReduceMotion() noexcept { return g_reduce_motion; }

bool DarkAppearance() noexcept {
    const auto surface = cabbird::CabbirdUiTheme().window_background;
    return 0.2126f * surface.red + 0.7152f * surface.green + 0.0722f * surface.blue < 0.45f;
}

void CaptureBackdrop(ImDrawList* list, const bool blur, const bool deep) noexcept {
    if (list == nullptr || !Active()) return;
    try {
        Command& command = NewCommand();
        command.kind = CommandKind::Capture;
        command.blur = blur;
        command.deep = blur && deep;
        list->AddCallback(CaptureCallback, &command);
        list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    } catch (...) {
    }
}

namespace {

// Records one surface over `min`..`max`; `sector` and `ring` shape it as a ring
// sector when the aperture in sector[3] is positive.
void RecordSurface(ImDrawList* list, const ImVec2 min, const ImVec2 max,
    const SurfaceOptions& options, const Look& look, const float (&sector)[4],
    const float (&ring)[4]) noexcept {
    try {
        Command& command = NewCommand();
        command.kind = CommandKind::Surface;
        command.clip = options.clip;
        GlassConstants& constants = command.constants;
        std::copy(std::begin(sector), std::end(sector), std::begin(constants.sector));
        std::copy(std::begin(ring), std::end(ring), std::begin(constants.ring));
        const float scale = options.scale > 0.0f ? options.scale : 1.0f;
        const float margin = look.shadow_alpha > 0.0f
            ? (look.shadow_blur + look.shadow_offset) * scale + 2.0f : 2.0f * scale;
        constants.rect[0] = min.x;
        constants.rect[1] = min.y;
        constants.rect[2] = max.x;
        constants.rect[3] = max.y;
        constants.shape[0] = options.rounding;
        constants.shape[1] = scale;
        constants.shape[2] = margin;
        constants.shape[3] = ImGui::GetStyle().Alpha;
        constants.tint[0] = look.tint.x;
        constants.tint[1] = look.tint.y;
        constants.tint[2] = look.tint.z;
        constants.tint[3] = look.tint_strength;
        constants.optics[0] = look.refraction;
        constants.optics[1] = look.chroma;
        constants.optics[2] = look.blur_mix;
        constants.optics[3] = look.saturation;
        constants.light[0] = look.rim;
        constants.light[1] = look.lift;
        constants.light[2] = DarkAppearance() ? 1.0f : 0.0f;
        constants.light[3] = look.minimum_alpha;
        constants.shadow[0] = look.shadow_alpha;
        constants.shadow[1] = look.shadow_offset * scale;
        constants.shadow[2] = look.shadow_blur * scale;
        constants.shadow[3] = look.gloss;
        constants.extra[0] = look.bevel;
        constants.extra[1] = look.glow;
        constants.extra[2] = look.magnify;
        constants.extra[3] = look.contact;
        constants.edge[0] = look.shade;
        constants.edge[1] = look.rim_blur;
        constants.edge[2] = (std::max)(0.0f, options.edge_feather) * scale;
        list->AddCallback(SurfaceCallback, &command);
        list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    } catch (...) {
    }
}

}  // namespace

void DrawSurface(ImDrawList* list, const ImVec2 min, const ImVec2 max,
    const SurfaceOptions& options) noexcept {
    if (list == nullptr || max.x - min.x < 1.0f || max.y - min.y < 1.0f) return;
    const Look look = ResolveLook(options);
    if (!Active()) {
        DrawFallback(list, min, max, options, look);
        return;
    }
    RecordSurface(list, min, max, options, look, {}, {});
}

void DrawSectorSurface(ImDrawList* list, const ImVec2 center, const float inner_radius,
    const float outer_radius, const float angle_start, const float angle_end,
    const SurfaceOptions& options) noexcept {
    constexpr float kPi = 3.14159265358979323846f;
    const float span = angle_end - angle_start;
    if (list == nullptr || !(inner_radius >= 0.0f) || !(outer_radius - inner_radius >= 1.0f) ||
        !(span > 0.0f) || !(span < kPi)) {
        return;
    }
    const Look look = ResolveLook(options);
    if (!Active()) {
        const int segments = (std::max)(4, static_cast<int>(span * outer_radius / 6.0f));
        if (!options.clip) list->PushClipRect(ImVec2(-16384.0f, -16384.0f),
            ImVec2(16384.0f, 16384.0f), false);
        list->PathArcTo(center, outer_radius, angle_start, angle_end, segments);
        list->PathArcTo(center, inner_radius, angle_end, angle_start, segments);
        const float fill = g_reduce_transparency ? 0.98f : look.fallback_alpha;
        list->PathFillConcave(Color(look.tint, fill * ImGui::GetStyle().Alpha));
        if (!options.clip) list->PopClipRect();
        return;
    }
    // The quad only has to cover the shape: bound both arcs, including any
    // axis extreme the span crosses.
    ImVec2 min(FLT_MAX, FLT_MAX);
    ImVec2 max(-FLT_MAX, -FLT_MAX);
    const auto include = [&](const float radius, const float angle) {
        const ImVec2 point(center.x + radius * std::cos(angle), center.y + radius * std::sin(angle));
        min = ImVec2((std::min)(min.x, point.x), (std::min)(min.y, point.y));
        max = ImVec2((std::max)(max.x, point.x), (std::max)(max.y, point.y));
    };
    include(inner_radius, angle_start);
    include(inner_radius, angle_end);
    include(outer_radius, angle_start);
    include(outer_radius, angle_end);
    for (int quarter = -8; quarter <= 8; ++quarter) {
        const float angle = static_cast<float>(quarter) * kPi * 0.5f;
        if (angle > angle_start && angle < angle_end) include(outer_radius, angle);
    }
    const float sector[4]{center.x, center.y, (angle_start + angle_end) * 0.5f, span * 0.5f};
    const float ring[4]{inner_radius, outer_radius, 0.0f, 0.0f};
    RecordSurface(list, min, max, options, look, sector, ring);
}

void RegisterWindowSurface(const SurfaceOptions& options) noexcept {
    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    if (window == nullptr) return;
    ImGuiWindow* const root = window->RootWindow != nullptr ? window->RootWindow : window;
    try {
        g_window_surfaces[root->ID] = {
            ImGui::GetFrameCount(), options, ImGui::GetStyle().Alpha};
    } catch (...) {
    }
}

namespace {

void BoundFrameCaptures(const ImDrawData& data) noexcept {
    // Both glass backends use framebuffer pixels. Other viewport transforms retain full captures.
    if (data.DisplayPos.x != 0 || data.DisplayPos.y != 0 ||
        data.FramebufferScale.x != 1 || data.FramebufferScale.y != 1) return;
    const ImRect screen(ImVec2(0, 0), data.DisplaySize);
    const ImRect empty(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    ImRect sharp = empty, blurred = empty;
    for (int i = data.CmdLists.Size - 1; i >= 0; --i) {
        const auto& draws = data.CmdLists[i]->CmdBuffer;
        for (int j = draws.Size - 1; j >= 0; --j) {
            const ImDrawCmd& draw = draws[j];
            if (draw.UserCallback == CaptureCallback) {
                auto& capture = *static_cast<Command*>(draw.UserCallbackData);
                capture.bounded_copy = true;
                capture.sampled_region = sharp;
                sharp = empty;
                // A thumb replaces just the sharp copy. Later frosted controls can still
                // read the half and quarter textures produced by the preceding blur.
                if (capture.blur) {
                    capture.blurred_region = blurred;
                    blurred = empty;
                }
            } else if (draw.UserCallback == SurfaceCallback) {
                const auto& c = static_cast<const Command*>(draw.UserCallbackData)->constants;
                // Magnification pulls towards a point inside the lens bounds, including for
                // ring sectors. Add the feather, refracted RGB taps and their linear footprint.
                const float reach = (std::max)(1.0f, c.edge[2]) * 0.5f + 1.0f +
                    std::abs(c.optics[0]) * c.shape[1] * (1.0f + std::abs(c.optics[1]));
                ImRect lens(c.rect[0] - reach, c.rect[1] - reach, c.rect[2] + reach, c.rect[3] + reach);
                lens.ClipWith(screen);
                if (lens.GetWidth() <= 0 || lens.GetHeight() <= 0) continue;
                sharp.Add(lens);
                if (c.optics[2] > 0.0f) blurred.Add(lens);
            } else if (draw.UserCallback && draw.UserCallback != ImDrawCallback_ResetRenderState) {
                sharp = blurred = screen;
            }
        }
    }
}

} // namespace

void PrepareDrawData(ImDrawData* data, const bool retain_desktop_layer) noexcept {
    if (data == nullptr || ImGui::GetCurrentContext() == nullptr || !Active()) return;
    try {
        ImGuiContext& context = *GImGui;
        const int frame = ImGui::GetFrameCount();
        std::size_t used = 0;
        for (ImGuiWindow* const window : context.Windows) {
            if (window == nullptr || !window->Active || window->Hidden ||
                window->DrawList == nullptr) {
                continue;
            }
            if ((window->Flags & ImGuiWindowFlags_ChildWindow) != 0 &&
                (window->Flags & ImGuiWindowFlags_Popup) == 0) {
                continue;
            }
            SurfaceOptions options;
            float surface_alpha = context.Style.Alpha;
            const auto registered = g_window_surfaces.find(window->ID);
            if (registered != g_window_surfaces.end() && registered->second.frame == frame) {
                options = registered->second.options;
                surface_alpha = registered->second.alpha;
            } else if ((window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) != 0) {
                options.rounding = (window->Flags & ImGuiWindowFlags_Tooltip) != 0
                    ? context.Style.PopupRounding * 0.75f : context.Style.PopupRounding;
                options.scale = ImGui::GetIO().FontGlobalScale > 0.0f
                    ? ImGui::GetIO().FontGlobalScale : 1.0f;
                options.clip = false;
            } else {
                continue;
            }
            const int index = data->CmdLists.find_index(window->DrawList);
            if (index < 0) continue;
            if (used == g_underlays.size()) {
                g_underlays.push_back(
                    std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData()));
            }
            ImDrawList* const underlay = g_underlays[used++].get();
            underlay->_ResetForNewFrame();
            underlay->PushClipRectFullScreen();
            underlay->PushTextureID(ImGui::GetIO().Fonts->TexID);
            CaptureBackdrop(underlay, true, options.deep_backdrop);
            // Window bodies are recorded after End/Render. Restore the alpha
            // that was active at registration instead of the popped style.
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, surface_alpha);
            DrawSurface(underlay, window->Pos, window->Rect().Max, options);
            ImGui::PopStyleVar();
            underlay->_PopUnusedDrawCmd();
            data->CmdLists.insert(data->CmdLists.Data + index, underlay);
            data->CmdListsCount = data->CmdLists.Size;
            data->TotalVtxCount += underlay->VtxBuffer.Size;
            data->TotalIdxCount += underlay->IdxBuffer.Size;
        }
        std::erase_if(g_window_surfaces,
            [frame](const auto& entry) { return entry.second.frame < frame - 1; });
        if (!retain_desktop_layer && g_commands_frame == frame) BoundFrameCaptures(*data);
    } catch (...) {
    }
}

void RenderDrawData(ID3D11DeviceContext* context, ImDrawData* data,
    ID3D11RenderTargetView* target) noexcept {
    if (context == nullptr || data == nullptr) return;
    // A hidden UI has no lists to submit. Leave the game's pipeline untouched;
    // callback-only lists still need submission even when they have no vertices.
    if (data->CmdListsCount == 0 ||
        data->DisplaySize.x <= 0.0f || data->DisplaySize.y <= 0.0f) return;
    // Save once per submission, rather than issuing COM queries for every
    // card. Preserve outputs before binding the overlay: DX11 automatically
    // clears any SRV of the same texture when it becomes a render target.
    struct MaterialBindings final {
        enum : UINT { kResources = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, kUavs = 64 }; // D3D11.1; feature level 11.0 uses eight.
        ID3D11DeviceContext* context;
        ID3D11Buffer* constants{};
        ID3D11HullShader* hull{};
        ID3D11DomainShader* domain{};
        ID3D11ComputeShader* compute{};
        ID3D11ClassInstance* instances[3][D3D11_SHADER_MAX_INTERFACES]{};
        UINT instance_counts[3]{D3D11_SHADER_MAX_INTERFACES,
            D3D11_SHADER_MAX_INTERFACES, D3D11_SHADER_MAX_INTERFACES};
        ID3D11ShaderResourceView* resources[6][kResources]{};
        ID3D11RenderTargetView* outputs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* depth{};
        ID3D11UnorderedAccessView* output_uavs[kUavs]{};
        ID3D11UnorderedAccessView* compute_uavs[kUavs]{};
        UINT output_count{};
        UINT uav_count{};
        bool full_inputs{};

        MaterialBindings(ID3D11DeviceContext* value, const bool bind_target) noexcept
            : context(value), full_inputs(bind_target) {
            // ImGui's DX11 backend clears HS/DS/CS during setup but only
            // backs up VS/PS/GS. Preserve dynamic-linkage instances as well.
            context->HSGetShader(&hull, instances[0], &instance_counts[0]);
            context->DSGetShader(&domain, instances[1], &instance_counts[1]);
            context->CSGetShader(&compute, instances[2], &instance_counts[2]);
            context->PSGetConstantBuffers(0, 1, &constants);
            context->PSGetShaderResources(0, full_inputs ? kResources : 3, resources[0]);
            if (full_inputs) {
                context->VSGetShaderResources(0, kResources, resources[1]);
                context->GSGetShaderResources(0, kResources, resources[2]);
                context->HSGetShaderResources(0, kResources, resources[3]);
                context->DSGetShaderResources(0, kResources, resources[4]);
                context->CSGetShaderResources(0, kResources, resources[5]);
            }
            context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                outputs, &depth);
            for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
                if (outputs[i] != nullptr) output_count = i + 1;
            }
            ID3D11Device* device{};
            context->GetDevice(&device);
            const auto level = device->GetFeatureLevel();
            device->Release();
            uav_count = level >= D3D_FEATURE_LEVEL_11_1 ? kUavs :
                level >= D3D_FEATURE_LEVEL_11_0 ? D3D11_PS_CS_UAV_REGISTER_COUNT : 0;
            if (uav_count != 0) {
                context->OMGetRenderTargetsAndUnorderedAccessViews(
                    0, nullptr, nullptr, 0, uav_count, output_uavs);
                context->CSGetUnorderedAccessViews(0, uav_count, compute_uavs);
            }
        }
        ~MaterialBindings() {
            // Bind original outputs together before inputs. Preserve UAV
            // append/consume counters, and use the actual RTV count: binding
            // eight slots (even null ones) would silently clear pixel UAVs.
            UINT counters[kUavs];
            std::fill(std::begin(counters), std::end(counters), UINT(-1));
            if (uav_count != 0) {
                if (output_count < uav_count) {
                    context->OMSetRenderTargetsAndUnorderedAccessViews(output_count,
                        outputs, depth, output_count, uav_count - output_count,
                        output_uavs + output_count, counters);
                } else {
                    context->OMSetRenderTargets(output_count, outputs, depth);
                }
                context->CSSetUnorderedAccessViews(0, uav_count, compute_uavs, counters);
            } else {
                context->OMSetRenderTargets(output_count, outputs, depth);
            }
            context->HSSetShader(hull, instances[0], instance_counts[0]);
            context->DSSetShader(domain, instances[1], instance_counts[1]);
            context->CSSetShader(compute, instances[2], instance_counts[2]);
            context->PSSetConstantBuffers(0, 1, &constants);
            context->PSSetShaderResources(0, full_inputs ? kResources : 3, resources[0]);
            if (full_inputs) {
                context->VSSetShaderResources(0, kResources, resources[1]);
                context->GSSetShaderResources(0, kResources, resources[2]);
                context->HSSetShaderResources(0, kResources, resources[3]);
                context->DSSetShaderResources(0, kResources, resources[4]);
                context->CSSetShaderResources(0, kResources, resources[5]);
            }
            if (hull != nullptr) hull->Release();
            if (domain != nullptr) domain->Release();
            if (compute != nullptr) compute->Release();
            for (auto& stage : instances)
                for (auto* instance : stage) if (instance != nullptr) instance->Release();
            if (constants != nullptr) constants->Release();
            for (auto& stage : resources)
                for (auto* resource : stage) if (resource != nullptr) resource->Release();
            for (auto* output : outputs) if (output != nullptr) output->Release();
            for (auto* uav : output_uavs) if (uav != nullptr) uav->Release();
            for (auto* uav : compute_uavs) if (uav != nullptr) uav->Release();
            if (depth != nullptr) depth->Release();
        }
    } bindings(context, target != nullptr);
    if (target != nullptr) context->OMSetRenderTargets(1, &target, nullptr);
    try {
        ImGui_ImplDX11_RenderDrawData(data);
    } catch (...) {
        // A draw-list callback must not unwind into the game's Present chain.
    }
}

bool AppendDrawListSnapshot(const ImDrawList& list, std::vector<std::byte>& bytes) {
    const auto append = [&bytes](auto values) {
        const auto data = std::as_bytes(values);
        if (!data.empty()) bytes.insert(bytes.end(), data.begin(), data.end());
    };
    const int sizes[]{list.VtxBuffer.Size, list.IdxBuffer.Size, list.CmdBuffer.Size};
    append(std::span{sizes});
    append(std::span{list.VtxBuffer.Data, static_cast<std::size_t>(list.VtxBuffer.Size)});
    append(std::span{list.IdxBuffer.Data, static_cast<std::size_t>(list.IdxBuffer.Size)});
    for (const ImDrawCmd& draw : list.CmdBuffer) {
        // ImGui zero-initializes the command header. It contains the clip, texture, offsets,
        // element count and callback identity, ending just before its transient data pointer.
        append(std::as_bytes(std::span{&draw, 1}).first(offsetof(ImDrawCmd, UserCallbackData)));
        if (draw.UserCallback == nullptr || draw.UserCallback == ImDrawCallback_ResetRenderState) continue;
        if (draw.UserCallback != CaptureCallback && draw.UserCallback != SurfaceCallback) return false;
        const auto& command = *static_cast<const Command*>(draw.UserCallbackData);
        if (draw.UserCallback == CaptureCallback) {
            const bool flags[]{command.blur, command.deep};
            append(std::span{flags});
        } else {
            append(std::span{&command.clip, 1});
            append(std::span{&command.constants, 1});
        }
    }
    return true;
}

bool SaveDesktopLayer(ID3D11RenderTargetView* target) noexcept {
    Renderer* const renderer = g_renderer.get();
    if (!renderer || !target || renderer->captured_frame != ImGui::GetFrameCount()) return false;
    ComPtr<ID3D11Resource> color;
    target->GetResource(&color);
    const std::array<ID3D11Resource*, 4> inputs{
        color.Get(), renderer->backdrop.Get(), renderer->half.texture.Get(), renderer->quarter.texture.Get()};
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        static_cast<ID3D11Texture2D*>(inputs[i])->GetDesc(&desc);
        auto& saved = renderer->desktop_layer[i];
        D3D11_TEXTURE2D_DESC previous{};
        if (saved) saved->GetDesc(&previous);
        if (previous.Width != desc.Width || previous.Height != desc.Height || previous.Format != desc.Format) {
            desc.BindFlags = i == 0 ? 0 : D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = 0;
            saved.Reset();
            if (FAILED(renderer->device->CreateTexture2D(&desc, nullptr, &saved))) return false;
            if (i != 0 && FAILED(renderer->device->CreateShaderResourceView(
                    saved.Get(), nullptr, renderer->desktop_views[i - 1].ReleaseAndGetAddressOf()))) {
                saved.Reset();
                return false;
            }
        }
        renderer->context->CopyResource(saved.Get(), inputs[i]);
    }
    return true;
}

void RestoreDesktopLayer(ID3D11RenderTargetView* target, const RECT& area) noexcept {
    Renderer& renderer = *g_renderer;
    ComPtr<ID3D11Resource> color;
    target->GetResource(&color);
    const D3D11_BOX box{static_cast<UINT>(area.left), static_cast<UINT>(area.top), 0,
        static_cast<UINT>(area.right), static_cast<UINT>(area.bottom), 1};
    renderer.context->CopySubresourceRegion(color.Get(), 0, box.left, box.top, 0,
        renderer.desktop_layer[0].Get(), 0, &box);
    renderer.desktop_sharp = true;
    renderer.desktop_blur = true;
    renderer.captured_frame = ImGui::GetFrameCount();
}

ImRect BoundDesktopCaptures(const ImDrawData& data) noexcept {
    const ImRect screen(data.DisplayPos, ImVec2(data.DisplayPos.x + data.DisplaySize.x,
        data.DisplayPos.y + data.DisplaySize.y));
    ImRect bounds(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    Command* capture = nullptr;
    for (const ImDrawList* list : data.CmdLists) {
        for (const ImDrawCmd& draw : list->CmdBuffer) {
            if (draw.UserCallback == CaptureCallback) {
                auto& next = *static_cast<Command*>(draw.UserCallbackData);
                next.bounded_copy = !next.blur;
                next.sampled_region = ImRect(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
                capture = next.blur ? nullptr : &next;
                if (next.blur) bounds = screen;
            } else if (draw.UserCallback == SurfaceCallback) {
                const auto& c = static_cast<const Command*>(draw.UserCallbackData)->constants;
                if (c.sector[3] > 0.0f) {
                    if (capture) capture->bounded_copy = false;
                    capture = nullptr;
                    bounds = screen;
                    continue;
                }
                // A rectangular lens magnifies toward its own centre. Only refraction and
                // chromatic separation can sample beyond the quad. Include the linear filter footprint.
                const float reach = c.shape[2] + std::abs(c.optics[0]) * c.shape[1] *
                    (1.0f + std::abs(c.optics[1])) + 1.0f;
                const ImRect lens(c.rect[0] - reach, c.rect[1] - reach,
                    c.rect[2] + reach, c.rect[3] + reach);
                if (capture) capture->sampled_region.Add(lens);
                bounds.Add(lens);
            } else if (draw.UserCallback == nullptr) {
                if (draw.ElemCount) bounds.Add(ImRect(draw.ClipRect));
            } else if (draw.UserCallback != ImDrawCallback_ResetRenderState) {
                if (capture) capture->bounded_copy = false;
                capture = nullptr;
                bounds = screen;
            }
        }
    }
    bounds.ClipWith(screen);
    return bounds;
}

void DrawThumb(ImDrawList* list, const ImVec2 min, const ImVec2 max, const float rounding,
    const float scale, const bool accent) noexcept {
    if (list == nullptr || max.x - min.x < 1.0f || max.y - min.y < 1.0f) return;
    const auto& theme = cabbird::CabbirdUiTheme();
    SurfaceOptions options;
    options.variant = Variant::Thumb;
    options.rounding = rounding;
    options.scale = scale;
    options.clip = true;
    if (accent) {
        options.tint = ImVec4(theme.accent.red, theme.accent.green, theme.accent.blue, 0.75f);
        options.tint_strength = DarkAppearance() ? 0.18f : 0.22f;
    }
    if (Active()) {
        // The lens refracts what is already drawn, its own label included.
        CaptureBackdrop(list, false);
        DrawSurface(list, min, max, options);
        return;
    }
    const ImVec4 fill = accent
        ? ImVec4(theme.accent.red, theme.accent.green, theme.accent.blue,
              g_reduce_transparency ? 0.34f : 0.26f)
        : ImVec4(1.0f, 1.0f, 1.0f, DarkAppearance() ? 0.16f : 0.80f);
    list->AddRectFilled(min, max, ImGui::GetColorU32(fill), rounding);
    list->AddRect(ImVec2(min.x + 0.5f, min.y + 0.5f), ImVec2(max.x - 0.5f, max.y - 0.5f),
        ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, DarkAppearance() ? 0.14f : 0.65f)),
        rounding, 0, (std::max)(1.0f, scale));
}

ImRect AnimateThumb(const ImGuiID id, const ImRect& target, const ImVec2 origin) noexcept {
    const ImVec2 target_center = target.GetCenter();
    const float x = Animate(id, target_center.x - origin.x, 18.0f);
    const float y = Animate(id ^ 0x632be5abU, target_center.y - origin.y, 18.0f);
    const float width = Animate(id ^ 0x85ebca6bU, target.GetWidth(), 18.0f);
    const float height = Animate(id ^ 0xc2b2ae35U, target.GetHeight(), 18.0f);
    const ImVec2 center(origin.x + x, origin.y + y);
    return ImRect(ImVec2(center.x - width * 0.5f, center.y - height * 0.5f),
        ImVec2(center.x + width * 0.5f, center.y + height * 0.5f));
}

float Animate(const ImGuiID id, const float target, const float speed) noexcept {
    ImGuiStorage* const storage = ImGui::GetStateStorage();
    if (storage == nullptr) return target;
    const ImGuiID key = ImHashStr("##glass-animation", 0, id);
    float* const value = storage->GetFloatRef(key, target);
    if (value == nullptr) return target;
    if (g_reduce_motion) {
        *value = target;
        return target;
    }
    const float delta = (std::max)(0.0f, ImGui::GetIO().DeltaTime);
    *value += (target - *value) * (1.0f - std::exp(-speed * delta));
    if (std::fabs(target - *value) < 0.002f) *value = target;
    return *value;
}

ImVec2 SwitchSize() noexcept {
    const float height = std::round(ImGui::GetFrameHeight() * 0.96f);
    return {std::round(height * 1.66f), height};
}

void DrawSwitch(ImDrawList* list, const ImVec2 min, const ImVec2 max, const float on,
    const float press, const bool hovered, const float scale) noexcept {
    if (list == nullptr) return;
    const auto& theme = cabbird::CabbirdUiTheme();
    const bool dark = DarkAppearance();
    const float height = max.y - min.y;
    const float width = max.x - min.x;
    const float radius = height * 0.5f;
    const ImVec4 accent = ControlAccent();
    const ImVec4 off = dark ? ImVec4(1.0f, 1.0f, 1.0f, hovered ? 0.22f : 0.17f)
                            : ImVec4(0.47f, 0.47f, 0.50f, hovered ? 0.30f : 0.24f);
    const ImVec4 track = Mix(off, ImVec4(accent.x, accent.y, accent.z, 1.0f), on);
    list->AddRectFilled(min, max, ImGui::GetColorU32(track), radius);
    // A light accent needs an edge of its own to read against a light card.
    const float accent_luminance = 0.2126f * accent.x + 0.7152f * accent.y + 0.0722f * accent.z;
    if (!dark && accent_luminance > 0.55f && on > 0.0f) {
        const ImVec4 edge = Mix(ImVec4(accent.x, accent.y, accent.z, 1.0f),
            ImVec4(0.0f, 0.0f, 0.0f, 1.0f), 0.30f);
        list->AddRect(ImVec2(min.x + 0.5f, min.y + 0.5f), ImVec2(max.x - 0.5f, max.y - 0.5f),
            ImGui::GetColorU32(ImVec4(edge.x, edge.y, edge.z, 0.55f * on)), radius, 0,
            (std::max)(1.0f, scale));
    }

    const float inset = (std::max)(2.0f, std::round(height * 0.09f));
    const float knob_radius = radius - inset;
    const float stretch = press * height * 0.30f;
    const float travel = width - inset * 2.0f - knob_radius * 2.0f - stretch;
    const float knob_left = min.x + inset + travel * on;
    const ImVec2 knob_min(knob_left, min.y + inset);
    const ImVec2 knob_max(knob_left + knob_radius * 2.0f + stretch, max.y - inset);
    if (press > 0.02f && Active()) {
        // While the switch is held its knob becomes a clear lens over the track,
        // the one place Apple puts glass into the content layer. The lens only
        // reads the sharp copy, so it takes the cheap capture: a second blurred
        // chain per frame bought nothing here and cost every frame the switch
        // was held, which is exactly when a dropped frame shows.
        CaptureBackdrop(list, false);
        const float grow = press * height * 0.16f;
        SurfaceOptions lens;
        lens.variant = Variant::Clear;
        lens.rounding = knob_radius + grow;
        lens.scale = scale;
        lens.clip = false;
        DrawSurface(list, ImVec2(knob_min.x - grow, knob_min.y - grow),
            ImVec2(knob_max.x + grow, knob_max.y + grow), lens);
    }
    const float solid = Active() ? 1.0f - press : 1.0f;
    if (solid > 0.0f) {
        for (int layer = 0; layer < 3; ++layer) {
            const float grow = static_cast<float>(layer) * 0.8f * scale;
            list->AddRectFilled(ImVec2(knob_min.x - grow, knob_min.y - grow + scale),
                ImVec2(knob_max.x + grow, knob_max.y + grow + 1.5f * scale),
                ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.07f * solid)),
                knob_radius + grow);
        }
        list->AddRectFilled(knob_min, knob_max,
            ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, solid)), knob_radius);
    }
}

bool Switch(const char* label, bool* value) noexcept {
    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    if (window == nullptr || window->SkipItems || value == nullptr) return false;
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 size = SwitchSize();
    const float height = (std::max)(ImGui::GetFrameHeight(), size.y);
    const ImVec2 position = window->DC.CursorPos;
    const ImRect bounds(position, ImVec2(position.x + size.x +
        (label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f),
        position.y + height));
    ImGui::ItemSize(bounds, style.FramePadding.y);
    if (!ImGui::ItemAdd(bounds, id)) return false;
    bool hovered{};
    bool held{};
    const bool pressed = ImGui::ButtonBehavior(bounds, id, &hovered, &held);
    if (pressed) {
        *value = !*value;
        ImGui::MarkItemEdited(id);
    }
    const float top = position.y + (height - size.y) * 0.5f;
    const ImRect track(ImVec2(position.x, top), ImVec2(position.x + size.x, top + size.y));
    const float on = Animate(id, *value ? 1.0f : 0.0f, 15.0f);
    const float press = Animate(id ^ 0x5bd1e995U, held && hovered ? 1.0f : 0.0f, 22.0f);
    ImGui::RenderNavCursor(track, id);
    const float scale = ImGui::GetIO().FontGlobalScale > 0.0f ? ImGui::GetIO().FontGlobalScale : 1.0f;
    DrawSwitch(window->DrawList, track.Min, track.Max, on, press, hovered, scale);
    if (label_size.x > 0.0f) {
        ImGui::RenderText(ImVec2(track.Max.x + style.ItemInnerSpacing.x,
            position.y + (height - label_size.y) * 0.5f), label);
    }
    return pressed;
}

float SegmentedControlWidth(
    const std::span<const char* const> labels, const SegmentedOptions& options) noexcept {
    const float scale = options.scale;
    float width = 3.0f * scale * 2.0f;
    for (const char* const label : labels) {
        width += ImGui::CalcTextSize(label, nullptr, true).x + options.padding_x * scale * 2.0f;
    }
    return width;
}

// Corner radius as a fraction of height. Apple's controls use continuous
// rounded rectangles; a full capsule reads as exaggerated at these sizes.
constexpr float kSegmentRounding = 0.34f;

bool SegmentedControl(const char* id, const std::span<const char* const> labels, int& selected,
    const SegmentedOptions& options) noexcept {
    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    if (window == nullptr || window->SkipItems || labels.empty()) return false;
    const float scale = options.scale;
    const float height = options.height * scale;
    const float inset = 3.0f * scale;
    const float padding = options.padding_x * scale;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = SegmentedControlWidth(labels, options);
    ImDrawList* const list = window->DrawList;
    const bool dark = DarkAppearance();
    if (options.glass_track) {
        SurfaceOptions track;
        track.variant = Variant::Regular;
        track.rounding = height * kSegmentRounding;
        track.scale = scale;
        track.shadow = false;
        if (g_palette_tint) track.tint_strength = DarkAppearance() ? 0.30f : 0.20f;
        DrawSurface(list, origin, ImVec2(origin.x + width, origin.y + height), track);
    } else {
        list->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
            ImGui::GetColorU32(dark ? ImVec4(1.0f, 1.0f, 1.0f, 0.07f)
                                    : ImVec4(0.47f, 0.47f, 0.50f, 0.13f)),
            height * kSegmentRounding);
    }

    ImGui::PushID(id);
    const ImGuiID base = window->GetID("##segmented");
    std::vector<float> starts(labels.size());
    std::vector<float> widths(labels.size());
    float x = origin.x + inset;
    for (std::size_t index = 0; index < labels.size(); ++index) {
        starts[index] = x;
        widths[index] = ImGui::CalcTextSize(labels[index], nullptr, true).x + padding * 2.0f;
        x += widths[index];
    }
    const int clamped = std::clamp(selected, 0, static_cast<int>(labels.size()) - 1);
    const std::size_t current_index = static_cast<std::size_t>(clamped);
    const float indicator_rounding = (height - inset * 2.0f) * kSegmentRounding;
    {
        // The lens goes down first so the labels stay crisp on top of it. It
        // swells a little while the control is held, like a pressed droplet.
        const bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() &&
            ImGui::IsMouseHoveringRect(origin, ImVec2(origin.x + width, origin.y + height));
        const float press = Animate(base ^ 0x1b873593U, held ? 1.0f : 0.0f, 20.0f);
        const ImRect target(ImVec2(starts[current_index], origin.y + inset),
            ImVec2(starts[current_index] + widths[current_index], origin.y + height - inset));
        ImRect thumb = AnimateThumb(base, target, origin);
        const float grow = press * 2.0f * scale;
        thumb.Expand(ImVec2(grow * 1.5f, grow));
        DrawThumb(list, thumb.Min, thumb.Max, indicator_rounding + grow, scale, true);
    }
    bool changed = false;
    const ImVec2 cursor = ImGui::GetCursorPos();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    for (std::size_t index = 0; index < labels.size(); ++index) {
        const bool disabled = index < 32 && (options.disabled_mask & (1U << index)) != 0;
        ImGui::SetCursorScreenPos(ImVec2(starts[index], origin.y + inset));
        ImGui::PushID(static_cast<int>(index));
        ImGui::BeginDisabled(disabled);
        const bool clicked = ImGui::InvisibleButton(
            "##segment", ImVec2(widths[index], height - inset * 2.0f));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::EndDisabled();
        ImGui::PopID();
        if (clicked && !disabled &&
            (selected != static_cast<int>(index) || options.activate_selected)) {
            selected = static_cast<int>(index);
            changed = true;
        }
        const char* const label = labels[index];
        const char* const label_end = ImGui::FindRenderedTextEnd(label);
        const ImVec2 label_size = ImGui::CalcTextSize(label, label_end);
        const bool current = static_cast<int>(index) == clamped;
        ImVec4 ink = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ink.w *= disabled ? 0.35f : current ? 1.0f : hovered ? 0.92f : 0.72f;
        list->AddText(ImVec2(starts[index] + (widths[index] - label_size.x) * 0.5f,
                origin.y + (height - label_size.y) * 0.5f),
            ImGui::GetColorU32(ink), label, label_end);
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    ImGui::SetCursorPos(cursor);
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
    return changed;
}

namespace {

constexpr int kTabBarStyleColors = 6;
constexpr int kTabBarStyleVars = 4;

void PushTabBarStyle() noexcept {
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Tab, clear);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, clear);
    ImGui::PushStyleColor(ImGuiCol_TabSelected, clear);
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, clear);
    ImGui::PushStyleColor(ImGuiCol_TabDimmed, clear);
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, clear);
    const float height = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, height * kSegmentRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBarOverlineSize, 0.0f);
}

void PopTabBarStyle() noexcept {
    ImGui::PopStyleVar(kTabBarStyleVars);
    ImGui::PopStyleColor(kTabBarStyleColors);
}

float CurrentScale() noexcept {
    const float scale = ImGui::GetIO().FontGlobalScale;
    return scale > 0.0f ? scale : 1.0f;
}

}  // namespace

bool BeginTabBar(const char* id, const ImGuiTabBarFlags flags) noexcept {
    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    if (window == nullptr || window->SkipItems) return false;
    const float scale = CurrentScale();
    const float inset = 3.0f * scale;
    // The bar sits inside a glass capsule as wide as its tabs were last frame;
    // the first frame of a new bar has no width yet and simply skips the track.
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + inset, ImGui::GetCursorPosY() + inset));
    const ImGuiID bar_id = window->GetID(id);
    const ImGuiTabBar* const previous = GImGui->TabBars.GetByKey(bar_id);
    const ImVec2 origin = window->DC.CursorPos;
    const float height = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
    if (previous != nullptr && previous->WidthAllTabs > 0.0f) {
        const float available = window->WorkRect.Max.x - origin.x;
        const float width = (std::min)(previous->WidthAllTabs, available);
        // Plugin windows have their own glass body, so the track refracts that.
        CaptureBackdrop(window->DrawList);
        SurfaceOptions track;
        track.variant = Variant::Regular;
        track.rounding = (height + inset * 2.0f) * kSegmentRounding;
        track.scale = scale;
        track.shadow = false;
        if (g_palette_tint) track.tint_strength = DarkAppearance() ? 0.30f : 0.20f;
        DrawSurface(window->DrawList, ImVec2(origin.x - inset, origin.y - inset),
            ImVec2(origin.x + width + inset, origin.y + height + inset), track);
        for (const ImGuiTabItem& tab : previous->Tabs) {
            if (tab.ID != previous->SelectedTabId || tab.Width <= 0.0f) continue;
            const float left = origin.x + tab.Offset - previous->ScrollingAnim;
            const ImRect target(ImVec2(left, origin.y), ImVec2(left + tab.Width, origin.y + height));
            const ImRect thumb = AnimateThumb(bar_id ^ 0x7f4a7c15U, target, origin);
            window->DrawList->PushClipRect(ImVec2(origin.x - inset, origin.y - inset),
                ImVec2(origin.x + width + inset, origin.y + height + inset), true);
            DrawThumb(window->DrawList, thumb.Min, thumb.Max, height * kSegmentRounding,
                scale, true);
            window->DrawList->PopClipRect();
            break;
        }
    }
    PushTabBarStyle();
    if (!ImGui::BeginTabBar(id, flags)) {
        PopTabBarStyle();
        return false;
    }
    return true;
}

void EndTabBar() noexcept {
    ImGui::EndTabBar();
    PopTabBarStyle();
    ImGui::Dummy(ImVec2(0.0f, 3.0f * CurrentScale()));
}

ImVec2 GlyphInkCenter(ImFont* font, const float size, const char* glyph) noexcept {
    if (font == nullptr || glyph == nullptr || *glyph == '\0') return ImVec2(0.0f, 0.0f);
    unsigned int codepoint = 0;
    ImTextCharFromUtf8(&codepoint, glyph, nullptr);
    const ImFontGlyph* const found = font->FindGlyphNoFallback(static_cast<ImWchar>(codepoint));
    if (found == nullptr || font->FontSize <= 0.0f) {
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, glyph);
        return ImVec2(extent.x * 0.5f, extent.y * 0.5f);
    }
    const float k = size / font->FontSize;
    return ImVec2((found->X0 + found->X1) * 0.5f * k, (found->Y0 + found->Y1) * 0.5f * k);
}

bool SliderScalar(const char* label, const ImGuiDataType data_type, void* value,
    const void* minimum, const void* maximum, const char* format,
    const ImGuiSliderFlags flags) noexcept {
    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    if (window == nullptr || window->SkipItems || value == nullptr) return false;
    ImGuiContext& context = *GImGui;
    const ImGuiStyle& style = context.Style;
    const ImGuiID id = window->GetID(label);
    const float width = ImGui::CalcItemWidth();
    const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    const float height = ImGui::GetFrameHeight();
    const ImVec2 position = window->DC.CursorPos;
    const ImRect frame(position, ImVec2(position.x + width, position.y + height));
    const ImRect total(frame.Min, ImVec2(frame.Max.x +
        (label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f), frame.Max.y));
    const bool input_allowed = (flags & ImGuiSliderFlags_NoInput) == 0;
    ImGui::ItemSize(total, style.FramePadding.y);
    if (!ImGui::ItemAdd(total, id, &frame, input_allowed ? ImGuiItemFlags_Inputable : 0)) {
        return false;
    }
    if (format == nullptr) format = ImGui::DataTypeGetInfo(data_type)->PrintFmt;
    const bool hovered = ImGui::ItemHoverable(frame, id, context.LastItemData.ItemFlags);
    bool typing = input_allowed && ImGui::TempInputIsActive(id);
    if (!typing) {
        const bool clicked = hovered && ImGui::IsMouseClicked(0, ImGuiInputFlags_None, id);
        const bool activate = clicked || context.NavActivateId == id;
        if (activate && clicked) ImGui::SetKeyOwner(ImGuiKey_MouseLeft, id);
        if (activate && input_allowed &&
            ((clicked && context.IO.KeyCtrl) ||
                (context.NavActivateId == id &&
                    (context.NavActivateFlags & ImGuiActivateFlags_PreferInput) != 0))) {
            typing = true;
        }
        if (activate && !typing) {
            ImGui::SetActiveID(id, window);
            ImGui::SetFocusID(id, window);
            ImGui::FocusWindow(window);
            context.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
        }
    }
    if (typing) {
        const bool clamp = (flags & ImGuiSliderFlags_ClampOnInput) != 0;
        return ImGui::TempInputScalar(frame, id, label, data_type, value, format,
            clamp ? minimum : nullptr, clamp ? maximum : nullptr);
    }

    // The value column is as wide as the widest value the slider can show.
    char text[64];
    const auto format_value = [&] {
        return text + ImGui::DataTypeFormatString(text, IM_ARRAYSIZE(text), data_type, value, format);
    };
    const char* text_end = format_value();
    float value_width = ImGui::CalcTextSize(text, text_end).x;
    for (const void* bound : {minimum, maximum}) {
        char bound_text[64];
        const int length = ImGui::DataTypeFormatString(
            bound_text, IM_ARRAYSIZE(bound_text), data_type, bound, format);
        value_width = (std::max)(value_width, ImGui::CalcTextSize(bound_text, bound_text + length).x);
    }
    const float scale = CurrentScale();
    const float gap = 10.0f * scale;
    const float knob = std::round(height * 0.66f);
    const ImRect track(frame.Min, ImVec2((std::max)(frame.Min.x + knob * 2.0f,
        frame.Max.x - value_width - gap), frame.Max.y));

    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, knob);
    ImRect grab;
    const bool changed = ImGui::SliderBehavior(
        track, id, data_type, value, minimum, maximum, format, flags, &grab);
    ImGui::PopStyleVar();
    if (changed) {
        ImGui::MarkItemEdited(id);
        text_end = format_value();  // Draw the new value in the same frame as the thumb.
    }

    ImDrawList* const list = window->DrawList;
    const auto& theme = cabbird::CabbirdUiTheme();
    const bool dark = DarkAppearance();
    const bool active = context.ActiveId == id;
    ImGui::RenderNavCursor(frame, id);
    const float center_y = frame.GetCenter().y;
    const float rail = (std::max)(3.0f, std::round(4.0f * scale));
    const float knob_x = grab.Max.x > grab.Min.x ? grab.GetCenter().x : track.Min.x + knob * 0.5f;
    const ImVec2 rail_min(track.Min.x + knob * 0.5f - rail * 0.5f, center_y - rail * 0.5f);
    const ImVec2 rail_max(track.Max.x - knob * 0.5f + rail * 0.5f, center_y + rail * 0.5f);
    list->AddRectFilled(rail_min, rail_max, ImGui::GetColorU32(dark
        ? ImVec4(1.0f, 1.0f, 1.0f, 0.18f) : ImVec4(0.47f, 0.47f, 0.50f, 0.26f)), rail * 0.5f);
    static_cast<void>(theme);
    list->AddRectFilled(rail_min, ImVec2(knob_x, rail_max.y),
        ImGui::GetColorU32(ControlAccent()), rail * 0.5f);
    const float press = Animate(id ^ 0x2545f491U, active ? 1.0f : 0.0f, 22.0f);
    const ImVec2 knob_min(knob_x - knob * 0.5f, center_y - knob * 0.5f);
    const ImVec2 knob_max(knob_x + knob * 0.5f, center_y + knob * 0.5f);
    if (press > 0.02f && Active()) {
        CaptureBackdrop(list, false);
        const float grow = press * knob * 0.22f;
        SurfaceOptions lens;
        lens.variant = Variant::Clear;
        lens.rounding = knob * 0.5f + grow;
        lens.scale = scale;
        lens.clip = false;
        DrawSurface(list, ImVec2(knob_min.x - grow * 1.3f, knob_min.y - grow),
            ImVec2(knob_max.x + grow * 1.3f, knob_max.y + grow), lens);
    }
    const float solid = Active() ? 1.0f - press : 1.0f;
    if (solid > 0.0f) {
        for (int layer = 0; layer < 3; ++layer) {
            const float spread = static_cast<float>(layer) * 0.8f * scale;
            list->AddCircleFilled(ImVec2(knob_x, center_y + scale), knob * 0.5f + spread,
                ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.07f * solid)));
        }
        list->AddCircleFilled(ImVec2(knob_x, center_y), knob * 0.5f,
            ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, solid)));
    }
    const ImVec2 text_size = ImGui::CalcTextSize(text, text_end);
    ImVec4 ink = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (!hovered && !active) ink.w *= 0.72f;
    list->AddText(ImVec2(frame.Max.x - text_size.x, center_y - text_size.y * 0.5f),
        ImGui::GetColorU32(ink), text, text_end);
    if (label_size.x > 0.0f) {
        ImGui::RenderText(ImVec2(frame.Max.x + style.ItemInnerSpacing.x,
            frame.Min.y + style.FramePadding.y), label);
    }
    return changed;
}

bool SliderFloat(const char* label, float* value, const float minimum, const float maximum,
    const char* format, const ImGuiSliderFlags flags) noexcept {
    return SliderScalar(label, ImGuiDataType_Float, value, &minimum, &maximum, format, flags);
}

bool SliderInt(const char* label, int* value, const int minimum, const int maximum,
    const char* format, const ImGuiSliderFlags flags) noexcept {
    return SliderScalar(label, ImGuiDataType_S32, value, &minimum, &maximum, format, flags);
}

ImVec4 ControlAccent() noexcept {
    const auto& accent = cabbird::CabbirdUiTheme().accent;
    return ImVec4(accent.red, accent.green, accent.blue, 1.0f);
}

bool BeginCombo(const char* label, const char* preview, const ImGuiComboFlags flags) noexcept {
    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    if (window == nullptr || window->SkipItems) return false;
    // The frame is measured before the call: once open, the combo makes its
    // popup the current window.
    const ImVec2 position = window->DC.CursorPos;
    const float width = (flags & ImGuiComboFlags_NoPreview) != 0
        ? ImGui::GetFrameHeight() : ImGui::CalcItemWidth();
    const float height = ImGui::GetFrameHeight();
    ImDrawList* const list = window->DrawList;
    const bool open = ImGui::BeginCombo(label, preview, flags | ImGuiComboFlags_NoArrowButton);
    const float scale = CurrentScale();
    const float half = (std::max)(3.0f, height * 0.13f);
    const ImVec2 center(position.x + width - height * 0.5f, position.y + height * 0.5f);
    ImVec4 ink = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ink.w *= 0.70f;
    const float lift = open ? -half * 0.25f : half * 0.25f;
    const float tip = open ? -half * 0.55f : half * 0.55f;
    list->PathLineTo(ImVec2(center.x - half, center.y - tip + lift));
    list->PathLineTo(ImVec2(center.x, center.y + tip + lift));
    list->PathLineTo(ImVec2(center.x + half, center.y - tip + lift));
    list->PathStroke(ImGui::GetColorU32(ink), 0, (std::max)(1.4f, 1.5f * scale));
    return open;
}

bool Combo(const char* label, int* current, const char* const items[], const int count) noexcept {
    if (current == nullptr || items == nullptr || count <= 0) return false;
    const char* const preview = *current >= 0 && *current < count ? items[*current] : "";
    bool changed = false;
    if (BeginCombo(label, preview)) {
        for (int index = 0; index < count; ++index) {
            ImGui::PushID(index);
            const bool selected = index == *current;
            if (ImGui::Selectable(items[index], selected)) {
                changed = changed || !selected;
                *current = index;
            }
            if (selected) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

}  // namespace cabbird::glass

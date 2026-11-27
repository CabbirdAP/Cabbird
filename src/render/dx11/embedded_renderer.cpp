#include "embedded_host_internal.hpp"
#include "cabbird/unity_services.hpp"
#include "cabbird/host_ui_service.hpp"
#include "cabbird/platform_ui_input_policy.hpp"
#include "cabbird/cabbird_ui_theme.hpp"
#include "cabbird/liquid_glass.hpp"
#include "cabbird/structured_logger.hpp"

#include "embedded_ui_resource_render_backend.hpp"

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace cabbird::embedded {
namespace {

// Says why the overlay is not on screen.
//
// Embedded start-up used to be entirely silent: a dozen separate conditions
// could each abandon the frame with a bare `return`, so a user whose overlay
// never appeared had nothing to go on and neither did anyone reading a bug
// report. Every abandoned start-up now names its reason exactly once per
// reason, which keeps a per-frame path from turning into a log flood while
// still surfacing the first cause and any later change of cause.
void ReportEmbeddedGate(
        EmbeddedState& state, const char* reason, const std::string& detail = {},
        const char* prefix = "embedded overlay not started: ") noexcept {
    try {
        const std::string key = std::string(reason) + '|' + detail;
        if (state.reported_gates.contains(key)) return;
        std::string message = std::string(prefix) + reason;
        if (!detail.empty()) message += " (" + detail + ")";
        {
            // The Worker swaps the queue under this lock, then opens/writes
            // the log outside it. A busy queue simply retries next frame.
            std::unique_lock lock(state.diagnostic_mutex, std::try_to_lock);
            if (!lock.owns_lock()) return;
            state.pending_diagnostics.push_back(message);
            state.reported_gates.insert(key);
        }
        if (state.diagnostics.logger != nullptr) {
            cabbird::LogDetails details;
            details.thread_domain = cabbird::LogThreadDomain::Render;
            details.event_id = "embedded.gate";
            static_cast<void>(state.diagnostics.logger->Log(
                cabbird::LogLevel::Warning, "embedded-renderer", message, std::move(details)));
        }
    } catch (...) {
    }
}

void CaptureImGuiSettings(EmbeddedState& state) noexcept {
    if (state.imgui_context == nullptr) return;
    try {
        std::size_t size{};
        const char* const data = ImGui::SaveIniSettingsToMemory(&size);
        const auto previous = state.imgui_ini_snapshot.load(std::memory_order_acquire);
        if (previous == nullptr || std::string_view(*previous) != std::string_view(data, size)) {
            state.imgui_ini_snapshot.store(
                std::make_shared<const std::string>(data, size), std::memory_order_release);
        }
        ImGui::GetIO().WantSaveIniSettings = false;
    } catch (...) {
        // Leave a retry requested if allocation fails; never replace the last
        // good preference snapshot with an incomplete one.
        ImGui::GetIO().WantSaveIniSettings = true;
    }
}

// Drives the overlay from polled device state.
//
// Used only when the game window's procedure could not be exchanged. ImGui
// normally learns about input from window messages, so without that hook the
// overlay would draw but ignore the mouse entirely, which is barely better than
// not drawing at all. Polling is coarser -- no character input, no scroll
// accumulation between frames -- but it restores pointing and clicking, which is
// what the panels actually need.
void FeedPolledInput(const EmbeddedState& state) noexcept {
    if (state.window == nullptr) return;
    ImGuiIO& io = ImGui::GetIO();
    POINT cursor{};
    if (GetCursorPos(&cursor) && ScreenToClient(state.window, &cursor)) {
        io.AddMousePosEvent(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    }
    const bool foreground = GetForegroundWindow() == state.window;
    io.AddFocusEvent(foreground);
    if (!foreground) return;
    struct PolledButton {
        int virtual_key;
        int imgui_button;
    };
    static constexpr std::array<PolledButton, 3> buttons{{
        {VK_LBUTTON, 0}, {VK_RBUTTON, 1}, {VK_MBUTTON, 2}}};
    for (const PolledButton& button : buttons) {
        const bool down = (GetAsyncKeyState(button.virtual_key) & 0x8000) != 0;
        io.AddMouseButtonEvent(button.imgui_button, down);
    }
}

std::string LastErrorDetail(const char* label, unsigned long error) {
    return std::string(label) + "=" + std::to_string(error);
}


class PerformanceTimer final {
public:
    PerformanceTimer(
        EmbeddedPerformanceProbe& probe,
        const EmbeddedPerformanceStage stage,
        const bool active) noexcept
        : probe_(active ? &probe : nullptr), stage_(stage),
          started_(active ? std::chrono::steady_clock::now()
                          : std::chrono::steady_clock::time_point{}) {}

    ~PerformanceTimer() { Stop(); }

    PerformanceTimer(const PerformanceTimer&) = delete;
    PerformanceTimer& operator=(const PerformanceTimer&) = delete;

    void Stop() noexcept {
        if (probe_ == nullptr) return;
        probe_->Record(stage_, std::chrono::steady_clock::now() - started_);
        probe_ = nullptr;
    }

private:
    EmbeddedPerformanceProbe* probe_{};
    EmbeddedPerformanceStage stage_{};
    std::chrono::steady_clock::time_point started_{};
};



float HostDpiScale(const HWND window) noexcept {
    if (window == nullptr || !IsWindow(window)) return 0.0F;
    const UINT dpi = GetDpiForWindow(window);
    return dpi == 0 ? 0.0F : static_cast<float>(dpi) / 96.0F;
}

void SynchronizeHostFontScale(EmbeddedState& state) noexcept {
    if (state.plugins == nullptr) return;
    const float scale = HostDpiScale(state.window);
    if (scale > 0.0F) {
        static_cast<void>(state.plugins->UiResources().SetHostFontScale(scale));
    }
}


constexpr UINT kPixelProbeWidth = 32;
constexpr UINT kPixelProbeHeight = 32;
constexpr UINT64 kMaximumPixelProbeBytes = 1024ULL * 1024ULL;
constexpr std::uint32_t kPixelProbeRetryFrames = 30;

std::uint64_t CaptureGeneration(const EmbeddedState& state) noexcept {
    if (!state.diagnostics.capture_generation) return 0;
    try {
        return state.diagnostics.capture_generation();
    } catch (...) {
        return 0;
    }
}

void NotifyRenderThread(
    const EmbeddedState& state, std::uint64_t capture_generation) noexcept {
    if (!state.diagnostics.render) return;
    try {
        state.diagnostics.render(capture_generation, GetCurrentThreadId());
    } catch (...) {
    }
}
















bool InvalidatePluginUiDevice(EmbeddedState& state) noexcept {
    if (!state.plugin_ui_device_active) return true;
    std::unique_lock plugin_lock(*state.plugin_mutex, std::try_to_lock);
    if (!plugin_lock.owns_lock()) return false;
    if (state.plugins != nullptr) state.plugins->OnUiDeviceLost();
    state.plugin_ui_device_active = false;
    return true;
}

bool RebuildPluginUiDevice(EmbeddedState& state) noexcept {
    if (!state.platform_ui_initialized) return true;
    std::unique_lock plugin_lock(*state.plugin_mutex, std::try_to_lock);
    if (!plugin_lock.owns_lock() || state.plugins == nullptr) return false;
    // Keep the flag set on an acknowledgement failure so ReleaseGraphics can
    // invalidate the generation before destroying the backend objects.
    state.plugin_ui_device_active = true;
    return state.plugins->OnUiDeviceRebuilt();
}




void UpdateMenuCursor(EmbeddedState& state, bool active) {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = active;
    if (active) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    }
    if (active) {
        if (!state.menu_cursor_active) {
            io.ClearEventsQueue();
            io.ClearInputMouse();
            state.previous_capture = GetCapture();
            state.cursor_clip_saved = GetClipCursor(&state.previous_cursor_clip) != FALSE;
            ReleaseCapture();
            state.menu_cursor_active = true;
        }
        ClipCursor(nullptr);
        return;
    }
    if (!state.menu_cursor_active) return;
    io.ClearEventsQueue();
    io.ClearInputKeys();
    io.ClearInputMouse();
    if (state.cursor_clip_saved) ClipCursor(&state.previous_cursor_clip);
    if (state.previous_capture != nullptr && IsWindow(state.previous_capture)) {
        SetCapture(state.previous_capture);
    }
    state.previous_capture = nullptr;
    state.cursor_clip_saved = false;
    state.menu_cursor_active = false;
    // The game owns the native cursor once menu capture ends. Synthesizing a
    // cursor message here races its WndProc and causes visible flicker.
}

std::uint64_t SwapChainArea(IDXGISwapChain* swap_chain) {
    DXGI_SWAP_CHAIN_DESC description{};
    if (swap_chain == nullptr || FAILED(swap_chain->GetDesc(&description))) return 0;
    RECT rectangle{};
    if (description.OutputWindow == nullptr ||
        !GetClientRect(description.OutputWindow, &rectangle)) return 0;
    const auto width = (std::max)(0L, rectangle.right - rectangle.left);
    const auto height = (std::max)(0L, rectangle.bottom - rectangle.top);
    return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
}

bool PreferSwapChain(const EmbeddedState& state, IDXGISwapChain* candidate) {
    if (state.source_swap_chain == candidate) return false;
    if (state.window == nullptr || !IsWindow(state.window) || !IsWindowVisible(state.window)) return true;
    const std::uint64_t candidate_area = SwapChainArea(candidate);
    return candidate_area > state.selected_area + state.selected_area / 4;
}

// Names the graphics API behind a swap chain.
//
// The Present hook lives in DXGI, which every Direct3D version shares, so it
// fires for a D3D11 swap chain exactly as it does for a D3D12 one. Everything
// downstream of it here is D3D12 only. When the device query fails there is no
// way to tell "wrong API" from "device query failed" without asking, so ask.
std::string DescribeSwapChainApi(IDXGISwapChain* swap_chain) {
    if (swap_chain == nullptr) return "swapChain=null";
    std::string description = "api=";
    void* probe{};
    const auto query = [&](const IID& id) {
        probe = nullptr;
        return SUCCEEDED(swap_chain->GetDevice(id, &probe)) && probe != nullptr;
    };
    if (query(IID{0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}})) {
        // ID3D11Device, spelled out so this file does not have to pull in d3d11.h.
        description += "d3d11";
    } else if (query(IID{0x9b7e4c0f, 0x342c, 0x4106, {0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0}})) {
        // ID3D10Device1.
        description += "d3d10";
    } else {
        description += "unknown";
    }
    if (probe != nullptr) static_cast<IUnknown*>(probe)->Release();
    DXGI_SWAP_CHAIN_DESC layout{};
    if (SUCCEEDED(swap_chain->GetDesc(&layout))) {
        description += " buffers=" + std::to_string(layout.BufferCount) +
            " format=" + std::to_string(static_cast<int>(layout.BufferDesc.Format)) +
            " windowed=" + std::to_string(layout.Windowed ? 1 : 0);
    }
    return description;
}

bool CompleteGraphicsInitialization(EmbeddedState& state);


// Everything after the device objects exist, which is identical whichever
// Direct3D version owns the back buffer. The resource backend serves both, so
// plugin fonts and icons work the same way on each.
bool CompleteGraphicsInitialization(EmbeddedState& state) {
    if (!InstallEmbeddedInput(state)) {
        // Losing the window procedure costs interactivity, not the overlay.
        // Tearing the renderer down here is what turned a recoverable input
        // problem into a completely dead platform: no UI service reaches the
        // plugins, so nothing renders and the hotkey never runs either.
        ReportEmbeddedGate(
            state, "window procedure could not be hooked; overlay stays visible but "
            "will not accept mouse or keyboard",
            LastErrorDetail("error", state.input_install_error));
    }
    {
        std::scoped_lock plugin_lock(*state.plugin_mutex);
        if (!state.lifecycle_invoker_bound) {
            const auto lifecycle_invoke = state.diagnostics.lifecycle_invoke;
            const auto lifecycle_post = state.diagnostics.lifecycle_post;
            const auto plugin_mutex = state.plugin_mutex;
            auto* const plugins = state.plugins;
            state.diagnostics.lifecycle_invoke = [lifecycle_invoke, plugin_mutex, plugins](
                std::function<void()> operation) -> std::uint32_t {
                auto guarded = [plugin_mutex, plugins, operation = std::move(operation)]() mutable {
                    plugins->RunLifecycleOperation(*plugin_mutex, std::move(operation));
                };
                if (lifecycle_invoke) return lifecycle_invoke(std::move(guarded));
                try {
                    guarded();
                    return ERROR_SUCCESS;
                } catch (...) {
                    return ERROR_UNHANDLED_EXCEPTION;
                }
            };
            state.diagnostics.lifecycle_post = [
                lifecycle_post, lifecycle_invoke, plugin_mutex, plugins](
                std::function<void()> operation) -> std::uint32_t {
                auto guarded = [plugin_mutex, plugins, operation = std::move(operation)]() mutable {
                    plugins->RunLifecycleOperation(*plugin_mutex, std::move(operation));
                };
                if (lifecycle_post) return lifecycle_post(std::move(guarded));
                if (lifecycle_invoke) return lifecycle_invoke(std::move(guarded));
                try {
                    guarded();
                    return ERROR_SUCCESS;
                } catch (...) {
                    return ERROR_UNHANDLED_EXCEPTION;
                }
            };
            state.lifecycle_invoker_bound = true;
        }
    }
    const auto plugin_owner = state.plugin_owner.lock();
    if (plugin_owner == nullptr ||
        !InitializePlatformUi(*state.plugins, state.diagnostics, plugin_owner)) {
        // A quarantined owner still references the previous generation. Do
        // not mark this context as initialized or publish a second UI owner.
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    {
        std::scoped_lock plugin_lock(*state.plugin_mutex);
        state.plugins->SetImGuiContext(ImGui::GetCurrentContext());
    }
    state.platform_ui_initialized = true;
    if (!state.ui_services.Publish(EmbeddedUiServiceTable())) {
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    const auto resource_backend = CreateEmbeddedUiResourceRenderBackend(state);
    if (resource_backend == nullptr) {
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    bool ui_device_rebuilt{};
    {
        std::scoped_lock plugin_lock(*state.plugin_mutex);
        SynchronizeHostFontScale(state);
        state.plugins->SetUiResourceRenderBackend(resource_backend);
        state.plugins->SetUiService(EmbeddedUiServiceTable());
        // The PluginManager owns logical font/texture generations. Do not
        // expose a fresh renderer generation until it has acknowledged the
        // current device generation.
        state.plugin_ui_device_active = true;
        ui_device_rebuilt = state.plugins->OnUiDeviceRebuilt();
    }
    if (!ui_device_rebuilt) {
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    state.renderer = RendererLifecycle::Ready;
    std::ostringstream message;
    message << "embedded=1 hwnd=" << state.window
            << " ui_generation="
            << state.ui_services.Query().generation;
    if (state.diagnostics.logger != nullptr) {
        cabbird::LogDetails details;
        details.thread_domain = cabbird::LogThreadDomain::Render;
        details.event_id = "render.embedded_ready";
        static_cast<void>(state.diagnostics.logger->Log(
            cabbird::LogLevel::Info, "render", message.str(), std::move(details)));
    }
    // Also written to the plain platform log, which is where every abandoned
    // start-up above reports, so a successful one is visible in the same place.
    {
        std::ostringstream ready;
        ready << "api=" << (state.render_api == EmbeddedRenderApi::D3D11 ? "d3d11" : "d3d12")
              << " hwnd=0x" << std::hex << reinterpret_cast<std::uintptr_t>(state.window)
              << std::dec << " toggleKey=" << state.config.platform_toggle_key
              << " inputHooked=" << (state.input_installed ? 1 : 0);
        ReportEmbeddedGate(state, "ready", ready.str(), "embedded overlay: ");
    }
    return true;
}

// Binds a render target view to back buffer zero.
//
// Split out because the view has to be dropped before ResizeBuffers and rebuilt
// afterwards; holding a reference to a back buffer is what makes a resize fail.
bool CreateD3D11RenderTarget(EmbeddedState& state) {
    if (state.d3d11_device == nullptr || state.source_swap_chain == nullptr) return false;
    ID3D11Texture2D* back_buffer{};
    if (FAILED(state.source_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) ||
        back_buffer == nullptr) {
        Release(back_buffer);
        return false;
    }
    const HRESULT created = state.d3d11_device->CreateRenderTargetView(
        back_buffer, nullptr, &state.d3d11_render_target);
    Release(back_buffer);
    return SUCCEEDED(created) && state.d3d11_render_target != nullptr;
}

void ReleaseD3D11RenderTarget(EmbeddedState& state) noexcept {
    Release(state.d3d11_render_target);
}

// Brings the overlay up on a swap chain owned by a D3D11 device.
//
// Far smaller than the D3D12 path because the immediate context is the entire
// submission model: no command queue to capture, no allocators, no fences, no
// resource barriers, and one render target view instead of a descriptor heap.
bool InitializeGraphicsD3D11(EmbeddedState& state, IDXGISwapChain* swap_chain) {
    if (state.plugins == nullptr || state.quarantined_plugin_owner != nullptr ||
        PlatformUiQuarantined(state.plugins)) {
        ReportEmbeddedGate(state, "plugin host unavailable or quarantined");
        return false;
    }
    if (state.platform_ui_initialized &&
        !ReleaseGraphics(state, RendererLifecycle::Cold)) {
        return false;
    }
    state.renderer = RendererLifecycle::Discovering;
    ID3D11Device* device{};
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swap_chain->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr ||
        FAILED(swap_chain->GetDesc(&description)) || description.BufferCount == 0 ||
        description.OutputWindow == nullptr) {
        Release(device);
        ReportEmbeddedGate(
            state, "swap chain is not a usable D3D11 target",
            DescribeSwapChainApi(swap_chain));
        state.renderer = RendererLifecycle::Cold;
        return false;
    }
    DWORD output_process{};
    GetWindowThreadProcessId(description.OutputWindow, &output_process);
    RECT output_rect{};
    GetClientRect(description.OutputWindow, &output_rect);
    const LONG width = output_rect.right - output_rect.left;
    const LONG height = output_rect.bottom - output_rect.top;
    if (output_process != GetCurrentProcessId() || !IsWindowVisible(description.OutputWindow) ||
        width < 640 || height < 360) {
        Release(device);
        ReportEmbeddedGate(
            state, "output window rejected",
            "process=" + std::to_string(output_process) +
                " visible=" + std::to_string(IsWindowVisible(description.OutputWindow) ? 1 : 0) +
                " size=" + std::to_string(width) + "x" + std::to_string(height));
        state.renderer = RendererLifecycle::Cold;
        return false;
    }
    state.d3d11_device = device;
    device->GetImmediateContext(&state.d3d11_context);
    swap_chain->AddRef();
    state.source_swap_chain = swap_chain;
    state.window = description.OutputWindow;
    state.render_target_format = description.BufferDesc.Format;
    state.selected_area = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (state.d3d11_context == nullptr || !CreateD3D11RenderTarget(state)) {
        ReportEmbeddedGate(state, "could not bind a render target to the D3D11 back buffer");
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    IMGUI_CHECKVERSION();
    state.imgui_context = ImGui::CreateContext();
    if (state.imgui_context == nullptr) {
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    if (!ConfigureCabbirdUiFontAtlasFromMemory(state.root)) {
        ReportEmbeddedGate(state, "prepared UI font sources incomplete", {},
            "embedded overlay degraded: ");
    }
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    const auto ini = state.imgui_ini_snapshot.load(std::memory_order_acquire);
    if (ini != nullptr && !ini->empty()) {
        ImGui::LoadIniSettingsFromMemory(ini->data(), ini->size());
    }
    if (!ImGui_ImplWin32_Init(state.window)) {
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    state.win32_initialized = true;
    if (!ImGui_ImplDX11_Init(state.d3d11_device, state.d3d11_context)) {
        static_cast<void>(ReleaseGraphics(state, RendererLifecycle::DeviceLost));
        return false;
    }
    state.dx11_initialized = true;
    // A failed material backend keeps the solid, readable UI fallback.
    static_cast<void>(cabbird::glass::InitializeRenderer(
        state.d3d11_device, state.d3d11_context));
    state.render_api = EmbeddedRenderApi::D3D11;
    return CompleteGraphicsInitialization(state);
}

bool InitializeGraphics(EmbeddedState& state, IDXGISwapChain* swap_chain) {
    if (swap_chain == nullptr) return false;
    // Anomaly settled the backend by asking the swap chain for its device and dispatching on
    // the answer.  This build carries only the Direct3D11 path, so
    // what is left of that branch is the answer the D3D11 title always produced.
    return InitializeGraphicsD3D11(state, swap_chain);
}


}  // namespace

bool ReleaseGraphics(
    EmbeddedState& state, RendererLifecycle final_state, bool force_release) {
    state.completed_ui_frame = false;
    state.renderer = final_state;
    if (state.imgui_context != nullptr &&
        ImGui::GetCurrentContext() != state.imgui_context) {
        // The renderer must never shut down a context owned by another
        // generation or host. Leave this generation retained for its render
        // owner to finish the handoff.
        return false;
    }
        if (state.imgui_context != nullptr) UpdateMenuCursor(state, false);
        state.ui_services.Withdraw(EmbeddedUiServiceTable());
    if (state.platform_ui_initialized) {
        // Teardown may need to drain a lifecycle callback which itself takes
        // the plugin mutex. Do this before taking that mutex here.
        if (force_release) {
            // The bounded stop path must establish quarantine before the
            // ImGui context is destroyed. If another owner transition is in
            // progress, leave this generation intact for its completion.
            if (!QuarantinePlatformUi(std::chrono::milliseconds(100))) return false;
        } else if (!ShutdownPlatformUi()) {
            return false;
        }
        state.platform_ui_initialized = false;
    }
    if (state.plugins != nullptr && PlatformUiQuarantined(state.plugins)) {
        return false;
    }
    {
        std::unique_lock plugin_lock(*state.plugin_mutex, std::try_to_lock);
        if (!plugin_lock.owns_lock()) return false;
        if (state.plugin_ui_device_active) {
            if (state.plugins != nullptr) state.plugins->OnUiDeviceLost();
            state.plugin_ui_device_active = false;
        }
        if (state.plugins != nullptr) {
            state.plugins->SetUiResourceRenderBackend({});
            state.plugins->SetUiService(nullptr);
            state.plugins->SetImGuiContext(nullptr);
        }
    }
    RestoreEmbeddedInput(state);
    cabbird::glass::ShutdownRenderer();
    if (state.dx11_initialized) {
        ImGui_ImplDX11_Shutdown();
        state.dx11_initialized = false;
    }
    if (state.win32_initialized) {
        ImGui_ImplWin32_Shutdown();
        state.win32_initialized = false;
    }
    if (state.imgui_context != nullptr) {
        CaptureImGuiSettings(state);
        ImGui::DestroyContext(state.imgui_context);
        state.imgui_context = nullptr;
    }
    ReleaseD3D11RenderTarget(state);
    Release(state.d3d11_context);
    Release(state.d3d11_device);
    state.render_api = EmbeddedRenderApi::None;
    Release(state.swap_chain);
    Release(state.source_swap_chain);
    state.window = nullptr;
    state.render_target_format = DXGI_FORMAT_UNKNOWN;
    state.selected_area = 0;
    return true;
}

void RenderEmbedded(IDXGISwapChain* swap_chain, UINT flags) {
    auto* state = g_state.load(std::memory_order_acquire);
    if (state == nullptr || (flags & DXGI_PRESENT_TEST) != 0) return;
    const bool sampled = state->performance.SampleRender();
    PerformanceTimer total_timer(
        state->performance, EmbeddedPerformanceStage::RenderTotal, sampled);
    {
    const auto lock_started = sampled ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    // A concurrent resize/teardown owns this frame. Let DXGI present the game
    // immediately rather than waiting for the overlay's lifecycle work.
    std::unique_lock render_lock(state->render_mutex, std::try_to_lock);
    if (!render_lock.owns_lock()) return;
    if (sampled) {
        state->performance.Record(
            EmbeddedPerformanceStage::RenderLockWait,
            std::chrono::steady_clock::now() - lock_started);
    }
    PerformanceTimer setup_timer(
        state->performance, EmbeddedPerformanceStage::RenderSetup, sampled);
    const RendererLifecycle lifecycle = state->renderer.load();
    if (lifecycle == RendererLifecycle::ResizePending ||
        lifecycle == RendererLifecycle::Stopping || lifecycle == RendererLifecycle::Stopped) return;
    if (lifecycle == RendererLifecycle::Ready && state->source_swap_chain != swap_chain) {
        if (!PreferSwapChain(*state, swap_chain)) return;
        // A failed release means the previous UI/device generation is still
        // owned by an in-flight callback. Do not initialize a second
        // generation against the same PluginManager or ImGui context.
        if (!ReleaseGraphics(*state, RendererLifecycle::Cold)) return;
    }
    if (state->renderer.load() != RendererLifecycle::Ready &&
        !InitializeGraphics(*state, swap_chain)) return;
    if (state->source_swap_chain != swap_chain) return;

    // One lease covers resource preparation and drawing. When Game/Lifecycle
    // owns the gate, retain the completed UI instead of presenting a bare game
    // frame. No plugin callback, atlas rebuild or resource collection runs in
    // that path; their draw resources remain owned by this Render generation.
    const auto prepare_lock_started = sampled ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
    std::unique_lock plugin_frame_lock(*state->plugin_mutex, std::try_to_lock);
    if (state->plugins == nullptr) return;
    if (!plugin_frame_lock.owns_lock()) {
        if (!state->completed_ui_frame ||
            ImGui::GetCurrentContext() != state->imgui_context ||
            state->d3d11_context == nullptr || state->d3d11_render_target == nullptr) return;
        ImDrawData* const data = ImGui::GetDrawData();
        if (data == nullptr) return;
        ID3D11Resource* resource{};
        state->d3d11_render_target->GetResource(&resource);
        ID3D11Texture2D* texture{};
        const HRESULT queried = resource->QueryInterface(IID_PPV_ARGS(&texture));
        resource->Release();
        if (FAILED(queried)) return;
        D3D11_TEXTURE2D_DESC target{};
        texture->GetDesc(&target);
        texture->Release();
        if (data->DisplaySize.x * data->FramebufferScale.x != target.Width ||
            data->DisplaySize.y * data->FramebufferScale.y != target.Height) return;
        // Glass callbacks and their deque payloads belong to this completed
        // ImGui frame. Captures still read the current game's backbuffer.
        cabbird::glass::RenderDrawData(state->d3d11_context, data,
            state->d3d11_render_target);
        return;
    }
    // Preparation can retire textures or rebuild the atlas used by the old
    // draw data. It is no longer eligible for submission after this point.
    state->completed_ui_frame = false;
    if (sampled) {
        state->performance.Record(EmbeddedPerformanceStage::RenderPrepareLockWait,
            std::chrono::steady_clock::now() - prepare_lock_started);
    }

    unsigned toggle_key = state->config.platform_toggle_key;
    unsigned hide_all_key = state->config.platform_hide_all_key;
    if (state->diagnostics.settings_snapshot) {
        const auto settings = state->diagnostics.settings_snapshot();
        if (settings.ready) {
            toggle_key = settings.values.input_menu_toggle;
            hide_all_key = settings.values.input_hide_all_toggle;
        }
    }
    const auto hotkeys = ConsumeEmbeddedMenuHotkeys(
        *state, toggle_key, hide_all_key, PlatformUiCapturingHotkey());
    if (hotkeys.toggle_hide_all) {
        cabbird::SetHostUiAllHidden(!cabbird::HostUiAllHidden());
    }
    if (hotkeys.toggle_menu) {
        const bool expanding = cabbird::HostUiMenusCollapsed();
        cabbird::SetHostUiMenusCollapsed(!expanding);
        // The shell's own close button marks the management window closed, and
        // that state is persisted. Collapsing is not the same as closing, so a
        // hotkey that only flipped the collapse flag could never bring a closed
        // shell back -- not in that session and not in any later one, because
        // start-up reads the persisted state and collapses again. Reopening here
        // is what makes the hotkey a way back in.
        if (expanding) static_cast<void>(RevealPlatformUi());
    }
    static_cast<void>(ApplyHostUiManagementExpansionRequest());

    // D3D11 submits through the immediate context, so none of the back buffer
    // index, fence wait, allocator reset or pixel probe below applies to it.
    std::uint64_t capture_generation{};
    bool probing = false;
    setup_timer.Stop();


    PerformanceTimer ui_timer(
        state->performance, EmbeddedPerformanceStage::RenderUi, sampled);
    {
        const auto prepare_started = sampled ? std::chrono::steady_clock::now()
                                             : std::chrono::steady_clock::time_point{};
        SynchronizeHostFontScale(*state);
        state->plugins->PrepareUiResources();
        PreparePlatformUiResources();
        if (sampled) {
            state->performance.Record(
                EmbeddedPerformanceStage::RenderPrepareLocked,
                std::chrono::steady_clock::now() - prepare_started);
        }
    }
    // Include staged plugin fonts before a first default-atlas upload.
    ImGui_ImplDX11_NewFrame();
    // The GPU atlas is resident now. Keep its font sources for device recovery,
    // but release the Alpha8 and RGBA32 CPU raster copies between rebuilds.
    ImFontAtlas* const atlas = ImGui::GetIO().Fonts;
    if (atlas != nullptr && atlas->TexID != ImTextureID{} &&
        (atlas->TexPixelsAlpha8 != nullptr || atlas->TexPixelsRGBA32 != nullptr)) {
        atlas->ClearTexData();
    }
    const auto frame_begin_started = sampled ? std::chrono::steady_clock::now()
                                             : std::chrono::steady_clock::time_point{};
    UpdateMenuCursor(*state, cabbird::HostUiMenusCaptureMouse());
    ImGui_ImplWin32_NewFrame();
    if (!state->input_installed) FeedPolledInput(*state);
    ImGui::NewFrame();
    cabbird::PrepareHostUiFrame();
    if (sampled) {
        state->performance.Record(
            EmbeddedPerformanceStage::RenderFrameBegin,
            std::chrono::steady_clock::now() - frame_begin_started);
    }
    bool input_frame_published{};
    {
        auto phase_started = sampled ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
        input_frame_published = PublishEmbeddedInputFrame(*state);
        if (sampled) {
            const auto phase_completed = std::chrono::steady_clock::now();
            state->performance.Record(
                EmbeddedPerformanceStage::RenderInput,
                phase_completed - phase_started);
            phase_started = phase_completed;
        }
        DrawPlatformUi();
        if (sampled) {
            const auto phase_completed = std::chrono::steady_clock::now();
            state->performance.Record(
                EmbeddedPerformanceStage::RenderPlatformUi,
                phase_completed - phase_started);
            phase_started = phase_completed;
        }
        if (!cabbird::HostUiAllHidden()) {
            state->plugins->Draw(ImGui::GetCurrentContext());
        }
        if (sampled) {
            const auto phase_completed = std::chrono::steady_clock::now();
            state->performance.Record(
                EmbeddedPerformanceStage::RenderPluginDraw,
                phase_completed - phase_started);
            phase_started = phase_completed;
        }
        if (input_frame_published) PublishEmbeddedUiCapture(*state);
        if (sampled) {
            state->performance.Record(
                EmbeddedPerformanceStage::RenderCapture,
                std::chrono::steady_clock::now() - phase_started);
        }
    }
    plugin_frame_lock.unlock();
    const auto finalize_started = sampled ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
    if (probing) {
        const ImVec2 origin = ImGui::GetMainViewport()->Pos;
        auto* marker = ImGui::GetForegroundDrawList();
        marker->AddRectFilled(
            ImVec2(origin.x + 2.0F, origin.y + 2.0F),
            ImVec2(origin.x + 30.0F, origin.y + 30.0F),
            IM_COL32(255, 0, 255, 255));
        marker->AddRectFilled(
            ImVec2(origin.x + 16.0F, origin.y + 2.0F),
            ImVec2(origin.x + 30.0F, origin.y + 30.0F),
            IM_COL32(0, 255, 255, 255));
    }
    // The overlay pass.  It runs with the ImGui frame still OPEN and before Render(), so
    // subscriptions draw into the background draw list -- underneath every host window,
    // which is what an ESP needs.  Placed after the plugin windows on purpose: a plugin's
    // draw callback is where it decides WHAT to draw, and this is where it is told the
    // frame is real (viewport size, projection) and gets to put pixels down.
    //
    // Costs nothing when nobody subscribed -- RunHostOverlayFrame returns before touching
    // ImGui in that case, so an ordinary session pays one taken mutex and an empty copy.
    if (!cabbird::HostUiAllHidden()) {
        cabbird::RunHostOverlayFrame(ImGui::GetCurrentContext(), &CabbirdUiFontForPixelSize);
    }
    ImGui::Render();
    if (ImGui::GetIO().WantSaveIniSettings) CaptureImGuiSettings(*state);
    cabbird::glass::PrepareDrawData(ImGui::GetDrawData());
    NotifyRenderThread(*state, capture_generation);
    if (sampled) {
        state->performance.Record(
            EmbeddedPerformanceStage::RenderFinalize,
            std::chrono::steady_clock::now() - finalize_started);
    }
    ui_timer.Stop();

    PerformanceTimer command_timer(
        state->performance, EmbeddedPerformanceStage::RenderCommands, sampled);
    if (state->d3d11_context == nullptr || state->d3d11_render_target == nullptr) {
        static_cast<void>(ReleaseGraphics(*state, RendererLifecycle::DeviceLost));
        return;
    }
    cabbird::glass::RenderDrawData(state->d3d11_context, ImGui::GetDrawData(),
        state->d3d11_render_target);
    state->completed_ui_frame = true;
    command_timer.Stop();
    // Lifecycle mutations are submitted only after the render mutex and the
    // plugin draw lock have been released, so a slow reload cannot block
    // Resize/Present teardown.
    }
    FlushPlatformUiActions();
}
void PrepareEmbeddedResize(IDXGISwapChain* swap_chain) {
    auto* const state = g_state.load(std::memory_order_acquire);
    if (state == nullptr) return;
    std::scoped_lock lock(state->render_mutex);
    if (state->source_swap_chain != swap_chain ||
        state->renderer.load() != RendererLifecycle::Ready) return;
    state->completed_ui_frame = false;
    state->renderer = RendererLifecycle::ResizePending;
    // DXGI rejects ResizeBuffers while a view still references its backbuffer.
    ReleaseD3D11RenderTarget(*state);
}

void CompleteEmbeddedResize(IDXGISwapChain* swap_chain) {
    auto* const state = g_state.load(std::memory_order_acquire);
    if (state == nullptr) return;
    std::scoped_lock lock(state->render_mutex);
    if (state->source_swap_chain != swap_chain ||
        state->renderer.load() != RendererLifecycle::ResizePending) return;
    // Recover the current buffer even when ResizeBuffers failed: DXGI retains
    // the old buffers on failure, and the overlay must keep drawing on them.
    if (CreateD3D11RenderTarget(*state)) {
        state->renderer = RendererLifecycle::Ready;
    } else {
        static_cast<void>(ReleaseGraphics(*state, RendererLifecycle::DeviceLost));
    }
}

void HandlePresentResult(IDXGISwapChain* swap_chain, HRESULT result) {
    if (result != DXGI_ERROR_DEVICE_REMOVED && result != DXGI_ERROR_DEVICE_RESET) return;
    auto* state = g_state.load(std::memory_order_acquire);
    if (state == nullptr) return;
    std::scoped_lock lock(state->render_mutex);
    if (state->source_swap_chain == swap_chain) {
        static_cast<void>(ReleaseGraphics(*state, RendererLifecycle::DeviceLost));
    }
}

}  // namespace cabbird::embedded

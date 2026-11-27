#include "cabbird/unitymem_compat.hpp"
#include "cabbird/platform_host.hpp"

#include "cabbird/host_ui_service.hpp"
#include "cabbird/adapter_service_registry.hpp"
#include "cabbird/platform_ui_model.hpp"
#include "cabbird/platform_ui_layout.hpp"
#include "cabbird/platform_ui_input_policy.hpp"
#include "cabbird/cabbird_ui_theme.hpp"
#include "cabbird/liquid_glass.hpp"
#include "cabbird/plugin_scope.hpp"
#include "cabbird/ui_resource_decoder.hpp"
#include "cabbird/ui_resource_registry.hpp"
#include "cabbird/ui_resource_render_backend.hpp"
#include "cabbird/reliable_storage.hpp"
#include "cabbird/unity_adapter.hpp"
#include "cabbird/plugin_manager.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <commdlg.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace cabbird {
namespace {

// Fully transparent clear colour. Composition takes the swap chain's alpha
// literally, so clearing to zero alpha leaves the desktop showing through
// everywhere the panels do not draw. A layered window cannot be used for this:
// WS_EX_LAYERED redirects the window to a surface the D3D swap chain never
// reaches, which paints an opaque rectangle instead.
constexpr float kHostClearColor[4]{0.0f, 0.0f, 0.0f, 0.0f};

struct HostWindow {
    HWND window{};
    HWND target{};
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    IDXGISwapChain* swap_chain{};
    ID3D11RenderTargetView* render_target{};
    ID3D11ShaderResourceView* header_logo{};
    // Whether the compositor honoured the swap chain's alpha. Placement depends
    // on it: a window that failed to become transparent must stay a small panel
    // rather than an opaque sheet over the whole game.
    bool alpha_composited{};
    bool all_hidden_region{};
    // The panel rectangles the window region was last built from. Rebuilding it
    // every frame would be wasted work; this is what says when it changed.
    std::vector<std::array<LONG, 4>> region_panels;
    // Keeps the composition-root owner mapped if an in-flight game tick
    // exceeds the bounded host shutdown handoff.
    std::shared_ptr<PluginManager> plugin_owner;
    bool visible{};
    unsigned toggle_key{VK_INSERT};
    unsigned hide_all_key{VK_DELETE};
    cabbird::PlatformMenuHotkey menu_hotkey;
    cabbird::PlatformMenuHotkey hide_all_hotkey;
    bool attached{};
};

HostWindow* g_window{};
ID3D11ShaderResourceView* g_standalone_header_logo{};

// A standalone host can outlive its worker when a game tick is still inside
// plugin code. Keep a process-boundary owner token so the composition root can
// report that generation as quarantined even though the native window itself
// is retained through a raw Win32 handle/global pointer.
struct StandaloneHostQuarantine final {
    std::mutex mutex;
    std::vector<std::shared_ptr<PluginManager>> owners;
};

StandaloneHostQuarantine* GetStandaloneHostQuarantine() noexcept {
    static auto* registry = []() noexcept -> StandaloneHostQuarantine* {
        try {
            return new StandaloneHostQuarantine();
        } catch (...) {
            return nullptr;
        }
    }();
    return registry;
}

void RetainStandaloneHostOwner(const std::shared_ptr<PluginManager>& owner) noexcept {
    if (owner == nullptr) return;
    auto* registry = GetStandaloneHostQuarantine();
    if (registry == nullptr) return;
    try {
        std::lock_guard<std::mutex> lock(registry->mutex);
        const auto found = std::ranges::find_if(registry->owners, [&owner](const auto& retained) {
            return retained.get() == owner.get();
        });
        if (found == registry->owners.end()) registry->owners.push_back(owner);
    } catch (...) {
        // HostWindow::plugin_owner remains a second lifetime fence when the
        // diagnostic registry cannot allocate.
    }
}

bool IsStandaloneHostQuarantined(const PluginManager* owner) noexcept {
    auto* registry = GetStandaloneHostQuarantine();
    if (registry == nullptr) return false;
    std::lock_guard<std::mutex> lock(registry->mutex);
    if (owner == nullptr) return !registry->owners.empty();
    return std::ranges::any_of(registry->owners, [owner](const auto& retained) {
        return retained != nullptr && retained.get() == owner;
    });
}

struct WindowCandidate {
    HWND window{};
    long long area{};
};

BOOL CALLBACK FindWindowCallback(HWND window, LPARAM parameter) {
    DWORD process_id{};
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != GetCurrentProcessId() || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    RECT rectangle{};
    if (!GetClientRect(window, &rectangle)) return TRUE;
    const auto area = static_cast<long long>(rectangle.right - rectangle.left) *
                      static_cast<long long>(rectangle.bottom - rectangle.top);
    auto& candidate = *reinterpret_cast<WindowCandidate*>(parameter);
    if (area > candidate.area) candidate = {window, area};
    return TRUE;
}

HWND FindProcessWindow() {
    WindowCandidate candidate;
    EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&candidate));
    return candidate.window;
}

// Restricts the window to the rectangles the overlay actually draws into.
//
// A window spanning the whole client area would otherwise swallow every click,
// including the ones meant for the game. WS_EX_TRANSPARENT looks like the answer
// but only passes mouse events through for layered windows, and layering
// redirects the window to a surface a D3D swap chain never presents into. A
// window region clips hit-testing as well as painting on an ordinary window, so
// whatever the overlay leaves empty belongs to the game again.
//
// Drags survive this because the backend takes the mouse capture on button
// down, and a captured window keeps receiving input regardless of its region.
void UpdateHostWindowRegion(HostWindow& host) {
    if (cabbird::HostUiAllHidden()) {
        if (!host.all_hidden_region) {
            HRGN region = CreateRectRgn(0, 0, 0, 0);
            if (region != nullptr) {
                if (SetWindowRgn(host.window, region, TRUE) != 0) host.all_hidden_region = true;
                else DeleteObject(region);
            }
        }
        return;
    }
    if (host.all_hidden_region) {
        SetWindowRgn(host.window, nullptr, TRUE);
        host.all_hidden_region = false;
        host.region_panels.clear();
    }

    if (!host.attached) return;
    const ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr) return;
    std::vector<std::array<LONG, 4>> panels;
    for (const ImGuiWindow* window : context->Windows) {
        if (window == nullptr || !window->WasActive || window->Hidden) continue;
        if ((window->Flags & ImGuiWindowFlags_ChildWindow) != 0) continue;
        const ImRect bounds = window->Rect();
        const std::array<LONG, 4> panel{
            static_cast<LONG>(std::floor(bounds.Min.x)),
            static_cast<LONG>(std::floor(bounds.Min.y)),
            static_cast<LONG>(std::ceil(bounds.Max.x)),
            static_cast<LONG>(std::ceil(bounds.Max.y))};
        if (panel[2] <= panel[0] || panel[3] <= panel[1]) continue;
        panels.push_back(panel);
    }
    if (panels == host.region_panels) return;
    host.region_panels = panels;
    if (panels.empty()) {
        // Nothing is being drawn. An empty region would make the window vanish
        // in a way that also drops the swap chain's output, so keep a single
        // pixel and leave the rest of the client area to the game.
        SetWindowRgn(host.window, CreateRectRgn(0, 0, 1, 1), TRUE);
        return;
    }
    HRGN region = CreateRectRgn(panels[0][0], panels[0][1], panels[0][2], panels[0][3]);
    if (region == nullptr) return;
    for (std::size_t index = 1; index < panels.size(); ++index) {
        const auto& panel = panels[index];
        HRGN addition = CreateRectRgn(panel[0], panel[1], panel[2], panel[3]);
        if (addition == nullptr) continue;
        CombineRgn(region, region, addition, RGN_OR);
        DeleteObject(addition);
    }
    // The window manager owns the region from here, so it must not be deleted.
    if (SetWindowRgn(host.window, region, TRUE) == 0) DeleteObject(region);
}

bool PlaceAttachedWindow(HostWindow& host) {
    if (!host.attached) return true;
    if (!IsWindow(host.target)) return false;
    RECT client{};
    POINT origin{};
    if (!GetClientRect(host.target, &client) || !ClientToScreen(host.target, &origin)) return false;
    const int client_width = client.right - client.left;
    const int client_height = client.bottom - client.top;
    if (client_width <= 0 || client_height <= 0) return true;
    // Cover the whole client area once the window is transparent, so panels can
    // be moved anywhere over the game. Clamping to a centred box was what
    // confined the panels to one rectangle, which only made sense while the
    // window was an opaque sheet that would otherwise have hidden the game.
    const bool full_client = host.alpha_composited;
    const int width = full_client
        ? client_width : std::max(320, std::min(1080, client_width - 32));
    const int height = full_client
        ? client_height : std::max(240, std::min(720, client_height - 32));
    const int left = full_client ? origin.x : origin.x + (client_width - width) / 2;
    const int top = full_client ? origin.y : origin.y + (client_height - height) / 2;
    return SetWindowPos(
               host.window, HWND_TOP, left, top, width, height,
               SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE;
}

void ReleaseRenderTarget(HostWindow& host) {
    if (host.render_target != nullptr) {
        host.render_target->Release();
        host.render_target = nullptr;
    }
}

bool CreateRenderTarget(HostWindow& host) {
    ID3D11Texture2D* back_buffer{};
    if (FAILED(host.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return false;
    const HRESULT result = host.device->CreateRenderTargetView(back_buffer, nullptr, &host.render_target);
    back_buffer->Release();
    return SUCCEEDED(result);
}

LRESULT WINAPI WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (g_window != nullptr) {
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
            g_window->menu_hotkey.RecordKeyDown(static_cast<unsigned>(wparam), (lparam & 0x40000000L) != 0);
            g_window->hide_all_hotkey.RecordKeyDown(static_cast<unsigned>(wparam), (lparam & 0x40000000L) != 0);
        } else if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && wparam == FALSE)) {
            g_window->menu_hotkey.Reset();
            g_window->hide_all_hotkey.Reset();
        }
    }
    if (!cabbird::HostUiAllHidden() && ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return TRUE;
    switch (message) {
    case WM_SIZE:
        if (g_window != nullptr && g_window->device != nullptr && wparam != SIZE_MINIMIZED) {
            // ResizeBuffers requires every backbuffer reference to be unbound.
            // Leaving the RTV on the output-merger can expose the default white
            // client background while Windows resizes the standalone preview.
            g_window->context->OMSetRenderTargets(0, nullptr, nullptr);
            g_window->context->ClearState();
            g_window->context->Flush();
            ReleaseRenderTarget(*g_window);
            if (SUCCEEDED(g_window->swap_chain->ResizeBuffers(
                    0, static_cast<UINT>(LOWORD(lparam)), static_cast<UINT>(HIWORD(lparam)),
                    DXGI_FORMAT_UNKNOWN, 0))) {
                static_cast<void>(CreateRenderTarget(*g_window));
            }
        }
        return 0;
    case WM_ERASEBKGND:
        // The D3D swap chain owns client painting. Suppress a GDI white flash
        // between a native resize and the next rendered frame.
        return 1;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_CLOSE:
        if (g_window != nullptr && !g_window->attached) {
            PostQuitMessage(0);
        } else if (g_window != nullptr) {
            g_window->visible = false;
            ShowWindow(window, SW_HIDE);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void DestroyDevice(HostWindow& host);

// Asks the desktop compositor to honour the swap chain's alpha channel.
//
// An empty blur region is the long-standing way to get per-pixel alpha on an
// ordinary window. The alternative, WS_EX_LAYERED, cannot be used here: it
// redirects the window onto a surface that a D3D swap chain never presents
// into, which shows up as an opaque rectangle where the overlay should be.
//
// Resolved at run time because the process may be running against the
// framework's own forwarding dwmapi, and because losing transparency is not
// worth failing over.
bool EnableHostWindowTransparency(HWND window) noexcept {
    if (window == nullptr) return false;
    using EnableBlurBehind = HRESULT(WINAPI*)(HWND, const DWM_BLURBEHIND*);
    const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm == nullptr) return false;
    const auto enable_blur_behind =
        reinterpret_cast<EnableBlurBehind>(GetProcAddress(dwm, "DwmEnableBlurBehindWindow"));
    if (enable_blur_behind == nullptr) return false;
    const HRGN region = CreateRectRgn(0, 0, -1, -1);
    DWM_BLURBEHIND blur{};
    blur.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
    blur.fEnable = TRUE;
    blur.hRgnBlur = region;
    const HRESULT result = enable_blur_behind(window, &blur);
    if (region != nullptr) DeleteObject(region);
    return SUCCEEDED(result);
}

bool CreateDevice(HostWindow& host) {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = host.window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const std::array<D3D_FEATURE_LEVEL, 2> levels{
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected{};
    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels.data(),
        static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &description, &host.swap_chain,
        &host.device, &selected, &host.context);
    if (FAILED(result) || !CreateRenderTarget(host)) {
        // D3D11CreateDeviceAndSwapChain may have produced a partially usable
        // device before render-target creation fails. Release every interface
        // here so callers can safely destroy only the window/class.
        DestroyDevice(host);
        return false;
    }
    return true;
}

void DestroyDevice(HostWindow& host) {
    ReleaseRenderTarget(host);
    if (host.header_logo != nullptr) {
        if (g_standalone_header_logo == host.header_logo) g_standalone_header_logo = nullptr;
        host.header_logo->Release();
        host.header_logo = nullptr;
    }
    if (host.swap_chain != nullptr) {
        host.swap_chain->Release();
        host.swap_chain = nullptr;
    }
    if (host.context != nullptr) {
        host.context->Release();
        host.context = nullptr;
    }
    if (host.device != nullptr) {
        host.device->Release();
        host.device = nullptr;
    }
}

std::optional<std::uintptr_t> ParseAddress(const char* text) {
    if (text == nullptr || *text == '\0') return std::nullopt;
    std::string_view value(text);
    int base = 10;
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        value.remove_prefix(2);
        base = 16;
    }
    std::uintptr_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, base);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()
        ? std::optional<std::uintptr_t>(result)
        : std::nullopt;
}

std::optional<std::vector<std::uint8_t>> ParseBytes(std::string_view text) {
    std::vector<std::uint8_t> result;
    std::size_t cursor{};
    while (cursor < text.size()) {
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
        if (cursor == text.size()) break;
        const auto end = text.find_first_of(" \t\r\n,", cursor);
        const auto token = text.substr(cursor, end == std::string_view::npos ? text.size() - cursor : end - cursor);
        unsigned value{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || value > 0xff) {
            return std::nullopt;
        }
        result.push_back(static_cast<std::uint8_t>(value));
        cursor = end == std::string_view::npos ? text.size() : end + 1;
    }
    return result.empty() ? std::nullopt : std::optional<std::vector<std::uint8_t>>(std::move(result));
}

[[nodiscard]] ImVec2 Offset(const ImVec2 position, const float x, const float y) noexcept {
    return {position.x + x, position.y + y};
}

[[nodiscard]] float PlatformUiScale() noexcept {
    const float scale = ImGui::GetIO().FontGlobalScale;
    return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

[[nodiscard]] float Scaled(const float value) noexcept {
    return value * PlatformUiScale();
}

[[nodiscard]] ImVec2 Scaled(const float x, const float y) noexcept {
    return {Scaled(x), Scaled(y)};
}

[[nodiscard]] ImVec2 ScaledOffset(
    const ImVec2 position, const float x, const float y) noexcept {
    return Offset(position, Scaled(x), Scaled(y));
}

[[nodiscard]] float AvailableItemWidth(
    const float reserved = 0.0f,
    const float maximum = (std::numeric_limits<float>::max)()) noexcept {
    const float available =
        (std::max)(1.0f, ImGui::GetContentRegionAvail().x - Scaled(reserved));
    return (std::min)(available,
        maximum == (std::numeric_limits<float>::max)() ? maximum : Scaled(maximum));
}

[[nodiscard]] ImVec2 FillAvailableSize(
    const float height,
    const float reserved = 0.0f,
    const float maximum = (std::numeric_limits<float>::max)()) noexcept {
    return {AvailableItemWidth(reserved, maximum), Scaled(height)};
}

void SetAvailableItemWidth(
    const float reserved = 0.0f,
    const float maximum = (std::numeric_limits<float>::max)()) noexcept {
    ImGui::SetNextItemWidth(AvailableItemWidth(reserved, maximum));
}

[[nodiscard]] std::string EncodeUtf8(const char32_t codepoint) {
    std::string result;
    if (codepoint <= 0x7fU) {
        result.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        result.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        result.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    return result;
}

enum class ShellGlyph : std::size_t {
    Package,
    Layers,
    Shield,
    Terminal,
    Settings,
    Refresh,
    More,
    ChevronDown,
    ChevronUp,
    ChevronLeft,
    ChevronRight,
    Pin,
    Search,
    Close,
    Play,
    Stop,
    Eye,
    EyeOff,
    Check,
    Warning,
    Info,
    Copy,
    Activity,
    Star,
    StarFilled,
    Count,
};

[[nodiscard]] const char* ShellGlyphText(const ShellGlyph glyph) {
    static const std::array<std::string, static_cast<std::size_t>(ShellGlyph::Count)> glyphs = [] {
        std::array<std::string, static_cast<std::size_t>(ShellGlyph::Count)> values;
        values[static_cast<std::size_t>(ShellGlyph::Package)] = EncodeUtf8(0xe7b8);
        values[static_cast<std::size_t>(ShellGlyph::Layers)] = EncodeUtf8(0xe81e);
        values[static_cast<std::size_t>(ShellGlyph::Shield)] = EncodeUtf8(0xea18);
        values[static_cast<std::size_t>(ShellGlyph::Terminal)] = EncodeUtf8(0xe756);
        values[static_cast<std::size_t>(ShellGlyph::Settings)] = EncodeUtf8(0xe713);
        values[static_cast<std::size_t>(ShellGlyph::Refresh)] = EncodeUtf8(0xe72c);
        values[static_cast<std::size_t>(ShellGlyph::More)] = EncodeUtf8(0xe712);
        values[static_cast<std::size_t>(ShellGlyph::ChevronDown)] = EncodeUtf8(0xe70d);
        values[static_cast<std::size_t>(ShellGlyph::ChevronUp)] = EncodeUtf8(0xe70e);
        values[static_cast<std::size_t>(ShellGlyph::ChevronLeft)] = EncodeUtf8(0xe76b);
        values[static_cast<std::size_t>(ShellGlyph::ChevronRight)] = EncodeUtf8(0xe76c);
        values[static_cast<std::size_t>(ShellGlyph::Pin)] = EncodeUtf8(0xe718);
        values[static_cast<std::size_t>(ShellGlyph::Search)] = EncodeUtf8(0xe721);
        values[static_cast<std::size_t>(ShellGlyph::Close)] = EncodeUtf8(0xe711);
        values[static_cast<std::size_t>(ShellGlyph::Play)] = EncodeUtf8(0xe768);
        values[static_cast<std::size_t>(ShellGlyph::Stop)] = EncodeUtf8(0xe71a);
        values[static_cast<std::size_t>(ShellGlyph::Eye)] = EncodeUtf8(0xe890);
        values[static_cast<std::size_t>(ShellGlyph::EyeOff)] = EncodeUtf8(0xe8a9);
        values[static_cast<std::size_t>(ShellGlyph::Check)] = EncodeUtf8(0xe73e);
        values[static_cast<std::size_t>(ShellGlyph::Warning)] = EncodeUtf8(0xe7ba);
        values[static_cast<std::size_t>(ShellGlyph::Info)] = EncodeUtf8(0xe946);
        values[static_cast<std::size_t>(ShellGlyph::Copy)] = EncodeUtf8(0xe8c8);
        values[static_cast<std::size_t>(ShellGlyph::Activity)] = EncodeUtf8(0xe9d9);
        values[static_cast<std::size_t>(ShellGlyph::Star)] = EncodeUtf8(0xe734);
        values[static_cast<std::size_t>(ShellGlyph::StarFilled)] = EncodeUtf8(0xe735);
        return values;
    }();
    return glyphs[static_cast<std::size_t>(glyph)].c_str();
}

constexpr std::string_view kPlatformUiWindowOwner = "cabbird.host.platform-ui";
constexpr std::string_view kPlatformUiWindowId = "management-shell";
constexpr std::uint64_t kPlatformUiWindowGeneration = 1;
constexpr int kPlatformLogoResourceId = 101;
constexpr float kPlatformHeaderHeight = 52.0f;
constexpr float kPlatformHeaderLogoSize = 30.0f;
constexpr float kPlatformHeaderActionColumnWidth = 110.0f;
constexpr float kPlatformToastHeight = 38.0f;
constexpr float kPlatformTabBarReserve = 82.0f;
constexpr float kPlatformGloballyCollapsedWidth = 132.0f;
constexpr float kPlatformInitialShellWidth = 1180.0f;
constexpr float kPlatformInitialShellHeight = 700.0f;
constexpr float kPlatformStandardShellHeight = 600.0f;
constexpr float kPlatformMinimumShellWidth = 760.0f;
constexpr float kPlatformMinimumShellHeight = 500.0f;
constexpr float kPlatformMaximumShellWidth = 1560.0f;
constexpr float kPlatformMaximumShellHeight = 900.0f;
constexpr float kPlatformShellViewportMargin = 12.0f;

struct ContactInformation final {
    std::string repository_label;
    std::string repository_url;
    std::string qq_group;
};

[[nodiscard]] std::optional<ContactInformation> LoadContactInformation(
    const std::filesystem::path& path) noexcept {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) return std::nullopt;
        const nlohmann::json document = nlohmann::json::parse(input);
        if (document.at("schemaVersion").get<int>() != 1) return std::nullopt;
        const nlohmann::json& repository = document.at("repository");
        ContactInformation contact{
            repository.at("label").get<std::string>(),
            repository.at("url").get<std::string>(),
            document.at("qqGroup").get<std::string>(),
        };
        if (contact.repository_label.empty() || contact.repository_url.empty() ||
            contact.qq_group.empty()) {
            return std::nullopt;
        }
        return contact;
    } catch (...) {
        return std::nullopt;
    }
}

void LoadStandaloneImGuiSettings(const std::filesystem::path& root) noexcept {
    ImGui::GetIO().IniFilename = nullptr;
    try {
        std::ifstream input(root / L"cabbird-imgui.ini", std::ios::binary);
        const std::string settings{std::istreambuf_iterator<char>(input), {}};
        if (!settings.empty()) ImGui::LoadIniSettingsFromMemory(settings.data(), settings.size());
    } catch (...) { }
}

void SaveStandaloneImGuiSettings(const std::filesystem::path& root) noexcept {
    // RunPlatform owns this context on its UI worker. Keep filesystem work
    // outside NewFrame/Draw, as in the embedded host's memory-only frame path.
    ImGui::GetIO().WantSaveIniSettings = false;
    try {
        std::size_t size{};
        const char* const settings = ImGui::SaveIniSettingsToMemory(&size);
        std::ofstream output(root / L"cabbird-imgui.ini", std::ios::binary | std::ios::trunc);
        output.write(settings, static_cast<std::streamsize>(size));
    } catch (...) { }
}

struct ThemeImagePicker final {
    std::stop_token stop;
    std::atomic<HWND> dialog{};
};

UINT_PTR CALLBACK ThemeImagePickerHook(HWND hook, UINT message, WPARAM, LPARAM parameter) {
    if (message == WM_INITDIALOG) {
        const auto* request = reinterpret_cast<const OPENFILENAMEW*>(parameter);
        auto* state = reinterpret_cast<ThemeImagePicker*>(request->lCustData);
        SetWindowLongPtrW(hook, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->dialog.store(GetParent(hook), std::memory_order_release);
        if (state->stop.stop_requested()) PostMessageW(GetParent(hook), WM_CLOSE, 0, 0);
    } else if (message == WM_DESTROY) {
        if (auto* state = reinterpret_cast<ThemeImagePicker*>(GetWindowLongPtrW(hook, GWLP_USERDATA))) {
            state->dialog.store(nullptr, std::memory_order_release);
        }
    }
    return 0;
}

std::filesystem::path ChooseThemeImage(std::stop_token stop, std::string& error) {
    ThemeImagePicker state{stop};
    std::stop_callback cancel(stop, [&state] {
        if (const HWND dialog = state.dialog.load(std::memory_order_acquire))
            PostMessageW(dialog, WM_CLOSE, 0, 0);
    });
    std::array<wchar_t, 32768> path{};
    OPENFILENAMEW request{};
    request.lStructSize = sizeof(request);
    const HWND owner = GetForegroundWindow();
    DWORD owner_process{};
    if (owner != nullptr && GetWindowThreadProcessId(owner, &owner_process) != 0 &&
        owner_process == GetCurrentProcessId()) request.hwndOwner = owner;
    request.lpstrFile = path.data();
    request.nMaxFile = static_cast<DWORD>(path.size());
    request.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All files\0*.*\0";
    request.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
        OFN_NOCHANGEDIR | OFN_ENABLEHOOK;
    request.lpfnHook = ThemeImagePickerHook;
    request.lCustData = reinterpret_cast<LPARAM>(&state);
    if (stop.stop_requested()) return {};
    if (GetOpenFileNameW(&request)) return path.data();
    if (CommDlgExtendedError() != 0) error = "the image picker could not be opened";
    return {};
}

bool ImportThemeImage(const std::filesystem::path& root, const std::filesystem::path& source,
    std::string& relative, std::string& error) {
    auto read = cabbird::ReadUiResourceBytes(source, cabbird::kDefaultUiResourceEncodedByteLimit);
    if (!read || !cabbird::DecodeUiImageRgba8(read.bytes)) {
        error = "the selected image could not be decoded";
        return false;
    }
    std::error_code code;
    const auto local = std::filesystem::relative(source, root, code);
    std::filesystem::path checked;
    if (!code && cabbird::ResolvePlatformUiLayoutPath(root, local.generic_string(), checked)) {
        relative = local.generic_string();
        return true;
    }
    std::uint64_t digest = 1469598103934665603ULL;
    for (const auto byte : read.bytes) { digest ^= byte; digest *= 1099511628211ULL; }
    char name[32]{};
    std::snprintf(name, sizeof(name), "background-%016llx", static_cast<unsigned long long>(digest));
    const auto directory = root / L"assets" / L"custom";
    std::filesystem::create_directories(directory, code);
    const auto target = directory / (std::string(name) + source.extension().string());
    if (code) { error = "the background directory could not be created"; return false; }
    relative = "assets/custom/" + target.filename().generic_string();
    cabbird::ReliableStorage storage(root.native());
    if (!storage.WriteAtomic(std::filesystem::path(relative).native(), std::as_bytes(std::span(read.bytes)))) {
        error = "the background image could not be stored";
        return false;
    }
    return true;
}

[[nodiscard]] std::vector<std::uint8_t> LoadEmbeddedLogoBytes() noexcept {
    try {
        const auto module = reinterpret_cast<HMODULE>(&__ImageBase);
        const HRSRC resource = FindResourceW(
            module, MAKEINTRESOURCEW(kPlatformLogoResourceId), RT_RCDATA);
        if (resource == nullptr) return {};
        const HGLOBAL loaded = LoadResource(module, resource);
        const DWORD size = SizeofResource(module, resource);
        const void* const bytes = loaded == nullptr ? nullptr : LockResource(loaded);
        if (bytes == nullptr || size == 0) return {};
        const auto* const first = static_cast<const std::uint8_t*>(bytes);
        return {first, first + size};
    } catch (...) {
        return {};
    }
}

[[nodiscard]] bool StartsWith(
    const std::string_view value, const std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] bool OpenExternalUrl(const std::string& url) noexcept {
    if (!StartsWith(url, "https://") && !StartsWith(url, "http://")) return false;
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, url.c_str(), static_cast<int>(url.size()),
        nullptr, 0);
    if (required <= 0) return false;
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, url.c_str(), static_cast<int>(url.size()),
            wide.data(), required) != required) {
        return false;
    }
    return reinterpret_cast<INT_PTR>(ShellExecuteW(
               nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

// The game path uploads this host-owned image through the D3D12 resource
// backend. The standalone D3D11 host has no generic resource backend, so keep
// an equivalent local shader-resource view for the same embedded bytes.
[[nodiscard]] bool CreateStandaloneHeaderLogo(HostWindow& host) noexcept {
    try {
        if (host.device == nullptr) return false;
        const std::vector<std::uint8_t> encoded = LoadEmbeddedLogoBytes();
        const cabbird::UiImageDecodeResult decoded = cabbird::DecodeUiImageRgba8(encoded);
        if (!decoded || decoded.image.width == 0 || decoded.image.height == 0 ||
            decoded.image.pixels.empty()) {
            return false;
        }

        D3D11_TEXTURE2D_DESC description{};
        description.Width = decoded.image.width;
        description.Height = decoded.image.height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = decoded.image.pixels.data();
        data.SysMemPitch = decoded.image.width * 4U;

        ID3D11Texture2D* texture{};
        if (FAILED(host.device->CreateTexture2D(&description, &data, &texture)) || texture == nullptr) {
            return false;
        }
        ID3D11ShaderResourceView* view{};
        const HRESULT result = host.device->CreateShaderResourceView(texture, nullptr, &view);
        texture->Release();
        if (FAILED(result) || view == nullptr) return false;

        if (host.header_logo != nullptr) host.header_logo->Release();
        host.header_logo = view;
        g_standalone_header_logo = view;
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool DrawStandaloneHeaderLogo(const float width, const float height) noexcept {
    if (g_standalone_header_logo == nullptr) return false;
    ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(g_standalone_header_logo)),
        ImVec2(width, height));
    return true;
}

cabbird::UiWindowRequest PlatformUiWindowRequest() {
    cabbird::UiWindowRequest request;
    request.id = std::string(kPlatformUiWindowId);
    request.title = "Cabbird Plugin Platform";
    request.persist_settings = true;
    request.initial_width = kPlatformInitialShellWidth;
    request.initial_height = kPlatformInitialShellHeight;
    // Viewport-specific minimum and maximum dimensions are applied while
    // drawing. Persisting them here would reject a valid size on a smaller
    // display or after a DPI change.
    request.constraints = {};
    request.default_open = true;
    return request;
}

class PlatformUi final : public std::enable_shared_from_this<PlatformUi> {
public:
    PlatformUi(
        PluginManager& plugins,
        PlatformDiagnostics diagnostics,
        cabbird::PlatformUiState state = {},
        std::shared_ptr<PluginManager> plugin_owner = {})
        : plugins_(plugins), plugin_owner_(std::move(plugin_owner)),
          management_window_scope_(std::make_shared<cabbird::PluginScope>(
              std::make_shared<cabbird::ResourceLedger>(),
              std::string(kPlatformUiWindowOwner), kPlatformUiWindowGeneration)),
          diagnostics_(std::move(diagnostics)) {
        if (diagnostics_.translator == nullptr) {
            diagnostics_.translator =
                cabbird::ParseHostCatalog(cabbird::Locale::EnUs).translator;
        }
        cabbird::SetHostUiDeveloperMode(false);
        // Keep route, tab and selection outside the renderer-owned ImGui
        // context so a device rebuild does not reset the management surface.
        model_.State() = std::move(state);
        model_.State().favorite_plugin_ids = plugins_.FavoritePlugins();
        if (model_.State().route == Route::UnityCompatibility) {
            model_.State().route = Route::Diagnostics;
        }
        SyncInputBuffers();
        cabbird::UiTextureRequest logo_request;
        logo_request.encoded_bytes = LoadEmbeddedLogoBytes();
        logo_request.format = cabbird::UiTextureFormat::Auto;
        if (!logo_request.encoded_bytes.empty()) {
            logo_texture_ = plugins_.UiResources().RequestTexture(
                management_window_scope_, std::move(logo_request));
            if (logo_texture_) {
                static_cast<void>(plugins_.QueueUiTextureLoad(
                    management_window_scope_, logo_texture_));
            }
        }
        const std::filesystem::path directory = plugins_.Directory();
        const cabbird::Locale locale = diagnostics_.translator->locale();
        const auto contact_path = diagnostics_.runtime_root / L"CONTACT";
        catalog_worker_ = std::jthread([this, directory, locale, contact_path](std::stop_token stop_token) {
            const auto contact = LoadContactInformation(contact_path);
            struct ChangeNotification final {
                HANDLE contents{INVALID_HANDLE_VALUE};
                HANDLE parent{INVALID_HANDLE_VALUE};
                ~ChangeNotification() { Close(contents); Close(parent); }
                static void Close(HANDLE& handle) noexcept {
                    if (handle != INVALID_HANDLE_VALUE) FindCloseChangeNotification(handle);
                    handle = INVALID_HANDLE_VALUE;
                }
                void Reset() noexcept { Close(contents); Close(parent); }
            } change;
            struct StopEvent final {
                HANDLE handle{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
                ~StopEvent() { if (handle != nullptr) CloseHandle(handle); }
            } stop;
            std::stop_callback wake(stop_token, [&stop] {
                if (stop.handle != nullptr) SetEvent(stop.handle);
            });
            while (!stop_token.stop_requested()) {
                // Watch the parent too: notifications on a directory do not
                // report replacement of that directory itself. Arm before scanning.
                if (change.parent == INVALID_HANDLE_VALUE) {
                    change.parent = FindFirstChangeNotificationW(directory.parent_path().c_str(), FALSE,
                        FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES);
                }
                if (change.contents == INVALID_HANDLE_VALUE) {
                    change.contents = FindFirstChangeNotificationW(directory.c_str(), TRUE,
                        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                        FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                        FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_ATTRIBUTES);
                }
                try {
                    auto next = std::make_shared<CatalogCache>();
                    next->contact = contact;
                    next->catalog = cabbird::DiscoverPluginCatalog(directory);
                    next->dependencies = cabbird::ResolvePluginDependencies(*next->catalog);
                    for (const auto& entry : next->catalog->Entries()) {
                        if (!entry.manifest) continue;
                        const auto localization = cabbird::LoadPluginCatalog(
                            locale, entry.package_root);
                        if (localization.catalog == nullptr) continue;
                        auto name = localization.catalog->Translate(
                            "window.title", entry.manifest->name, {});
                        if (!name.text.empty()) {
                            next->display_names.emplace(entry.manifest->id, std::move(name.text));
                        }
                        auto description = localization.catalog->Translate(
                            "plugin.description", entry.manifest->description, {});
                        if (!description.text.empty()) {
                            next->descriptions.emplace(
                                entry.manifest->id, std::move(description.text));
                        }
                    }
                    {
                        std::scoped_lock lock(catalog_mutex_);
                        catalog_cache_ = std::move(next);
                    }
                } catch (...) {
                    // Keep the last good catalog visible when a package is being
                    // replaced or a manifest is temporarily unreadable.
                }
                if (change.parent != INVALID_HANDLE_VALUE && stop.handle != nullptr) {
                    const HANDLE handles[]{stop.handle, change.parent, change.contents};
                    const DWORD count = change.contents == INVALID_HANDLE_VALUE ? 2 : 3;
                    const DWORD reason = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
                    if (reason == WAIT_OBJECT_0 || stop_token.stop_requested()) break;
                    if (reason == WAIT_OBJECT_0 + 1) {
                        if (!FindNextChangeNotification(change.parent)) ChangeNotification::Close(change.parent);
                        ChangeNotification::Close(change.contents);
                        continue;
                    }
                    if (reason == WAIT_OBJECT_0 + 2) {
                        if (!FindNextChangeNotification(change.contents)) ChangeNotification::Close(change.contents);
                        continue;
                    }
                    change.Reset();
                }
                // If notifications fail, retry off Render without leaving a
                // permanently stale cache or spinning on a failed wait handle.
                for (int tick = 0; tick != 10 && !stop_token.stop_requested(); ++tick) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });
        management_window_ = plugins_.UiResources().RegisterWindow(
            management_window_scope_, PlatformUiWindowRequest());
        if (management_window_) {
            if (const auto window = plugins_.UiResources().WindowState(
                    management_window_scope_, management_window_)) {
                if (!window->open) cabbird::SetHostUiMenusCollapsed(true);
                if (window->width > 0.0F && window->height > 0.0F) {
                    management_shell_expanded_size_ = {window->width, window->height};
                    if (window->width < kPlatformMinimumShellWidth ||
                        window->height < kPlatformMinimumShellHeight) {
                        management_shell_expanded_size_ = {
                            kPlatformInitialShellWidth, kPlatformInitialShellHeight};
                    }
                }
            }
        }
        StartThemeWorker();
    }

    ~PlatformUi() {
        catalog_worker_.request_stop();
        theme_worker_.request_stop();
        RevokeManagementWindow();
    }

    [[nodiscard]] bool Ready() const noexcept { return static_cast<bool>(management_window_); }

    [[nodiscard]] bool Reveal() noexcept {
        std::scoped_lock lifetime_lock(lifetime_mutex_);
        if (closing_) return false;
        try {
            const auto window = plugins_.UiResources().WindowState(
                management_window_scope_, management_window_);
            return window && !window->open && plugins_.UiResources().OpenWindow(
                management_window_scope_, management_window_);
        } catch (...) {
            return false;
        }
    }

    void ExpandManagementShell() noexcept {
        std::scoped_lock operation_lock(operation_mutex_);
        management_shell_collapsed_ = false;
    }

    [[nodiscard]] bool BelongsTo(const PluginManager& plugins) const noexcept {
        return &plugins_ == &plugins;
    }

    [[nodiscard]] bool CapturingSettingsHotkey() {
        std::scoped_lock operation_lock(operation_mutex_);
        return settings_hotkey_capture_;
    }

    [[nodiscard]] bool CanReap() const noexcept {
        std::scoped_lock lock(lifetime_mutex_);
        return active_callbacks_ == 0 && outstanding_invocations_ == 0;
    }

    [[nodiscard]] bool Closing() const noexcept {
        std::scoped_lock lock(lifetime_mutex_);
        return closing_;
    }

    void Quarantine() noexcept {
        cabbird::SetHostUiDeveloperMode(false);
        {
            std::scoped_lock lock(lifetime_mutex_);
            closing_ = true;
        }
        catalog_worker_.request_stop();
        theme_worker_.request_stop();
        RevokeManagementWindow();
    }

    [[nodiscard]] cabbird::PlatformUiState State() const {
        std::scoped_lock lock(operation_mutex_);
        return model_.State();
    }

    void Flush() {
        std::scoped_lock submission_lock(submission_mutex_);
        {
            std::scoped_lock lock(lifetime_mutex_);
            if (closing_) return;
        }
        FlushActions();
    }

    // Stop accepting new callbacks before teardown. A shared owner captured by
    // an already-running callback keeps this object alive until it returns.
    struct ShutdownResult final {
        std::optional<cabbird::PlatformUiState> state;
        bool callbacks_drained{};
    };

    [[nodiscard]] ShutdownResult BeginShutdown(
        std::chrono::milliseconds drain_timeout = std::chrono::seconds(5)) {
        cabbird::SetHostUiDeveloperMode(false);
        const auto bounded_timeout = (std::max)(drain_timeout, std::chrono::milliseconds::zero());
        const auto deadline = bounded_timeout == std::chrono::milliseconds::max()
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() + bounded_timeout;
        const auto remaining = [&] {
            if (deadline == std::chrono::steady_clock::time_point::max()) {
                return std::chrono::milliseconds::max();
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return std::chrono::milliseconds::zero();
            return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        };
        {
            // Serialize with a frame that is already drawing or flushing,
            // then release the submission gate before waiting on lifecycle
            // callbacks. A callback may re-enter the public Flush entrypoint;
            // keeping this mutex held while draining would deadlock it.
            std::scoped_lock submission_lock(submission_mutex_);
            std::scoped_lock lock(lifetime_mutex_);
            closing_ = true;
        }
        // A retired owner must not keep scanning the package directory while
        // its last lifecycle callback is being drained. The owner itself is
        // retained by the global handoff below until this method succeeds.
        catalog_worker_.request_stop();
        theme_worker_.request_stop();
        if (diagnostics_.lifecycle_drain) {
            const bool drained = diagnostics_.lifecycle_drain(remaining());
            if (!drained) {
                // A callback may be executing plugin code beyond the host
                // deadline. Keep this owner as a quarantine fence and let the
                // renderer release its current ImGui/D3D generation; the
                // owner and plugin manager remain mapped until callback drain
                // is observed by a later process boundary.
                return QuarantineResult();
            }
        }
        // Submission is closed before this point, so no new Draw/Flush call
        // can acquire the operation lock. Wait for callback scopes first;
        // callbacks release operation_mutex_ before taking lifetime_mutex_, so
        // taking the locks in the opposite order here would deadlock teardown.
        {
            std::unique_lock lifetime_lock(lifetime_mutex_);
            if (!lifetime_condition_.wait_for(
                    lifetime_lock, remaining(), [this] {
                        return active_callbacks_ == 0 && outstanding_invocations_ == 0;
                    })) {
                return QuarantineResult();
            }
        }
        std::scoped_lock operation_lock(operation_mutex_);
        CancelOutstandingOperations();
        CancelMemoryWork();
        RevokeManagementWindow();
        return {model_.State(), true};
    }

    void Draw() {
        const bool measure = PerformanceDiagnosticsEnabled();
        auto phase_started = measure ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
        std::scoped_lock submission_lock(submission_mutex_);
        RecordPerformance(
            PlatformUiPerformanceStage::SubmissionLockWait, phase_started);
        {
            std::scoped_lock lock(lifetime_mutex_);
            if (closing_) return;
        }
        phase_started = measure ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
        std::scoped_lock operation_lock(operation_mutex_);
        RecordPerformance(
            PlatformUiPerformanceStage::OperationLockWait, phase_started);
        phase_started = measure ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
        const auto window = plugins_.UiResources().WindowState(
            management_window_scope_, management_window_);
        RecordPerformance(PlatformUiPerformanceStage::WindowState, phase_started);
        if (!window || !window->open) {
            settings_hotkey_capture_ = false;
            return;
        }
        settings_hotkey_capture_drawn_ = false;
        RefreshSnapshot(measure);
        ApplySettingsPreview();
        phase_started = measure ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
        UpdateStatusToast();
        const CabbirdUiServiceV1* ui = cabbird::HostUiServiceTable();
        if (ui == nullptr || ui->set_next_window_size == nullptr || ui->begin_window == nullptr ||
            ui->end_window == nullptr || ui->set_next_window_size_constraints == nullptr ||
            ui->get_window_size == nullptr) {
            return;
        }
        const ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) return;
        const float ui_scale = PlatformUiScale();
        const bool ui_scale_changed = std::abs(ui_scale - management_shell_ui_scale_) > 0.0001f;
        // Keep a consistent margin around the responsive product surface and
        // scale both the viewport bounds and the persisted logical size.
        const bool fill_viewport = diagnostics_.standalone_window;
        const float shell_margin = fill_viewport ? 0.0f : Scaled(kPlatformShellViewportMargin);
        // The bounds have to answer to the viewport, not only to the DPI scale.
        // A 1180x700 surface at 125% asks for 1475x875, which overflows a
        // 1280x720 client area, and the overflow is silently clipped -- the
        // shell then looks confined to a box no matter how large the window is.
        const float available_width =
            std::max(io.DisplaySize.x - shell_margin * 2.0f, 1.0f);
        const float available_height =
            std::max(io.DisplaySize.y - shell_margin * 2.0f, 1.0f);
        const float maximum_shell_width = fill_viewport ? available_width
            : std::min(Scaled(kPlatformMaximumShellWidth), available_width);
        const float maximum_shell_height = fill_viewport ? available_height
            : std::min(Scaled(kPlatformMaximumShellHeight), available_height);
        // Kept below the maxima above: a viewport smaller than the minimum
        // would otherwise hand std::clamp an inverted range.
        const float minimum_shell_width =
            std::min(Scaled(kPlatformMinimumShellWidth), maximum_shell_width);
        const float minimum_shell_height =
            std::min(Scaled(kPlatformMinimumShellHeight), maximum_shell_height);
        // The embedded shell keeps its logical initial or saved size. Viewport
        // changes only clamp that size; a standalone host owns its whole client.
        const ImVec2 viewport_logical{
            available_width / ui_scale, available_height / ui_scale};
        const bool viewport_changed =
            std::abs(viewport_logical.x - management_shell_viewport_.x) > 0.5f ||
            std::abs(viewport_logical.y - management_shell_viewport_.y) > 0.5f;
        if (fill_viewport) {
            management_shell_expanded_size_ = viewport_logical;
        }
        management_shell_viewport_ = viewport_logical;
        ImVec2 logical_expanded_size = management_shell_expanded_size_;
        if (logical_expanded_size.x <= 0.0f || logical_expanded_size.y <= 0.0f) {
            logical_expanded_size = {
                kPlatformInitialShellWidth, kPlatformInitialShellHeight};
        }
        ImVec2 expanded_size{
            logical_expanded_size.x * ui_scale, logical_expanded_size.y * ui_scale};
        expanded_size.x = std::clamp(expanded_size.x, minimum_shell_width, maximum_shell_width);
        expanded_size.y = std::clamp(expanded_size.y, minimum_shell_height, maximum_shell_height);
        const bool globally_collapsed = ManagementShellGloballyCollapsed();
        const bool shell_collapsed = ManagementShellCollapsed();
        const bool restoring_from_collapse = management_shell_was_collapsed_ && !shell_collapsed;
        const float shell_width = globally_collapsed
            ? std::min(Scaled(kPlatformGloballyCollapsedWidth), maximum_shell_width)
            : expanded_size.x;
        const float shell_height = shell_collapsed
            ? std::min(Scaled(kPlatformHeaderHeight), maximum_shell_height) : expanded_size.y;
        ImGui::SetNextWindowPos(
            ImVec2(shell_margin, shell_margin),
            fill_viewport ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
        if (globally_collapsed) {
            ui->set_next_window_size_constraints(
                ui->user, shell_width, shell_height, shell_width, shell_height);
            ui->set_next_window_size(
                ui->user, shell_width, shell_height, static_cast<std::uint32_t>(ImGuiCond_Always));
        } else if (management_shell_collapsed_) {
            ui->set_next_window_size_constraints(
                ui->user, minimum_shell_width, shell_height, maximum_shell_width, shell_height);
            ui->set_next_window_size(
                ui->user, expanded_size.x, shell_height,
                static_cast<std::uint32_t>(ImGuiCond_Always));
        } else {
            ui->set_next_window_size_constraints(
                ui->user, fill_viewport ? expanded_size.x : minimum_shell_width,
                fill_viewport ? expanded_size.y : minimum_shell_height,
                maximum_shell_width, maximum_shell_height);
            if (fill_viewport || viewport_changed || management_shell_apply_initial_size_ || restoring_from_collapse ||
                ui_scale_changed) {
                ui->set_next_window_size(
                    ui->user, expanded_size.x, expanded_size.y,
                    static_cast<std::uint32_t>(ImGuiCond_Always));
            }
        }
        const std::string title = window->title + "###" + window->stable_id;
        // Keep the native host title bar only. The shell owns its outer border,
        // its 4px layout grid, and every internal fixed-height band.
        const float interface_alpha = settings_draft_
            ? static_cast<float>(settings_draft_->interface_opacity_percent) / 100.0f
            : 1.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, interface_alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool visible = ui->begin_window(
            ui->user, {title.data(), title.size()}, nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground |
                (fill_viewport ? ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize : 0)) != 0;
        ImGui::PopStyleVar();
        if (visible) {
            const ImVec2 management_window_origin = ImGui::GetWindowPos();
            const ImVec2 management_window_size = ImGui::GetWindowSize();
            // The canvas belongs to the host, not the game's backbuffer. The
            // local glass controls may refract this surface, but never reveal
            // raw gameplay underneath an otherwise opaque management window.
            ImGui::GetWindowDrawList()->AddRectFilled(management_window_origin,
                Offset(management_window_origin, management_window_size.x, management_window_size.y),
                ImGui::GetColorU32(ThemeColor(cabbird::CabbirdUiSurfaceColor())),
                ImGui::GetStyle().WindowRounding);
            DrawThemeImages(management_window_origin, management_window_size, false);
            if (!ManagementShellCollapsed()) DrawThemeImages(management_window_origin, management_window_size, true);
            management_shell_locked_ = cabbird::HostUiCurrentWindowLocked();
            UpdateLayout();
            HandleShellShortcuts();
            const bool shell_was_collapsed = ManagementShellCollapsed();
            cabbird::glass::CaptureBackdrop(ImGui::GetWindowDrawList());
            RecordPerformance(PlatformUiPerformanceStage::FrameSetup, phase_started);
            phase_started = measure ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
            DrawManagementShell();
            RecordPerformance(PlatformUiPerformanceStage::ManagementShell, phase_started);
            phase_started = measure ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
            if (!shell_was_collapsed && !ManagementShellCollapsed()) {
                DrawConfirmationPopup();
                DrawRepositoryUninstallPopup();
                DrawOperationDetailsPopup();
                DrawMemoryConfirmationPopup();
                DrawSettingsLeavePopup();
            }
            DrawStatusToast(management_window_origin, management_window_size);
            RecordPerformance(PlatformUiPerformanceStage::Popups, phase_started);
            phase_started = measure ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
        }
        if (!shell_collapsed && !ManagementShellCollapsed()) {
            float width{};
            float height{};
            ui->get_window_size(ui->user, &width, &height);
            if (width > 0.0f && height > 0.0f) {
                const float maximum_logical_width = maximum_shell_width / ui_scale;
                const float maximum_logical_height = maximum_shell_height / ui_scale;
                const bool width_was_clamped =
                    management_shell_expanded_size_.x > maximum_logical_width + 0.5f &&
                    width >= maximum_shell_width - 0.5f;
                const bool height_was_clamped =
                    management_shell_expanded_size_.y > maximum_logical_height + 0.5f &&
                    height >= maximum_shell_height - 0.5f;
                if (!width_was_clamped) management_shell_expanded_size_.x = width / ui_scale;
                if (!height_was_clamped) management_shell_expanded_size_.y = height / ui_scale;
            }
            if (!fill_viewport) {
                static_cast<void>(plugins_.UiResources().SetWindowSize(
                    management_window_scope_, management_window_,
                    management_shell_expanded_size_.x, management_shell_expanded_size_.y));
            }
        }
        if (!globally_collapsed) management_shell_apply_initial_size_ = false;
        management_shell_was_collapsed_ = shell_collapsed;
        management_shell_ui_scale_ = ui_scale;
        ui->end_window(ui->user);
        ImGui::PopStyleVar();
        if (!settings_hotkey_capture_drawn_ || ManagementShellCollapsed()) {
            settings_hotkey_capture_ = false;
        }
        RecordPerformance(PlatformUiPerformanceStage::WindowPersist, phase_started);

        // Mutations are flushed by FlushPlatformUiActions after the render
        // lock is released. Draw itself only consumes a snapshot and queues
        // typed intents.
    }

    void PrepareResources() noexcept {
        std::scoped_lock submission_lock(submission_mutex_);
        std::scoped_lock operation_lock(operation_mutex_);
        std::shared_ptr<cabbird::PluginScope> scope;
        cabbird::UiResourceHandle texture;
        {
            std::scoped_lock lifetime_lock(lifetime_mutex_);
            if (closing_) return;
            scope = management_window_scope_;
            texture = logo_texture_;
        }
        plugins_.PrepareUiTexture(scope, texture);
        ApplyThemeResources();
        const auto prepare = [&](const ThemeImage& image) {
            if (image.handle) plugins_.PrepareUiTexture(scope, image.handle);
        };
        prepare(theme_background_);
        for (const auto& sticker : theme_stickers_) prepare(sticker);
        const bool wallpaper = theme_background_.Valid() &&
            theme_layout_.background.mode != cabbird::PlatformUiBackgroundMode::Disabled &&
            theme_layout_.background.opacity > 0.0f;
        const float surface_alpha = wallpaper ? theme_layout_.surface_opacity : 1.0f;
        if (std::abs(cabbird::GetCabbirdUiSurfaceAlpha() - surface_alpha) > 0.0001f) {
            cabbird::SetCabbirdUiSurfaceAlpha(surface_alpha);
            ApplyCabbirdUiStyle();
        }
    }

private:
    struct ThemeImage final {
        cabbird::UiResourceHandle handle;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> standalone;
        float width{};
        float height{};
        [[nodiscard]] bool Valid() const noexcept { return static_cast<bool>(handle) || standalone != nullptr; }
    };

    void StartThemeWorker() {
        theme_requested_palette_ = cabbird::GetCabbirdUiPalette();
        const auto root = diagnostics_.runtime_root;
        theme_worker_ = std::jthread([this, root](std::stop_token stop) {
            std::filesystem::path previous_path;
            std::filesystem::file_time_type previous_stamp{}, previous_shared_stamp{};
            std::uint64_t revision{};
            for (;;) {
                cabbird::CabbirdUiPalette palette;
                std::optional<int> mode;
                std::optional<float> opacity;
                bool pick{};
                bool reload{};
                {
                    std::unique_lock lock(theme_mutex_);
                    theme_changed_.wait_for(lock, stop, std::chrono::seconds(1), [this] {
                        return theme_reload_ || theme_mode_request_ || theme_opacity_request_ || theme_pick_request_;
                    });
                    palette = theme_requested_palette_;
                    reload = std::exchange(theme_reload_, false);
                    mode = std::exchange(theme_mode_request_, std::nullopt);
                    opacity = std::exchange(theme_opacity_request_, std::nullopt);
                    pick = std::exchange(theme_pick_request_, false) && !stop.stop_requested();
                    theme_picker_running_ = pick;
                    // Submission is closed before stop is requested. Drain the
                    // already queued settings without opening another picker.
                    if (stop.stop_requested() && !mode && !opacity) break;
                }
                try {
                    const auto shared = root / std::filesystem::path(cabbird::kPlatformUiLayoutRelativePath);
                    std::string failure;
                    if (mode || opacity || pick) {
                        cabbird::PlatformUiLayoutDocument layout;
                        if (auto read = cabbird::ReadUiResourceBytes(shared, cabbird::kPlatformUiLayoutMaximumBytes)) {
                            auto parsed = cabbird::ParsePlatformUiLayout(std::string_view(
                                reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size()));
                            if (parsed.ok) layout = std::move(parsed.layout);
                            else failure = std::move(parsed.message);
                        }
                        bool changed = false;
                        if (failure.empty() && pick && !stop.stop_requested()) {
                            const auto chosen = ChooseThemeImage(stop, failure);
                            if (!chosen.empty() && !stop.stop_requested()) {
                                std::string relative;
                                if (ImportThemeImage(root, chosen, relative, failure)) {
                                    layout.background.path = std::move(relative);
                                    layout.background.mode = cabbird::PlatformUiBackgroundMode::Crop;
                                    layout.surface_opacity = cabbird::kPlatformUiLayoutSurfaceOpacityDefault;
                                    changed = true;
                                }
                            }
                        }
                        if (failure.empty() && mode) {
                            layout.background.mode = static_cast<cabbird::PlatformUiBackgroundMode>(*mode);
                            changed = true;
                        }
                        if (failure.empty() && opacity) { layout.surface_opacity = *opacity; changed = true; }
                        if (changed)
                            static_cast<void>(cabbird::WritePlatformUiLayout(shared, layout, failure));
                        reload = reload || changed || !failure.empty();
                    }
                    if (stop.stop_requested()) {
                        std::scoped_lock lock(theme_mutex_);
                        theme_picker_running_ = false;
                        continue;
                    }
                    const auto path = cabbird::ResolvePlatformUiLayoutFile(root, cabbird::ToString(palette));
                    std::error_code error;
                    const auto stamp = std::filesystem::last_write_time(path, error);
                    error.clear();
                    const auto shared_stamp = std::filesystem::last_write_time(shared, error);
                    if (reload || path != previous_path || stamp != previous_stamp || shared_stamp != previous_shared_stamp) {
                        auto prepared = std::make_shared<cabbird::PlatformUiLayoutSnapshot>(
                            cabbird::PreparePlatformUiLayout(root, path, ++revision));
                        if (!failure.empty()) prepared->message = std::move(failure);
                        std::scoped_lock lock(theme_mutex_);
                        theme_published_ = std::move(prepared);
                        previous_path = path;
                        previous_stamp = stamp;
                        previous_shared_stamp = shared_stamp;
                    }
                } catch (...) {
                    auto failure = std::make_shared<cabbird::PlatformUiLayoutSnapshot>();
                    failure->revision = ++revision;
                    failure->message = "the interface background could not be prepared";
                    std::scoped_lock lock(theme_mutex_);
                    theme_published_ = std::move(failure);
                }
                { std::scoped_lock lock(theme_mutex_); theme_picker_running_ = false; }
            }
        });
    }

    void ReleaseThemeResources() noexcept {
        const auto release = [this](const ThemeImage& image) {
            if (image.handle) static_cast<void>(plugins_.UiResources().Release(management_window_scope_, image.handle));
        };
        release(theme_background_);
        for (const auto& sticker : theme_stickers_) release(sticker);
        theme_background_ = {};
        theme_stickers_.clear();
    }

    ThemeImage ResolveThemeImage(const cabbird::UiRgba8Image& image) {
        ThemeImage result;
        if (image.width == 0 || image.height == 0 || image.pixels.empty()) return result;
        result.width = static_cast<float>(image.width);
        result.height = static_cast<float>(image.height);
        if (g_window != nullptr && g_window->device != nullptr) {
            D3D11_TEXTURE2D_DESC description{};
            description.Width = image.width; description.Height = image.height;
            description.MipLevels = description.ArraySize = 1;
            description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA pixels{image.pixels.data(), image.width * 4U, 0};
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(g_window->device->CreateTexture2D(&description, &pixels, &texture)))
                static_cast<void>(g_window->device->CreateShaderResourceView(texture.Get(), nullptr, &result.standalone));
        } else {
            cabbird::UiTextureRequest request;
            request.format = cabbird::UiTextureFormat::Rgba8;
            request.width = image.width; request.height = image.height;
            request.encoded_bytes = image.pixels;
            result.handle = plugins_.UiResources().RequestTexture(management_window_scope_, std::move(request));
            if (result.handle) static_cast<void>(plugins_.QueueUiTextureLoad(management_window_scope_, result.handle));
        }
        return result;
    }

    void ApplyThemeResources() noexcept {
        try {
            std::shared_ptr<const cabbird::PlatformUiLayoutSnapshot> prepared;
            { std::scoped_lock lock(theme_mutex_); prepared = std::exchange(theme_published_, nullptr); }
            if (!prepared) return;
            ReleaseThemeResources();
            theme_layout_ = prepared->layout;
            theme_layout_available_ = prepared->available;
            theme_layout_message_ = prepared->message;
            theme_background_ = ResolveThemeImage(prepared->background);
            theme_stickers_.reserve(prepared->stickers.size());
            for (const auto& sticker : prepared->stickers) theme_stickers_.push_back(ResolveThemeImage(sticker));
        } catch (...) { theme_layout_message_ = "the interface background could not be uploaded"; }
    }

    void EmitThemeImage(const ThemeImage& image, const cabbird::UiTextureDrawRequest& request,
        ImDrawList* const list) {
        if (image.handle) {
            static_cast<void>(plugins_.DrawUiTextureEx(management_window_scope_, image.handle, request));
        } else if (image.standalone != nullptr) {
            const auto texture = reinterpret_cast<ImTextureID>(image.standalone.Get());
            if (request.rotation_degrees == 0.0f) {
                list->AddImageRounded(texture, {request.x, request.y},
                    {request.x + request.width, request.y + request.height},
                    {request.uv0_x, request.uv0_y}, {request.uv1_x, request.uv1_y},
                    request.tint_rgba, request.rounding);
            } else {
                const cabbird::PlatformUiStickerPlacement placement{request.x, request.y,
                    request.width, request.height, request.rotation_degrees, request.uv0_x,
                    request.uv0_y, request.uv1_x, request.uv1_y, 1.0f};
                const auto quad = cabbird::ComputePlatformUiImageQuad(placement);
                list->AddImageQuad(texture, {quad.x[0], quad.y[0]}, {quad.x[1], quad.y[1]},
                    {quad.x[2], quad.y[2]}, {quad.x[3], quad.y[3]}, {quad.u[0], quad.v[0]},
                    {quad.u[1], quad.v[1]}, {quad.u[2], quad.v[2]}, {quad.u[3], quad.v[3]}, request.tint_rgba);
            }
        }
    }

    void DrawThemeImages(const ImVec2 origin, const ImVec2 size, const bool front) {
        if (!theme_layout_available_) return;
        ImDrawList* const list = front ? ImGui::GetForegroundDrawList() : ImGui::GetWindowDrawList();
        const auto tint = [](const cabbird::CabbirdUiColor& color, const float opacity) {
            return ImGui::GetColorU32(ImVec4(color.red, color.green, color.blue, color.alpha * opacity));
        };
        list->PushClipRect(origin, Offset(origin, size.x, size.y), true);
        const bool wallpaper = theme_background_.Valid() &&
            theme_layout_.background.mode != cabbird::PlatformUiBackgroundMode::Disabled;
        if (!front && wallpaper) {
            const auto placement = cabbird::ComputePlatformUiBackgroundPlacement(theme_layout_.background.mode,
                theme_background_.width, theme_background_.height, origin.x, origin.y, size.x, size.y);
            cabbird::UiTextureDrawRequest request;
            request.x = placement.x; request.y = placement.y;
            request.width = placement.width; request.height = placement.height;
            request.uv0_x = placement.uv0_x; request.uv0_y = placement.uv0_y;
            request.uv1_x = placement.uv1_x; request.uv1_y = placement.uv1_y;
            request.tint_rgba = tint(theme_layout_.background.tint, theme_layout_.background.opacity);
            if (theme_layout_.background.mode != cabbird::PlatformUiBackgroundMode::Center)
                request.rounding = ImGui::GetStyle().WindowRounding;
            EmitThemeImage(theme_background_, request, list);
            auto veil = cabbird::CabbirdUiSurfaceColor();
            list->AddRectFilled(origin, Offset(origin, size.x, size.y),
                tint(veil, theme_layout_.surface_opacity), ImGui::GetStyle().WindowRounding);
        }
        list->PopClipRect();
        if (ManagementShellCollapsed()) return;
        for (std::size_t index = 0; index < theme_layout_.stickers.size() && index < theme_stickers_.size(); ++index) {
            const auto& sticker = theme_layout_.stickers[index];
            if ((sticker.layer == cabbird::PlatformUiStickerLayer::Front) != front || !theme_stickers_[index].Valid()) continue;
            const auto placement = cabbird::ComputePlatformUiStickerPlacement(sticker,
                theme_stickers_[index].width, theme_stickers_[index].height, size.x, size.y);
            if (!placement.Visible()) continue;
            cabbird::UiTextureDrawRequest request;
            request.x = origin.x + placement.x; request.y = origin.y + placement.y;
            request.width = placement.width; request.height = placement.height;
            request.rotation_degrees = placement.rotation;
            request.uv0_x = placement.uv0_x; request.uv0_y = placement.uv0_y;
            request.uv1_x = placement.uv1_x; request.uv1_y = placement.uv1_y;
            request.tint_rgba = tint(sticker.tint, sticker.opacity); request.on_top = front;
            list->PushClipRectFullScreen();
            EmitThemeImage(theme_stickers_[index], request, list);
            list->PopClipRect();
        }
    }

    using Route = cabbird::PlatformUiRoute;
    using Tab = cabbird::PlatformUiPluginTab;
    using Filter = cabbird::PlatformUiPluginFilter;
    using Sort = cabbird::PlatformUiPluginSort;
    using DiagnosticTab = cabbird::PlatformUiDiagnosticsTab;
    using Intent = cabbird::PlatformUiIntent;
    using Mutation = cabbird::PlatformUiPluginMutation;

    enum class IntentExecutionState : std::uint8_t {
        Pending,
        Running,
        Settled,
    };

    enum class LayoutMode : std::uint8_t {
        Wide,
        Standard,
        Compact,
    };

    enum class DeveloperPanel : std::uint8_t {
        Plugins,
        Services,
        Hooks,
        Memory,
        UnityProfile,
    };

    enum class DeveloperPluginTab : std::uint8_t {
        Overview,
        Capabilities,
        Performance,
        Logs,
    };

    enum class SettingsSection : std::uint8_t {
        Interface,
        Input,
        Updates,
        Diagnostics,
        Advanced,
        About,
    };

    // One editable row in the third-party plugin source editor. The URL lives
    // in a fixed buffer because the host does not vendor imgui_stdlib.
    struct RepositoryChannelRow {
        std::array<char, 1024> url{};
        bool enabled{true};
    };

    struct SettingsApplyMailbox final {
        std::mutex mutex;
        std::optional<cabbird::PlatformSettingsApplyResult> result;
    };

    struct RepositoryConfigureResult final {
        cabbird::PluginRepositoryConfig config;
        cabbird::RepositoryOperationSubmission submission;
    };

    struct RepositoryConfigureMailbox final {
        std::mutex mutex;
        std::optional<RepositoryConfigureResult> result;
    };

    struct QueuedIntent final {
        Intent intent;
        std::uint64_t operation_id{};
        std::vector<cabbird::AffectedPlugin> affected;
        std::map<std::string, std::uint64_t, std::less<>> generations;
        std::shared_ptr<std::atomic<IntentExecutionState>> execution_state;
    };

    // Keeps the PlatformUi owner alive for the complete dispatcher invocation,
    // including the interval after Invoke reports a timeout but before the
    // queued callback is finally cancelled or completes.
    struct InvocationGuard final {
        std::shared_ptr<PlatformUi> owner;
        std::function<void()> on_abandon;
        std::atomic_bool callback_started{};
        ~InvocationGuard() {
            if (!callback_started.load(std::memory_order_acquire) && on_abandon) {
                try {
                    on_abandon();
                } catch (...) {
                }
            }
            if (owner != nullptr) owner->LeaveInvocation();
        }

        void MarkCallbackStarted() noexcept {
            callback_started.store(true, std::memory_order_release);
        }
    };

    struct CatalogCache final {
        std::optional<ContactInformation> contact;
        std::optional<cabbird::PluginCatalogSnapshot> catalog;
        std::optional<cabbird::PluginDependencyPlan> dependencies;
        cabbird::PluginDisplayNameMap display_names;
        cabbird::PluginDescriptionMap descriptions;
    };

    struct AwaitingBatch final {
        QueuedIntent queued;
        std::map<std::string, std::uint64_t, std::less<>> generations;
        struct Observation final {
            bool present{};
            std::uint64_t generation{};
            cabbird::PlatformUiPluginState state{cabbird::PlatformUiPluginState::Unknown};
            bool enabled{};

            friend bool operator==(const Observation&, const Observation&) = default;
        };
        std::map<std::string, Observation, std::less<>> last_observations;
        bool observed_once{};
        std::chrono::steady_clock::time_point deadline{};
    };

    struct PendingMemoryWrite final {
        std::uintptr_t address{};
        std::vector<std::uint8_t> bytes;
        bool patch{};
    };

    struct PerformanceRow final {
        const cabbird::InstalledPluginView* plugin{};
        const CallbackMetricsView* metrics{};
        bool update{};
    };

    [[nodiscard]] const char* Text(const cabbird::MessageId id) const noexcept {
        return diagnostics_.translator->Text(id).data();
    }

    [[nodiscard]] std::string Format(
        const cabbird::MessageId id,
        const std::span<const std::string_view> arguments) const {
        return diagnostics_.translator->Format(id, arguments);
    }

    [[nodiscard]] std::string StableLabel(
        const cabbird::MessageId id, const std::string_view stable_id) const {
        return cabbird::StableDisplayLabel(diagnostics_.translator->Text(id), stable_id);
    }

    const char* RouteLabel(Route route) const noexcept {
        switch (route) {
        case Route::Plugins: return Text(cabbird::MessageId::ShellRoutePlugins);
        case Route::UnityCompatibility:
            return Text(cabbird::MessageId::ShellRouteUnityCompatibility);
        case Route::Diagnostics: return Text(cabbird::MessageId::ShellRouteDiagnostics);
        case Route::Settings: return Text(cabbird::MessageId::ShellRouteSettings);
        }
        return Text(cabbird::MessageId::CommonUnknown);
    }

    const char* PluginTabLabel(Tab tab) const noexcept {
        switch (tab) {
        case Tab::Installed: return Text(cabbird::MessageId::PluginsTabInstalled);
        case Tab::Available: return Text(cabbird::MessageId::PluginsTabAvailable);
        case Tab::Updates: return Text(cabbird::MessageId::PluginsTabUpdates);
        case Tab::ThirdParty: return Text(cabbird::MessageId::PluginsTabThirdParty);
        }
        return Text(cabbird::MessageId::CommonUnknown);
    }

    const char* FilterLabel(Filter filter) const noexcept {
        switch (filter) {
        case Filter::All: return Text(cabbird::MessageId::PluginsFilterAll);
        case Filter::Running: return Text(cabbird::MessageId::PluginsFilterRunning);
        case Filter::Disabled: return Text(cabbird::MessageId::PluginsFilterDisabled);
        case Filter::Issues: return Text(cabbird::MessageId::PluginsFilterIssues);
        case Filter::Favorites: return Text(cabbird::MessageId::PluginsFilterFavorites);
        }
        return Text(cabbird::MessageId::CommonUnknown);
    }

    const char* SortLabel(Sort sort) const noexcept {
        switch (sort) {
        case Sort::Name: return Text(cabbird::MessageId::PluginsSortName);
        case Sort::State: return Text(cabbird::MessageId::PluginsSortState);
        case Sort::Author: return Text(cabbird::MessageId::PluginsSortAuthor);
        }
        return Text(cabbird::MessageId::PluginsSortName);
    }

    const char* DiagnosticTabLabel(DiagnosticTab tab) const noexcept {
        switch (tab) {
        case DiagnosticTab::Overview: return Text(cabbird::MessageId::DiagnosticsTabOverview);
        case DiagnosticTab::PluginPerformance:
            return Text(cabbird::MessageId::DiagnosticsTabPluginPerformance);
        case DiagnosticTab::Logs: return Text(cabbird::MessageId::DiagnosticsTabLogs);
        case DiagnosticTab::Developer: return Text(cabbird::MessageId::DiagnosticsTabDeveloper);
        }
        return Text(cabbird::MessageId::DiagnosticsTabOverview);
    }

    const char* ServiceStateName(cabbird::ServiceState state) const noexcept {
        switch (state) {
        case cabbird::ServiceState::Registered:
            return Text(cabbird::MessageId::ServiceStateRegistered);
        case cabbird::ServiceState::Starting:
            return Text(cabbird::MessageId::ServiceStateStarting);
        case cabbird::ServiceState::Ready: return Text(cabbird::MessageId::ServiceStateReady);
        case cabbird::ServiceState::Degraded:
            return Text(cabbird::MessageId::ServiceStateDegraded);
        case cabbird::ServiceState::Failed: return Text(cabbird::MessageId::ServiceStateFailed);
        case cabbird::ServiceState::Stopping:
            return Text(cabbird::MessageId::ServiceStateStopping);
        case cabbird::ServiceState::Stopped: return Text(cabbird::MessageId::ServiceStateStopped);
        }
        return Text(cabbird::MessageId::CommonUnknown);
    }

    const char* UnityLevelName(cabbird::UnityCompatibilityLevel level) const noexcept {
        switch (level) {
        case cabbird::UnityCompatibilityLevel::Unknown:
            return Text(cabbird::MessageId::CommonUnknown);
        case cabbird::UnityCompatibilityLevel::CoreOnly:
            return Text(cabbird::MessageId::UnityLevelCoreOnly);
        case cabbird::UnityCompatibilityLevel::Partial:
            return Text(cabbird::MessageId::UnityLevelPartial);
        case cabbird::UnityCompatibilityLevel::Supported:
            return Text(cabbird::MessageId::UnityLevelSupported);
        }
        return Text(cabbird::MessageId::CommonUnknown);
    }

    static ImVec4 StateColor(cabbird::PlatformUiPluginState state) noexcept {
        const auto& theme = cabbird::CabbirdUiTheme();
        switch (state) {
        case cabbird::PlatformUiPluginState::Active:
            return ThemeColor(theme.success);
        case cabbird::PlatformUiPluginState::Disabled:
            return ThemeColor(theme.text_muted);
        case cabbird::PlatformUiPluginState::Loaded:
            return ThemeColor(theme.info);
        default:
            return ThemeColor(theme.warning);
        }
    }

    static cabbird::PluginOperationReason OperationReasonForState(
        cabbird::PlatformUiPluginState state) noexcept {
        switch (state) {
        case cabbird::PlatformUiPluginState::Disabled:
            return cabbird::PluginOperationReason::Disabled;
        case cabbird::PlatformUiPluginState::Rejected:
            return cabbird::PluginOperationReason::Rejected;
        case cabbird::PlatformUiPluginState::Incompatible:
            return cabbird::PluginOperationReason::Incompatible;
        case cabbird::PlatformUiPluginState::DependencyBlocked:
            return cabbird::PluginOperationReason::DependencyBlocked;
        case cabbird::PlatformUiPluginState::WaitingForService:
            return cabbird::PluginOperationReason::ProviderUnavailable;
        case cabbird::PlatformUiPluginState::Faulted:
        case cabbird::PlatformUiPluginState::Quarantined:
        case cabbird::PlatformUiPluginState::Stopping:
        case cabbird::PlatformUiPluginState::Unknown:
            return cabbird::PluginOperationReason::BackendFailure;
        case cabbird::PlatformUiPluginState::Active:
        case cabbird::PlatformUiPluginState::Loaded:
            return cabbird::PluginOperationReason::None;
        }
        return cabbird::PluginOperationReason::BackendFailure;
    }

    static bool IsHealthyPluginState(cabbird::PlatformUiPluginState state) noexcept {
        return state == cabbird::PlatformUiPluginState::Active ||
            state == cabbird::PlatformUiPluginState::Loaded;
    }

    static bool TryClaimIntent(const QueuedIntent& queued) noexcept {
        if (queued.execution_state == nullptr) return true;
        auto expected = IntentExecutionState::Pending;
        return queued.execution_state->compare_exchange_strong(
            expected, IntentExecutionState::Running,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    static bool TrySettleIntent(
        const QueuedIntent& queued, IntentExecutionState expected_state) noexcept {
        if (queued.execution_state == nullptr) return true;
        auto expected = expected_state;
        return queued.execution_state->compare_exchange_strong(
            expected, IntentExecutionState::Settled,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    [[nodiscard]] std::shared_ptr<InvocationGuard> BeginInvocation() {
        const auto self = shared_from_this();
        auto guard = std::make_shared<InvocationGuard>();
        {
            std::scoped_lock lock(lifetime_mutex_);
            if (closing_) return {};
            ++outstanding_invocations_;
        }
        guard->owner = self;
        return guard;
    }

    void LeaveInvocation() noexcept {
        {
            std::scoped_lock lock(lifetime_mutex_);
            if (outstanding_invocations_ != 0) --outstanding_invocations_;
        }
        lifetime_condition_.notify_all();
    }

    [[nodiscard]] bool EnterCallback(
        std::shared_ptr<QueuedIntent> rejected = {}) noexcept {
        std::scoped_lock lock(lifetime_mutex_);
        if (closing_) {
            if (rejected != nullptr) {
                try {
                    rejected_callbacks_.push_back(std::move(rejected));
                } catch (...) {
                    // The owner is already closing. A rejected intent has no
                    // executable callback left; leave its shared execution
                    // state for the dispatcher completion path.
                }
            }
            return false;
        }
        ++active_callbacks_;
        return true;
    }

    void LeaveCallback() noexcept {
        {
            std::scoped_lock lock(lifetime_mutex_);
            if (active_callbacks_ != 0) --active_callbacks_;
        }
        lifetime_condition_.notify_all();
    }

    void ExecuteIntentCallback(const std::shared_ptr<QueuedIntent>& pending) {
        if (!EnterCallback(pending)) return;
        struct CallbackScope final {
            PlatformUi* owner;
            ~CallbackScope() { owner->LeaveCallback(); }
        } scope{this};
        ExecuteIntent(*pending);
    }

    void FlushMemoryCallback() {
        if (!EnterCallback()) {
            CancelMemoryWork();
            return;
        }
        struct CallbackScope final {
            PlatformUi* owner;
            ~CallbackScope() { owner->LeaveCallback(); }
        } scope{this};
        struct MemoryInvocationScope final {
            PlatformUi* owner;
            ~MemoryInvocationScope() { owner->CompleteMemoryInvocation(); }
        } memory_scope{this};
        std::scoped_lock operation_lock(operation_mutex_);
        FlushDeferredMemory();
    }

    void CompleteMemoryInvocation() noexcept {
        std::scoped_lock operation_lock(operation_mutex_);
        memory_invocation_pending_ = false;
    }

    void CancelOutstandingOperations() {
        // The caller owns operation_mutex_. The recursive mutex also keeps
        // ApplyIntentFailure safe when it publishes cancellation results.
        std::vector<QueuedIntent> queued;
        std::vector<AwaitingBatch> batches;
        std::vector<std::shared_ptr<QueuedIntent>> rejected;
        queued.swap(queued_intents_);
        batches.swap(awaiting_batches_);
        {
            std::scoped_lock lock(lifetime_mutex_);
            rejected.swap(rejected_callbacks_);
        }
        const auto cancel = [this](const QueuedIntent& item, IntentExecutionState state) {
            static_cast<void>(ApplyIntentFailure(
                item, state, cabbird::PlatformUiResultCode::ProviderUnavailable,
                cabbird::PluginOperationReason::ProviderUnavailable,
                "UI owner closed before the lifecycle operation settled", true));
        };
        for (const auto& item : queued) cancel(item, IntentExecutionState::Pending);
        for (const auto& batch : batches) cancel(batch.queued, IntentExecutionState::Running);
        for (const auto& item : rejected) {
            if (item != nullptr) cancel(*item, IntentExecutionState::Pending);
        }
    }

    [[nodiscard]] ShutdownResult QuarantineResult() {
        RevokeManagementWindow();
        std::optional<cabbird::PlatformUiState> state;
        if (operation_mutex_.try_lock()) {
            state = model_.State();
            operation_mutex_.unlock();
        }
        return {std::move(state), false};
    }

    void RevokeManagementWindow() noexcept {
        if (management_window_scope_ == nullptr) return;
        static_cast<void>(management_window_scope_->FreezeCallbackSources());
        static_cast<void>(management_window_scope_->RevokeAll());
    }

    void SyncInputBuffers() {
        std::snprintf(search_.data(), search_.size(), "%s", model_.State().search.c_str());
        std::snprintf(log_filter_.data(), log_filter_.size(), "%s",
            model_.State().diagnostics_log_filter.c_str());
    }

    void RecordPerformance(
        const PlatformUiPerformanceStage stage,
        const std::chrono::steady_clock::time_point started) const noexcept {
        if (started == std::chrono::steady_clock::time_point{} ||
            !diagnostics_.performance_probe) {
            return;
        }
        try {
            diagnostics_.performance_probe(stage, std::chrono::steady_clock::now() - started);
        } catch (...) {
        }
    }

    [[nodiscard]] bool PerformanceDiagnosticsEnabled() const noexcept {
        if (!diagnostics_.performance_probe_enabled) return false;
        try {
            return diagnostics_.performance_probe_enabled();
        } catch (...) {
            return false;
        }
    }

    std::shared_ptr<const CatalogCache> CatalogSnapshot() const {
        std::scoped_lock lock(catalog_mutex_);
        return catalog_cache_;
    }

    void RefreshSnapshot(const bool measure) {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_snapshot_refresh_ && model_.PendingOperations().empty()) return;
        next_snapshot_refresh_ = now + std::chrono::milliseconds(100);
        const auto refresh_started = measure ? std::chrono::steady_clock::now()
                                             : std::chrono::steady_clock::time_point{};
        auto phase_started = refresh_started;
        const auto catalog = CatalogSnapshot();
        contact_ = catalog ? catalog->contact : std::nullopt;
        RecordPerformance(PlatformUiPerformanceStage::RefreshCatalog, phase_started);
        phase_started = measure ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
        const auto runtime_plugins = plugins_.Plugins();
        RecordPerformance(PlatformUiPerformanceStage::RuntimePlugins, phase_started);
        std::map<std::string, cabbird::PluginEnablementDecision, std::less<>> enablement;
        for (const auto& plugin : runtime_plugins) {
            enablement.emplace(plugin.id, cabbird::PluginEnablementDecision{
                plugin.enabled, true, plugin.enabled ? "enabled by runtime" : "disabled by configuration"});
        }

        phase_started = measure ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
        cabbird::PlatformUiSnapshot snapshot;
        if (catalog && catalog->catalog && catalog->dependencies) {
            snapshot = cabbird::BuildPlatformUiSnapshot(
                0, runtime_plugins, *catalog->catalog, *catalog->dependencies,
                enablement, catalog->display_names, catalog->descriptions);
        } else {
            snapshot = cabbird::BuildPlatformUiSnapshot(0, runtime_plugins);
        }
        RecordPerformance(PlatformUiPerformanceStage::BuildSnapshot, phase_started);
        snapshot.diagnostics.runtime_version = diagnostics_.runtime_version;
        snapshot.diagnostics.process_id = GetCurrentProcessId();
        snapshot.diagnostics.healthy = true;
        if (diagnostics_.repository_snapshot) {
            phase_started = measure ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
            snapshot.repository = diagnostics_.repository_snapshot();
            RecordPerformance(PlatformUiPerformanceStage::RepositorySnapshot, phase_started);
        }
        if (diagnostics_.service_graph) {
            phase_started = measure ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
            const auto graph = diagnostics_.service_graph();
            snapshot.diagnostics.healthy = graph.error == ERROR_SUCCESS && graph.failures.empty();
            for (const auto& failure : graph.failures) {
                snapshot.diagnostics.recent_faults.push_back(
                    failure.service_id + ": error " + std::to_string(failure.error));
            }
            RecordPerformance(PlatformUiPerformanceStage::ServiceGraphSnapshot, phase_started);
        }
        phase_started = measure ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
        std::vector<cabbird::AvailableServiceVersion> services;
        for (const auto& service : cabbird::ProcessAdapterServices().Snapshot()) {
            services.push_back({service.id, service.version});
        }
        RecordPerformance(PlatformUiPerformanceStage::AdapterServicesSnapshot, phase_started);
        phase_started = measure ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
        if (diagnostics_.unity_compatibility) {
            snapshot.unity_compatibility = diagnostics_.unity_compatibility();
            snapshot.unity_compatibility.services = std::move(services);
        } else {
            snapshot.unity_compatibility = cabbird::BuildUnityCompatibilitySnapshot(
                {}, std::move(services));
            snapshot.unity_compatibility.reason =
                "UNITY compatibility provider is not published by this host";
        }
        RecordPerformance(PlatformUiPerformanceStage::UnityCompatibilitySnapshot, phase_started);
        phase_started = measure ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
        snapshot.operation_results = model_.Snapshot().operation_results;
        snapshot.operation_plans = model_.Snapshot().operation_plans;
        model_.Publish(std::move(snapshot));
        ResolveAwaitingBatch(model_.Snapshot());
        RecordPerformance(PlatformUiPerformanceStage::ModelPublish, phase_started);
        phase_started = measure ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
        RefreshSettingsState();
        RecordPerformance(PlatformUiPerformanceStage::SettingsRefresh, phase_started);
        RecordPerformance(PlatformUiPerformanceStage::RefreshTotal, refresh_started);
    }

    [[nodiscard]] bool SettingsDirty() const noexcept {
        return settings_draft_.has_value() && settings_snapshot_.ready &&
            *settings_draft_ != settings_snapshot_.values;
    }

    [[nodiscard]] float SettingsScrollClearance() const noexcept {
        const bool draft_bar_visible = SettingsDirty() || settings_save_pending_ ||
            !settings_apply_error_.empty();
        return FloatingBarClearance() + (draft_bar_visible
            ? Scaled(36.0f + (kTabBarInset + 2.0f) * 2.0f + 10.0f) : 0.0f);
    }

    static Route RouteFromKey(const std::string_view route) noexcept {
        if (route == "unity_compatibility") return Route::Diagnostics;
        if (route == "diagnostics") return Route::Diagnostics;
        if (route == "settings") return Route::Settings;
        return Route::Plugins;
    }

    void RefreshSettingsState() {
        if (settings_apply_mailbox_ != nullptr) {
            std::optional<cabbird::PlatformSettingsApplyResult> result;
            {
                std::scoped_lock lock(settings_apply_mailbox_->mutex);
                result.swap(settings_apply_mailbox_->result);
            }
            if (result) {
                settings_save_pending_ = false;
                if (result->Applied()) {
                    settings_snapshot_ = result->snapshot;
                    settings_draft_ = result->snapshot.values;
                    settings_scale_edit_percent_.reset();
                    settings_base_revision_ = result->snapshot.revision;
                    settings_apply_error_.clear();
                    status_ = Text(cabbird::MessageId::SettingsSaved);
                    status_failure_ = false;
                    if (settings_route_after_save_) {
                        const Route route = *settings_route_after_save_;
                        settings_route_after_save_.reset();
                        Navigate(route);
                    }
                } else {
                    settings_apply_error_ = result->message;
                    settings_validation_errors_ = std::move(result->validation_errors);
                    status_ = Text(cabbird::MessageId::SettingsSaveFailed);
                    status_failure_ = true;
                }
            }
        }

        if (repository_configure_mailbox_ != nullptr) {
            std::optional<RepositoryConfigureResult> result;
            {
                std::scoped_lock lock(repository_configure_mailbox_->mutex);
                result.swap(repository_configure_mailbox_->result);
            }
            if (result) {
                repo_editor_save_pending_ = false;
                if (result->submission.accepted) {
                    LoadRepositoryEditor(result->config);
                    repo_editor_status_ = Text(cabbird::MessageId::SettingsRepositoriesApplied);
                    repo_editor_status_failure_ = false;
                    status_ = repo_editor_status_;
                    status_failure_ = false;
                } else {
                    const std::array<std::string_view, 1> arguments{
                        result->submission.message};
                    repo_editor_status_ = Format(
                        cabbird::MessageId::SettingsRepositoriesApplyFailed, arguments);
                    repo_editor_status_failure_ = true;
                }
            }
        }

        if (!diagnostics_.settings_snapshot) return;
        const cabbird::PlatformSettingsSnapshot published = diagnostics_.settings_snapshot();
        const bool was_ready = settings_snapshot_.ready;
        const bool dirty = SettingsDirty();
        settings_snapshot_ = published;
        if (published.ready && (!settings_draft_ ||
                (!dirty && !settings_save_pending_ && !settings_scale_edit_percent_))) {
            settings_draft_ = published.values;
            settings_scale_edit_percent_.reset();
            settings_base_revision_ = published.revision;
            settings_apply_error_.clear();
        }
        if (!settings_route_restored_ && published.ready) {
            settings_route_restored_ = true;
            if (!was_ready && published.values.interface_remember_last_route &&
                model_.State().route == Route::Plugins) {
                model_.State().route = RouteFromKey(published.last_route);
            }
        }
    }

    void ApplySettingsPreview() {
        if (!settings_draft_) return;
        const auto& values = *settings_draft_;
        cabbird::glass::SetReduceMotion(values.interface_reduced_motion);
        cabbird::glass::SetReduceTransparency(values.interface_reduced_transparency);
        const bool custom_colors_changed =
            cabbird::GetCabbirdUiCustomColors() != values.interface_custom_colors;
        if (custom_colors_changed) {
            cabbird::SetCabbirdUiCustomColors(values.interface_custom_colors);
        }
        const bool palette_changed =
            cabbird::GetCabbirdUiPalette() != values.interface_palette;
        if (palette_changed) {
            cabbird::SetCabbirdUiPalette(values.interface_palette);
            {
                std::scoped_lock lock(theme_mutex_);
                theme_requested_palette_ = values.interface_palette;
                theme_reload_ = true;
            }
            theme_changed_.notify_all();
        }
        if (palette_changed ||
            (values.interface_palette == cabbird::CabbirdUiPalette::Custom &&
                custom_colors_changed)) {
            ApplyCabbirdUiStyle();
        }
        SetDeveloperMode(values.advanced_developer_mode);
        cabbird::SetHostUiInputCapturePolicy(values.input_capture_policy);
        ImGuiIO& io = ImGui::GetIO();
        const float requested_scale = static_cast<float>(values.interface_scale_percent) / 100.0f;
        if (std::abs(io.FontGlobalScale - requested_scale) > 0.0001f) {
            static_cast<void>(ApplyCabbirdUiFontScale(requested_scale));
        }
        if (values.input_gamepad_navigation) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    }

    static ImVec4 ThemeColor(const cabbird::CabbirdUiColor& color) noexcept {
        return {color.red, color.green, color.blue, color.alpha};
    }

    static ImVec4 ThemeColorWithAlpha(
        const cabbird::CabbirdUiColor& color, const float alpha) noexcept {
        return {color.red, color.green, color.blue, alpha};
    }

    static ImVec4 AccentColor() noexcept { return ThemeColor(cabbird::CabbirdUiTheme().accent); }
    static ImVec4 SuccessColor() noexcept { return ThemeColor(cabbird::CabbirdUiTheme().success); }
    static ImVec4 WarningColor() noexcept { return ThemeColor(cabbird::CabbirdUiTheme().warning); }
    static ImVec4 ErrorColor() noexcept { return ThemeColor(cabbird::CabbirdUiTheme().danger); }
    static ImVec4 InfoColor() noexcept { return ThemeColor(cabbird::CabbirdUiTheme().info); }
    static ImVec4 SurfaceColor() noexcept {
        return ThemeColor(cabbird::CabbirdUiTheme().child_background);
    }
    static ImVec4 RaisedColor() noexcept {
        return ThemeColor(cabbird::CabbirdUiTheme().popup_background);
    }
    static ImVec4 NavColor() noexcept {
        return ThemeColor(cabbird::CabbirdUiTheme().navigation_background);
    }

    [[nodiscard]] const char* DisplayPluginState(
        const cabbird::PlatformUiPluginState state) const noexcept {
        switch (state) {
        case cabbird::PlatformUiPluginState::Active:
            return Text(cabbird::MessageId::PluginStateRunning);
        case cabbird::PlatformUiPluginState::Loaded:
            return Text(cabbird::MessageId::PluginStateLoaded);
        case cabbird::PlatformUiPluginState::Disabled:
            return Text(cabbird::MessageId::PluginStateDisabled);
        case cabbird::PlatformUiPluginState::Faulted:
            return Text(cabbird::MessageId::PluginStateFaulted);
        case cabbird::PlatformUiPluginState::Quarantined:
            return Text(cabbird::MessageId::PluginStateQuarantined);
        case cabbird::PlatformUiPluginState::WaitingForService:
            return Text(cabbird::MessageId::PluginStateWaitingForService);
        case cabbird::PlatformUiPluginState::Rejected:
            return Text(cabbird::MessageId::PluginStatePackageRejected);
        case cabbird::PlatformUiPluginState::DependencyBlocked:
            return Text(cabbird::MessageId::PluginStateDependencyBlocked);
        case cabbird::PlatformUiPluginState::Incompatible:
            return Text(cabbird::MessageId::PluginStateIncompatible);
        case cabbird::PlatformUiPluginState::Stopping:
            return Text(cabbird::MessageId::PluginStateStopping);
        case cabbird::PlatformUiPluginState::Unknown:
            return Text(cabbird::MessageId::CommonUnknown);
        }
        return Text(cabbird::MessageId::CommonUnknown);
    }

    [[nodiscard]] const char* DisplayRepositoryState(
        const cabbird::RepositoryCoordinatorState state) const noexcept {
        switch (state) {
        case cabbird::RepositoryCoordinatorState::Disabled:
            return Text(cabbird::MessageId::RepositoryStateDisabled);
        case cabbird::RepositoryCoordinatorState::Refreshing:
            return Text(cabbird::MessageId::RepositoryStateRefreshing);
        case cabbird::RepositoryCoordinatorState::Ready:
            return Text(cabbird::MessageId::RepositoryStateReady);
        case cabbird::RepositoryCoordinatorState::Degraded:
            return Text(cabbird::MessageId::RepositoryStateDegraded);
        case cabbird::RepositoryCoordinatorState::Unavailable:
            return Text(cabbird::MessageId::RepositoryStateUnavailable);
        case cabbird::RepositoryCoordinatorState::Stopped:
            return Text(cabbird::MessageId::RepositoryStateStopped);
        }
        return Text(cabbird::MessageId::RepositoryStateUnavailable);
    }

    [[nodiscard]] const char* DisplayRepositoryOperationState(
        const cabbird::RepositoryOperationState state) const noexcept {
        switch (state) {
        case cabbird::RepositoryOperationState::Queued:
            return Text(cabbird::MessageId::RepositoryOperationQueued);
        case cabbird::RepositoryOperationState::Downloading:
            return Text(cabbird::MessageId::RepositoryOperationDownloading);
        case cabbird::RepositoryOperationState::Installing:
            return Text(cabbird::MessageId::RepositoryOperationInstalling);
        case cabbird::RepositoryOperationState::Uninstalling:
            return Text(cabbird::MessageId::RepositoryOperationUninstalling);
        case cabbird::RepositoryOperationState::Succeeded:
            return Text(cabbird::MessageId::RepositoryOperationSucceeded);
        case cabbird::RepositoryOperationState::Failed:
            return Text(cabbird::MessageId::RepositoryOperationFailed);
        case cabbird::RepositoryOperationState::Cancelled:
            return Text(cabbird::MessageId::RepositoryOperationCancelled);
        }
        return Text(cabbird::MessageId::RepositoryOperationFailed);
    }

    [[nodiscard]] static ImVec4 PluginStateColor(
        const cabbird::PlatformUiPluginState state) noexcept {
        switch (state) {
        case cabbird::PlatformUiPluginState::Active: return SuccessColor();
        case cabbird::PlatformUiPluginState::Loaded: return InfoColor();
        case cabbird::PlatformUiPluginState::Faulted:
        case cabbird::PlatformUiPluginState::Quarantined:
        case cabbird::PlatformUiPluginState::Rejected:
            return ErrorColor();
        case cabbird::PlatformUiPluginState::Disabled:
            return ThemeColor(cabbird::CabbirdUiTheme().text_muted);
        default: return WarningColor();
        }
    }

    [[nodiscard]] static ShellGlyph RouteGlyph(const Route route) noexcept {
        switch (route) {
        case Route::Plugins: return ShellGlyph::Package;
        case Route::UnityCompatibility: return ShellGlyph::Shield;
        case Route::Diagnostics: return ShellGlyph::Terminal;
        case Route::Settings: return ShellGlyph::Settings;
        }
        return ShellGlyph::Info;
    }

    [[nodiscard]] static ShellGlyph StateGlyph(
        const cabbird::PlatformUiPluginState state) noexcept {
        switch (state) {
        case cabbird::PlatformUiPluginState::Active: return ShellGlyph::Check;
        case cabbird::PlatformUiPluginState::Loaded: return ShellGlyph::Info;
        case cabbird::PlatformUiPluginState::Disabled: return ShellGlyph::Stop;
        default: return ShellGlyph::Warning;
        }
    }

    [[nodiscard]] const char* MutationLabel(const Mutation mutation) const noexcept {
        switch (mutation) {
        case Mutation::Start: return Text(cabbird::MessageId::OperationStartingPlugin);
        case Mutation::Stop: return Text(cabbird::MessageId::OperationStoppingPlugin);
        case Mutation::Reload: return Text(cabbird::MessageId::OperationReloadingPlugin);
        case Mutation::Enable: return Text(cabbird::MessageId::OperationEnablingPlugin);
        case Mutation::Disable: return Text(cabbird::MessageId::OperationDisablingPlugin);
        case Mutation::SetVisible:
            return Text(cabbird::MessageId::OperationUpdatingPluginWindow);
        case Mutation::ReloadAll:
            return Text(cabbird::MessageId::OperationReloadingInstalledPlugins);
        case Mutation::None: return Text(cabbird::MessageId::OperationPlugin);
        }
        return Text(cabbird::MessageId::OperationPlugin);
    }

    [[nodiscard]] const char* MutationActionLabel(const Mutation mutation) const noexcept {
        switch (mutation) {
        case Mutation::Start: return Text(cabbird::MessageId::CommonStart);
        case Mutation::Stop: return Text(cabbird::MessageId::CommonStop);
        case Mutation::Reload:
        case Mutation::ReloadAll: return Text(cabbird::MessageId::CommonReload);
        case Mutation::Enable: return Text(cabbird::MessageId::CommonEnable);
        case Mutation::Disable: return Text(cabbird::MessageId::CommonDisable);
        case Mutation::SetVisible: return Text(cabbird::MessageId::CommonUpdate);
        case Mutation::None: return Text(cabbird::MessageId::OperationPlugin);
        }
        return Text(cabbird::MessageId::OperationPlugin);
    }

    [[nodiscard]] const char* OperationStateLabel(
        const cabbird::PlatformUiOperationState state) const noexcept {
        switch (state) {
        case cabbird::PlatformUiOperationState::Succeeded:
            return Text(cabbird::MessageId::CommonSucceeded);
        case cabbird::PlatformUiOperationState::PartiallyFailed:
        case cabbird::PlatformUiOperationState::Failed:
            return Text(cabbird::MessageId::CommonFailed);
        case cabbird::PlatformUiOperationState::Cancelled:
            return Text(cabbird::MessageId::OperationCancelled);
        case cabbird::PlatformUiOperationState::Idle:
        case cabbird::PlatformUiOperationState::Submitted:
        case cabbird::PlatformUiOperationState::Running:
            return Text(cabbird::MessageId::OperationInProgress);
        }
        return Text(cabbird::MessageId::OperationInProgress);
    }

    [[nodiscard]] std::string OperationDisplayLabel(
        const cabbird::PlatformUiOperationResult& operation) const {
        if (operation.state == cabbird::PlatformUiOperationState::Idle ||
            operation.state == cabbird::PlatformUiOperationState::Submitted ||
            operation.state == cabbird::PlatformUiOperationState::Running) {
            return MutationLabel(operation.mutation);
        }
        return std::string(MutationActionLabel(operation.mutation)) + " - " +
            OperationStateLabel(operation.state);
    }

    [[nodiscard]] static ImVec4 OperationStateColor(
        const cabbird::PlatformUiOperationState state) noexcept {
        switch (state) {
        case cabbird::PlatformUiOperationState::Succeeded: return SuccessColor();
        case cabbird::PlatformUiOperationState::Failed: return ErrorColor();
        case cabbird::PlatformUiOperationState::PartiallyFailed: return WarningColor();
        case cabbird::PlatformUiOperationState::Cancelled:
            return ThemeColor(cabbird::CabbirdUiTheme().text_muted);
        case cabbird::PlatformUiOperationState::Idle:
        case cabbird::PlatformUiOperationState::Submitted:
        case cabbird::PlatformUiOperationState::Running:
            return InfoColor();
        }
        return InfoColor();
    }

    [[nodiscard]] LayoutMode ComputeLayoutMode() const noexcept {
        const ImVec2 size = ImGui::GetWindowSize();
        if (size.x >= Scaled(980.0f) && size.y >= Scaled(600.0f)) {
            return LayoutMode::Wide;
        }
        if (size.x >= Scaled(720.0f) && size.y >= Scaled(480.0f)) {
            return LayoutMode::Standard;
        }
        return LayoutMode::Compact;
    }

    void UpdateLayout() noexcept {
        const LayoutMode next = ComputeLayoutMode();
        if (next != LayoutMode::Compact) {
            compact_plugin_detail_ = false;
            compact_plugin_detail_pending_id_.clear();
        }
        layout_mode_ = next;
    }

    [[nodiscard]] bool IsCompact() const noexcept { return layout_mode_ == LayoutMode::Compact; }

    [[nodiscard]] bool ManagementShellGloballyCollapsed() const noexcept {
        return cabbird::HostUiMenusCollapsed() && !management_shell_locked_;
    }

    [[nodiscard]] bool ManagementShellCollapsed() const noexcept {
        return management_shell_collapsed_ || ManagementShellGloballyCollapsed();
    }

    [[nodiscard]] float PluginListWidth(const float available) const noexcept {
        const float default_width = Scaled(layout_mode_ == LayoutMode::Wide ? 360.0f : 320.0f);
        const float requested = plugin_list_width_ > 0.0f
            ? Scaled(plugin_list_width_) : default_width;
        const float minimum = Scaled(layout_mode_ == LayoutMode::Wide ? 300.0f : 280.0f);
        const float maximum = Scaled(layout_mode_ == LayoutMode::Wide ? 440.0f : 380.0f);
        return std::clamp(requested, minimum,
            std::min(maximum, std::max(minimum, available - Scaled(360.0f))));
    }

    void DrawTooltip(const char* text) const {
        if (text == nullptr || *text == '\0' ||
            !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            return;
        }
        ImGui::SetTooltip("%s", text);
    }

    static void DrawGlass(ImDrawList* const list, const ImVec2 min, const ImVec2 max,
        const cabbird::glass::Variant variant, const float rounding,
        const bool shadow = true, const bool clip = true) noexcept {
        cabbird::glass::SurfaceOptions options;
        options.variant = variant;
        options.rounding = Scaled(rounding);
        options.scale = PlatformUiScale();
        options.shadow = shadow;
        options.clip = clip;
        cabbird::glass::DrawSurface(list, min, max, options);
    }

    static ImVec4 InkColor(const float alpha) noexcept {
        return ThemeColorWithAlpha(cabbird::CabbirdUiTheme().text, alpha);
    }

    // The accent darkened toward the text colour until it reads as ink on the
    // light glass; the raw accent of light palettes is too pale for a label.
    static ImVec4 AccentInk() noexcept {
        const auto& theme = cabbird::CabbirdUiTheme();
        const float mix = cabbird::glass::DarkAppearance() ? 0.0f : 0.45f;
        return ImVec4(theme.accent.red + (theme.text.red - theme.accent.red) * mix,
            theme.accent.green + (theme.text.green - theme.accent.green) * mix,
            theme.accent.blue + (theme.text.blue - theme.accent.blue) * mix, 1.0f);
    }

    static constexpr ImVec4 kClear{0.0f, 0.0f, 0.0f, 0.0f};

    static ImFont* FontForScale(const float scale) noexcept {
        ImFont* font = CabbirdUiFontForScale(scale);
        return font != nullptr ? font : ImGui::GetFont();
    }

    static constexpr float kTabBarItemHeight = 54.0f;
    static constexpr float kTabBarInset = 5.0f;
    static constexpr float kTabBarBottomMargin = 22.0f;
    static constexpr float kTabBarItemPadding = 20.0f;
    static constexpr float kTabBarRounding = 0.36f;

    static float CapsuleRounding(const ImVec2 size) noexcept {
        return (size.y > 0.0f ? size.y : ImGui::GetFrameHeight()) * kTabBarRounding;
    }

    // Vertical padding that makes a text field or combo 36 px tall, the height
    // of the segmented controls it sits beside.
    static float FieldPaddingY() noexcept {
        return std::max(0.0f, std::round((Scaled(36.0f) - ImGui::GetFontSize()) * 0.5f));
    }

    // Where the leading glyph of the text field just submitted is drawn.
    static ImVec2 FieldGlyphPosition() noexcept {
        return Offset(ImGui::GetItemRectMin(), Scaled(11.0f),
            (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f + Scaled(1.0f));
    }

    // How much of the route's bottom edge the floating tab bar covers, with a
    // small gap: content that must stay reachable ends above this.
    [[nodiscard]] static float FloatingBarClearance() noexcept {
        return Scaled(kTabBarItemHeight + kTabBarInset * 2.0f + 8.0f);
    }

    [[nodiscard]] static ImVec2 ShellScrollTableSize() noexcept {
        // A nested scrolling table owns its scroll range. Padding appended to
        // its parent cannot move its final row out from under the floating bar.
        const ImGuiWindow* const root = ImGui::GetCurrentWindow()->RootWindow;
        const float visible_bottom = root->Pos.y + root->Size.y -
            Scaled(kTabBarBottomMargin + kTabBarItemHeight + kTabBarInset * 2.0f + 8.0f);
        return {0.0f, std::max(1.0f, std::min(ImGui::GetContentRegionAvail().y,
            visible_bottom - ImGui::GetCursorScreenPos().y))};
    }

    // A selectable whose highlight is a rounded, animated capsule drawn under
    // its label instead of ImGui's square header fill.
    // A selectable with no fill at all: hover and selection only change the
    // ink of its label. Callers lay a glass lens over the selected one.
    bool CapsuleSelectable(const char* label, const bool selected, const ImVec2 size) const {
        const auto& theme = cabbird::CabbirdUiTheme();
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const ImVec2 resolved = ImGui::CalcItemSize(size,
            ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight());
        const bool pointer_over = ImGui::IsWindowHovered() &&
            ImGui::IsMouseHoveringRect(cursor, Offset(cursor, resolved.x, resolved.y));
        ImGui::PushStyleColor(ImGuiCol_Header, kClear);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kClear);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, kClear);
        ImGui::PushStyleColor(ImGuiCol_Text, selected ? AccentInk()
            : pointer_over ? ThemeColor(theme.text) : InkColor(0.62f));
        const bool clicked = ImGui::Selectable(label, selected, 0, size);
        ImGui::PopStyleColor(4);
        return clicked;
    }

    // A vertical list of choices on a content card. Labels change ink on hover
    // and selection, and a glass lens slides under the selected row.
    template <typename Label>
    bool DrawLensRail(const char* id, const std::size_t count, std::size_t& selected,
        Label&& label, const float row_height) const {
        ImDrawList* const list = ImGui::GetWindowDrawList();
        list->ChannelsSplit(2);
        list->ChannelsSetCurrent(1);
        ImRect selected_rect;
        bool changed = false;
        ImGui::PushID(id);
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.08f, 0.5f));
        for (std::size_t index = 0; index < count; ++index) {
            ImGui::PushID(static_cast<int>(index));
            if (CapsuleSelectable(label(index).c_str(), index == selected,
                    FillAvailableSize(row_height))) {
                changed = changed || selected != index;
                selected = index;
            }
            if (index == selected) {
                selected_rect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            }
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        list->ChannelsSetCurrent(0);
        if (selected_rect.GetWidth() > 0.0f) {
            const ImRect thumb = cabbird::glass::AnimateThumb(
                ImGui::GetID("##rail-thumb"), selected_rect, ImGui::GetWindowPos());
            cabbird::glass::DrawThumb(list, thumb.Min, thumb.Max,
                thumb.GetHeight() * kTabBarRounding, PlatformUiScale(), true);
        }
        list->ChannelsMerge();
        ImGui::PopID();
        return changed;
    }

    [[nodiscard]] bool PrimaryButton(const char* label, ImVec2 size = {}) const {
        const auto& theme = cabbird::CabbirdUiTheme();
        ImGui::PushStyleColor(ImGuiCol_Button, AccentColor());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentColor());
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeColor(theme.accent_active));
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColor(theme.inverse_text));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, CapsuleRounding(size));
        const bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        return pressed;
    }

    [[nodiscard]] bool DestructiveButton(const char* label, ImVec2 size = {}) const {
        const auto& theme = cabbird::CabbirdUiTheme();
        ImGui::PushStyleColor(ImGuiCol_Button, ThemeColorWithAlpha(theme.danger, 0.86f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeColorWithAlpha(theme.danger, 0.86f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeColorWithAlpha(theme.danger, 0.70f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, CapsuleRounding(size));
        const bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        return pressed;
    }

    [[nodiscard]] bool IconButton(const char* label, const char* tooltip) const {
        const bool pressed = ImGui::Button(label, Scaled(30.0f, 30.0f));
        DrawTooltip(tooltip);
        return pressed;
    }

    static void DrawGlyphCentred(const ShellGlyph glyph, const ImVec2 min, const ImVec2 max,
        const ImU32 color, const float size = 0.0f) {
        ImFont* const font = ImGui::GetFont();
        if (font == nullptr) return;
        const float glyph_size = size > 0.0f ? size : ImGui::GetFontSize();
        const char* const text = ShellGlyphText(glyph);
        const ImVec2 center = cabbird::glass::GlyphInkCenter(font, glyph_size, text);
        ImGui::GetWindowDrawList()->AddText(font, glyph_size,
            ImVec2(min.x + (max.x - min.x) * 0.5f - center.x,
                min.y + (max.y - min.y) * 0.5f - center.y), color, text);
    }

    [[nodiscard]] bool DrawShellIconButton(const char* id, const ShellGlyph glyph,
        const char* tooltip, const bool enabled = true, const bool selected = false) const {
        const auto& theme = cabbird::CabbirdUiTheme();
        ImGui::PushID(id);
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const bool pointer_over = enabled && ImGui::IsWindowHovered() &&
            ImGui::IsMouseHoveringRect(cursor, Offset(cursor, Scaled(30.0f), Scaled(30.0f)));
        const ImVec4 fill = selected ? ThemeColorWithAlpha(theme.accent, 0.24f) : kClear;
        ImVec4 text_color = selected || pointer_over
            ? ThemeColor(theme.text) : ThemeColor(theme.text_muted);
        if (!enabled) text_color.w *= ImGui::GetStyle().DisabledAlpha;
        ImGui::PushStyleColor(ImGuiCol_Button, fill);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fill);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, fill);
        ImGui::PushStyleColor(ImGuiCol_Text, kClear);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Scaled(10.0f));
        ImGui::BeginDisabled(!enabled);
        const bool pressed = ImGui::Button("##icon", Scaled(30.0f, 30.0f));
        ImGui::EndDisabled();
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        const ImVec2 item_min = ImGui::GetItemRectMin();
        const ImVec2 item_max = ImGui::GetItemRectMax();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        DrawGlyphCentred(glyph, item_min, item_max,
            ImGui::ColorConvertFloat4ToU32(text_color));
        ImGui::PopID();
        if (hovered && tooltip != nullptr) ImGui::SetTooltip("%s", tooltip);
        return pressed && enabled;
    }

    [[nodiscard]] bool DrawShellCommandButton(const char* id, const char* label,
        const ShellGlyph glyph, const bool primary = false, const bool enabled = true,
        const ImVec2 size = {}) const {
        const auto& theme = cabbird::CabbirdUiTheme();
        const std::string text = std::string(ShellGlyphText(glyph)) + "  " + label;
        ImGui::PushID(id);
        ImGui::PushStyleColor(ImGuiCol_Button, primary ? AccentColor() : InkColor(0.085f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, primary ? AccentColor() : InkColor(0.085f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            primary ? ThemeColor(theme.accent_active) : InkColor(0.14f));
        ImGui::PushStyleColor(ImGuiCol_Text,
            primary ? ThemeColor(theme.inverse_text) : ThemeColor(theme.text));
        const ImVec2 button_size = size.x > 0.0f ? size : ImVec2(0.0f, Scaled(30.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, CapsuleRounding(button_size));
        ImGui::BeginDisabled(!enabled);
        const bool pressed = ImGui::Button(text.c_str(), button_size);
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        ImGui::PopID();
        return pressed && enabled;
    }

    static void DrawCardSurface(const ImVec2 origin, const ImVec2 size) {
        cabbird::glass::SurfaceOptions options;
        options.variant = cabbird::glass::Variant::Material;
        options.rounding = Scaled(14.0f);
        options.scale = PlatformUiScale();
        options.shadow = false;
        cabbird::glass::DrawSurface(ImGui::GetWindowDrawList(), origin,
            Offset(origin, size.x, size.y), options);
    }

    void BeginCardChild(const char* id, const ImVec2 size, const float padding,
        const ImGuiWindowFlags flags = 0) const {
        const ImVec2 resolved = ImGui::CalcItemSize(size,
            ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
        DrawCardSurface(ImGui::GetCursorScreenPos(), resolved);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scaled(padding, padding));
        ImGui::BeginChild(id, resolved, ImGuiChildFlags_AlwaysUseWindowPadding,
            flags | ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();
    }

    void EndCardChild() const { ImGui::EndChild(); }

    void BeginShellBodyChild(const char* id, const ImVec2 size = {},
        const bool bordered = false, const ImGuiWindowFlags window_flags = 0) const {
        const float padding = Scaled(IsCompact() ? 12.0f : 16.0f);
        if (bordered) {
            const ImVec2 resolved = ImGui::CalcItemSize(size,
                ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
            DrawCardSurface(ImGui::GetCursorScreenPos(), resolved);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
        ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding,
            window_flags | (bordered ? ImGuiWindowFlags_NoBackground : 0));
    }

    void EndShellBodyChild() const {
        ImGui::Dummy(ImVec2(0.0f, FloatingBarClearance()));
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    enum class ShellHeaderControl { Collapse, Lock, Close };

    [[nodiscard]] bool DrawShellHeaderControl(const char* id, const ShellHeaderControl control,
        const bool active, const bool disabled, const char* tooltip) const {
        const ShellGlyph glyph = control == ShellHeaderControl::Collapse
            ? (active ? ShellGlyph::ChevronUp : ShellGlyph::ChevronDown)
            : control == ShellHeaderControl::Lock ? ShellGlyph::Pin : ShellGlyph::Close;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        const bool pressed = DrawShellIconButton(id, glyph, tooltip, !disabled, active);
        ImGui::PopStyleVar();
        return pressed;
    }

    [[nodiscard]] static std::string Ellipsize(std::string_view text, const float width) {
        if (text.empty() || ImGui::CalcTextSize(text.data(), text.data() + text.size()).x <= width) {
            return std::string(text);
        }
        constexpr std::string_view suffix{"..."};
        if (ImGui::CalcTextSize(suffix.data(), suffix.data() + suffix.size()).x > width) return {};
        std::size_t end = text.size();
        while (end > 0) {
            --end;
            while (end > 0 &&
                   (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U) {
                --end;
            }
            std::string candidate{text.substr(0, end)};
            candidate += suffix;
            if (ImGui::CalcTextSize(candidate.c_str()).x <= width) return candidate;
        }
        return std::string(suffix);
    }

    // Draws the glossy top sheen of a plugin tile. AddRectFilledMultiColor cannot be used
    // here: that fill is an axis-aligned rectangle, so it squares off the rounded top
    // corners. The silhouette is emitted once as a convex fan with a per-vertex alpha
    // ramp instead: the corners follow the tile's own path, and the ramp is interpolated
    // by the rasterizer, so the fade is exactly the linear gradient the rectangle faked.
    static void DrawIconTopSheen(ImDrawList* const draw_list, const ImVec2 minimum,
        const ImVec2 maximum, const float rounding) {
        const float height = maximum.y - minimum.y;
        if (height <= 0.0f) return;
        draw_list->PathRect(minimum, maximum, rounding, ImDrawFlags_RoundCornersTop);
        const int point_count = draw_list->_Path.Size;
        if (point_count < 3) {
            draw_list->PathClear();
            return;
        }
        const ImVec2 white_uv = draw_list->_Data->TexUvWhitePixel;
        draw_list->PrimReserve((point_count - 2) * 3, point_count);
        const auto index_base = static_cast<ImDrawIdx>(draw_list->_VtxCurrentIdx);
        for (int i = 0; i < point_count; ++i) {
            const ImVec2 point = draw_list->_Path[i];
            const float fade = ImClamp(1.0f - (point.y - minimum.y) / height, 0.0f, 1.0f);
            draw_list->PrimWriteVtx(point, white_uv,
                IM_COL32(255, 255, 255, static_cast<int>(40.0f * fade + 0.5f)));
        }
        for (int i = 2; i < point_count; ++i) {
            draw_list->PrimWriteIdx(index_base);
            draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(index_base + i - 1));
            draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(index_base + i));
        }
        draw_list->PathClear();
    }

    static void DrawGenericPluginIcon(const ImVec2 position, const float size) {
        const auto& theme = cabbird::CabbirdUiTheme();
        ImDrawList* const draw_list = ImGui::GetWindowDrawList();
        const ImU32 border = ImGui::ColorConvertFloat4ToU32(
            ThemeColorWithAlpha(theme.icon_border, 0.55f));
        const ImU32 fill = ImGui::ColorConvertFloat4ToU32(ThemeColor(theme.icon_fill));
        // A continuous-corner tile in the proportion Apple uses for app icons.
        const float rounding = size * 0.26f;
        draw_list->AddRectFilled(position, Offset(position, size, size), fill, rounding);
        DrawIconTopSheen(draw_list, position, Offset(position, size, size * 0.5f), rounding);
        draw_list->AddRect(position, Offset(position, size, size), border, rounding);
        const ImVec2 glyph_size = ImGui::CalcTextSize(ShellGlyphText(ShellGlyph::Package));
        draw_list->AddText(Offset(position, (size - glyph_size.x) * 0.5f,
            (size - glyph_size.y) * 0.5f - Scaled(1.0f)),
            ImGui::ColorConvertFloat4ToU32(ThemeColor(theme.text_muted)),
            ShellGlyphText(ShellGlyph::Package));
    }

    [[nodiscard]] float PluginStateBadgeWidth(
        const cabbird::PlatformUiPluginState state) const {
        const char* const glyph = ShellGlyphText(StateGlyph(state));
        const char* const label = DisplayPluginState(state);
        return ImGui::CalcTextSize(glyph).x + Scaled(5.0f) +
            ImGui::CalcTextSize(label).x + Scaled(14.0f);
    }

    [[nodiscard]] static float PluginStateBadgeHeight() noexcept { return Scaled(20.0f); }

    void DrawPluginStateBadge(const ImVec2 position,
        const cabbird::PlatformUiPluginState state) const {
        const char* const glyph = ShellGlyphText(StateGlyph(state));
        const char* const label = DisplayPluginState(state);
        const float glyph_width = ImGui::CalcTextSize(glyph).x;
        const float width = PluginStateBadgeWidth(state);
        const float height = PluginStateBadgeHeight();
        const ImVec4 color = PluginStateColor(state);
        ImDrawList* const draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(position, Offset(position, width, height),
            ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, 0.17f)),
            height * 0.5f);
        draw_list->AddText(ScaledOffset(position, 7.0f, 4.0f),
            ImGui::ColorConvertFloat4ToU32(color), glyph);
        draw_list->AddText(Offset(
                position, Scaled(7.0f) + glyph_width + Scaled(5.0f), Scaled(3.0f)),
            ImGui::ColorConvertFloat4ToU32(color), label);
    }

    [[nodiscard]] const cabbird::PlatformUiOperationResult* PresentedOperation() const noexcept {
        for (auto it = model_.PendingOperations().rbegin(); it != model_.PendingOperations().rend(); ++it) {
            return &*it;
        }
        const auto& results = model_.Snapshot().operation_results;
        for (auto it = results.rbegin(); it != results.rend(); ++it) {
            if (it->state == cabbird::PlatformUiOperationState::PartiallyFailed ||
                it->state == cabbird::PlatformUiOperationState::Failed) {
                return &*it;
            }
        }
        return nullptr;
    }

    void HandleShellShortcuts() {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F) &&
            model_.State().route == Route::Plugins &&
            model_.State().plugin_tab == Tab::Installed) {
            focus_plugin_search_ = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && IsCompact() &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
            if (compact_plugin_detail_) compact_plugin_detail_ = false;
            else compact_plugin_detail_pending_id_.clear();
        }
    }

    void DrawManagementShell() {
        page_header_actions_ = nullptr;
        // The route is drawn first so the header can carry the route's actions
        // in the same row as the window controls.
        const ImVec2 top = ImGui::GetCursorPos();
        if (!ManagementShellCollapsed()) {
            ImGui::SetCursorPos(ImVec2(top.x, top.y + Scaled(kPlatformHeaderHeight)));
            DrawShellWorkspace();
        }
        ImGui::SetCursorPos(top);
        DrawShellHeader();
    }

    void DrawShellWorkspace() {
        const float workspace_height = ImGui::GetContentRegionAvail().y;
        ImGui::BeginChild("PlatformShellWorkspace", ImVec2(0.0f, workspace_height), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                ImGuiWindowFlags_NoBackground);
        ImGui::SetCursorPos(ImVec2(Scaled(14.0f), 0.0f));
        ImGui::BeginChild("PlatformShellRoute",
            ImVec2(-Scaled(14.0f), -Scaled(12.0f)), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                ImGuiWindowFlags_NoBackground);
        DrawShellRoute();
        ImGui::EndChild();
        DrawShellTabBar();
        DrawSettingsDraftBar();
        ImGui::EndChild();
    }

    // The route switcher: a floating Liquid Glass tab bar centred at the
    // bottom of the shell, with each route's icon above its label and a glass
    // lens sliding under the selected one.
    void DrawShellTabBar() {
        const auto& theme = cabbird::CabbirdUiTheme();
        const auto& snapshot = model_.Snapshot();
        constexpr std::array<Route, 3> routes{
            Route::Plugins, Route::Diagnostics, Route::Settings};
        const float scale = PlatformUiScale();
        const float icon_scale = IsCompact() ? 1.40f : 1.55f;
        ImFont* const icon_font = FontForScale(scale * icon_scale);
        const float icon_size = Scaled(cabbird::CabbirdUiBaseFontSize() * icon_scale);
        ImFont* const label_font = ImGui::GetFont();
        const float label_size = ImGui::GetFontSize();
        float content_width = 0.0f;
        for (const Route route : routes) {
            content_width = std::max(content_width,
                label_font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, RouteLabel(route)).x);
            content_width = std::max(content_width, icon_size);
        }
        const float desired_item_width = content_width + Scaled(kTabBarItemPadding) * 2.0f;
        const float maximum_item_width = (std::max)(1.0f,
            (ImGui::GetWindowSize().x - Scaled(28.0f + kTabBarInset * 2.0f)) /
                static_cast<float>(routes.size()));
        const float item_width = (std::min)(desired_item_width, maximum_item_width);
        const float item_height = Scaled(kTabBarItemHeight);
        const float inset = Scaled(kTabBarInset);
        const float item_rounding = item_height * kTabBarRounding;
        const float bar_width = item_width * static_cast<float>(routes.size()) + inset * 2.0f;
        const float bar_height = item_height + inset * 2.0f;
        const ImVec2 window_position = ImGui::GetWindowPos();
        const ImVec2 window_size = ImGui::GetWindowSize();
        const ImVec2 bar_min(
            std::round(window_position.x + (window_size.x - bar_width) * 0.5f),
            std::round(window_position.y + window_size.y - Scaled(kTabBarBottomMargin) -
                bar_height));
        const ImVec2 bar_max = Offset(bar_min, bar_width, bar_height);
        tab_bar_rect_ = ImRect(bar_min, bar_max);
        // The bar is a child window of its own, submitted after the route, so it
        // floats over the page and takes the pointer before the page does.
        ImGui::SetCursorScreenPos(bar_min);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("PlatformShellTabBar", ImVec2(bar_width, bar_height), false,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        ImDrawList* const list = ImGui::GetWindowDrawList();

        // The bar floats over the wallpaper and the stickers, so it is captured
        // fresh: everything the route drew is part of what it refracts.
        cabbird::glass::CaptureBackdrop(list);
        cabbird::glass::SurfaceOptions bar;
        bar.variant = cabbird::glass::Variant::Regular;
        bar.rounding = item_rounding + inset;
        bar.scale = scale;
        bar.shadow = false;
        bar.tint_strength = cabbird::glass::DarkAppearance() ? 0.30f : 0.16f;
        cabbird::glass::DrawSurface(list, bar_min, bar_max, bar);

        std::size_t selected_index = 0;
        for (std::size_t index = 0; index < routes.size(); ++index) {
            if (model_.State().route == routes[index]) selected_index = index;
        }
        {
            const ImVec2 item_min = Offset(bar_min,
                inset + item_width * static_cast<float>(selected_index), inset);
            const ImRect target(item_min, Offset(item_min, item_width, item_height));
            ImRect thumb = cabbird::glass::AnimateThumb(
                ImGui::GetID("##tab-bar-thumb"), target, bar_min);
            const float press = cabbird::glass::Animate(
                ImGui::GetID("##tab-bar-press"), tab_bar_held_ ? 1.0f : 0.0f, 20.0f);
            const float grow = press * Scaled(2.5f);
            thumb.Expand(ImVec2(grow * 1.4f, grow));
            cabbird::glass::DrawThumb(list, thumb.Min, thumb.Max, item_rounding + grow,
                scale, false);
        }

        bool held = false;
        for (std::size_t index = 0; index < routes.size(); ++index) {
            const Route route = routes[index];
            const bool selected = index == selected_index;
            const ImVec2 item_min = Offset(bar_min,
                inset + item_width * static_cast<float>(index), inset);
            ImGui::SetCursorScreenPos(item_min);
            std::string item_id{"##tab-"};
            item_id += cabbird::ToString(route);
            const bool clicked = ImGui::InvisibleButton(
                item_id.c_str(), ImVec2(item_width, item_height));
            const bool hovered = ImGui::IsItemHovered();
            held = held || ImGui::IsItemActive();
            // Hover only changes the ink of the icon and the label.
            const ImU32 ink = ImGui::GetColorU32(
                selected ? AccentInk() : hovered ? ThemeColor(theme.text) : InkColor(0.62f));
            const char* const glyph = ShellGlyphText(RouteGlyph(route));
            const ImVec2 glyph_center =
                cabbird::glass::GlyphInkCenter(icon_font, icon_size, glyph);
            const float center_x = item_min.x + item_width * 0.5f;
            const ImVec2 icon_center(center_x, item_min.y + item_height * 0.36f);
            list->AddText(icon_font, icon_size,
                ImVec2(std::round(icon_center.x - glyph_center.x),
                    std::round(icon_center.y - glyph_center.y)),
                ink, glyph);
            const std::string display_label = Ellipsize(RouteLabel(route),
                (std::max)(1.0f, item_width - Scaled(12.0f)));
            const char* const label = display_label.c_str();
            const ImVec2 label_extent =
                label_font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, label);
            list->AddText(label_font, label_size,
                ImVec2(std::round(center_x - label_extent.x * 0.5f),
                    std::round(item_min.y + item_height * 0.77f - label_extent.y * 0.5f)),
                ink, label);

            std::string badge;
            bool dot = false;
            if (route == Route::Plugins && snapshot.runtime_summary.issues != 0) {
                badge = std::to_string(snapshot.runtime_summary.issues);
            } else if (route == Route::Settings && SettingsDirty()) {
                dot = true;
            }
            if (!badge.empty() || dot) {
                const ImVec2 badge_center(icon_center.x + icon_size * 0.52f,
                    icon_center.y - icon_size * 0.40f);
                const float badge_radius = Scaled(dot ? 4.0f : 8.0f);
                list->AddCircleFilled(badge_center, badge_radius,
                    ImGui::GetColorU32(dot ? WarningColor() : ErrorColor()));
                if (!badge.empty()) {
                    const float badge_text = ImGui::GetFontSize() * 0.80f;
                    const ImVec2 extent = label_font->CalcTextSizeA(
                        badge_text, FLT_MAX, 0.0f, badge.c_str());
                    list->AddText(label_font, badge_text,
                        ImVec2(badge_center.x - extent.x * 0.5f, badge_center.y - extent.y * 0.5f),
                        IM_COL32(255, 255, 255, 255), badge.c_str());
                }
            }
            if (clicked) Navigate(route);
        }
        tab_bar_held_ = held;
        ImGui::EndChild();
    }

    void DrawShellWindowControls() {
        const bool locked = management_shell_locked_;
        const bool collapsed = ManagementShellCollapsed();
        const char* const collapse_tooltip = locked
            ? Text(cabbird::MessageId::ShellCollapseLocked)
            : Text(collapsed ? cabbird::MessageId::ShellExpand
                             : cabbird::MessageId::ShellCollapse);
        if (DrawShellHeaderControl("##shell-collapse", ShellHeaderControl::Collapse,
                collapsed, locked, collapse_tooltip)) {
            if (diagnostics_.standalone_window && g_window != nullptr) {
                ShowWindow(g_window->window, SW_MINIMIZE);
            } else {
                management_shell_collapsed_ = !management_shell_collapsed_;
            }
        }
        ImGui::SameLine(0.0f, Scaled(4.0f));
        const char* const lock_tooltip = locked
            ? Text(cabbird::MessageId::ShellUnlock)
            : Text(cabbird::MessageId::ShellLock);
        if (DrawShellHeaderControl("##shell-lock", ShellHeaderControl::Lock, locked,
                false, lock_tooltip)) {
            management_shell_locked_ = !locked;
            cabbird::SetHostUiCurrentWindowLocked(management_shell_locked_);
            if (!locked) management_shell_collapsed_ = false;
        }
        ImGui::SameLine(0.0f, Scaled(4.0f));
        if (DrawShellHeaderControl("##shell-close", ShellHeaderControl::Close,
                false, false, Text(cabbird::MessageId::ShellClose))) {
            if (diagnostics_.standalone_window && g_window != nullptr) {
                PostMessageW(g_window->window, WM_CLOSE, 0, 0);
            } else if (plugins_.UiResources().CloseWindow(
                    management_window_scope_, management_window_)) {
                cabbird::SetHostUiMenusCollapsed(true);
            }
        }
    }

    void DrawShellDragRegion(const ImVec2 origin, const ImVec2 size) const {
        if (diagnostics_.standalone_window) return;
        ImGuiWindow* const header = ImGui::GetCurrentWindow();
        if (header == nullptr) return;
        ImGuiWindow* const root = header->RootWindow == nullptr ? header : header->RootWindow;
        const float drag_width = ManagementShellGloballyCollapsed()
            ? size.x : (std::max)(0.0f,
                size.x - Scaled(kPlatformHeaderActionColumnWidth));
        if (drag_width <= 0.0f) return;
        const ImRect bounds(origin,
            Offset(origin, drag_width, Scaled(kPlatformHeaderHeight)));
        bool hovered{};
        bool held{};
        const ImGuiID id = header->GetID("##platform-shell-drag");
        ImGui::KeepAliveID(id);
        const bool pressed = ImGui::ButtonBehavior(
            bounds, id, &hovered, &held,
            ImGuiButtonFlags_NoNavFocus | ImGuiButtonFlags_PressedOnClick);
        if (pressed) ImGui::StartMouseMovingWindow(root);
    }

    void DrawShellHeader() {
        const auto& theme = cabbird::CabbirdUiTheme();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColor(theme.header_background));
        ImGui::BeginChild("PlatformGlobalHeader",
            ImVec2(0.0f, Scaled(kPlatformHeaderHeight)), false,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 origin = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        ImDrawList* const list = ImGui::GetWindowDrawList();
        ImGui::SetCursorScreenPos(ScaledOffset(origin, 16.0f, 11.0f));
        const float logo_size = Scaled(kPlatformHeaderLogoSize);
        const bool logo_drawn = (logo_texture_ && plugins_.DrawUiTexture(
            management_window_scope_, logo_texture_, logo_size,
            logo_size, 0xffffffffU)) ||
            DrawStandaloneHeaderLogo(logo_size, logo_size);
        if (!logo_drawn) ImGui::Dummy(ImVec2(logo_size, logo_size));
        const bool strip = ManagementShellCollapsed();
        {
            // A fixed product title, drawn from a bake of its own size.
            const float title_scale = 1.45f;
            ImFont* const title_font =
                FontForScale(PlatformUiScale() * title_scale);
            const float title_size = Scaled(cabbird::CabbirdUiBaseFontSize() * title_scale);
            list->AddText(title_font, title_size,
                ImVec2(std::round(origin.x + Scaled(56.0f)),
                    std::round(origin.y + (size.y - title_size) * 0.5f)),
                ImGui::ColorConvertFloat4ToU32(ThemeColor(theme.text)),
                Text(cabbird::MessageId::ApplicationTitle));
        }
        // Page actions go in before the drag region so they own the pointer.
        if (!strip && page_header_actions_) {
            const ImVec2 area_origin = Offset(origin, Scaled(40.0f), Scaled(1.0f));
            const ImVec2 area_size(std::max(0.0f, size.x - Scaled(40.0f) -
                    Scaled(kPlatformHeaderActionColumnWidth) - Scaled(6.0f)),
                Scaled(kPlatformHeaderHeight));
            page_header_actions_(area_origin, area_size);
        }
        DrawShellDragRegion(origin, size);
        if (!ManagementShellGloballyCollapsed()) {
            const ImVec2 controls = Offset(
                origin, size.x - Scaled(kPlatformHeaderActionColumnWidth), Scaled(11.0f));
            DrawGlass(list, Offset(controls, -Scaled(4.0f), -Scaled(4.0f)),
                Offset(controls, Scaled(102.0f), Scaled(34.0f)),
                cabbird::glass::Variant::Regular, 12.0f);
            ImGui::SetCursorScreenPos(controls);
            DrawShellWindowControls();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // Publishes the route's header actions; the global header draws them
    // after the route. `actions` receives the origin and size of the header
    // area left of the window controls. The title is fixed to the product
    // name, so the route's own title is not shown.
    template <typename DrawActions>
    void DrawShellPageHeader(
        const char*, const std::string_view, DrawActions&& actions, const float = 0.0f) {
        page_header_actions_ = std::forward<DrawActions>(actions);
    }

    void DrawShellProviderUnavailable(const char* subject, const char* reason) {
        BeginShellBodyChild("PlatformProviderUnavailable");
        ImGui::SetCursorPosY(std::max(24.0f, ImGui::GetContentRegionAvail().y * 0.18f));
        const std::array<std::string_view, 1> arguments{subject};
        const std::string unavailable =
            Format(cabbird::MessageId::PluginsProviderUnavailable, arguments);
        ImGui::TextColored(WarningColor(), "!  %s", unavailable.c_str());
        ImGui::PushTextWrapPos(std::min(620.0f, ImGui::GetContentRegionAvail().x));
        ImGui::TextDisabled("%s", reason);
        ImGui::PopTextWrapPos();
        const std::string open_diagnostics = StableLabel(
            cabbird::MessageId::PluginsOpenDiagnostics, "provider-open-diagnostics");
        if (ImGui::Button(open_diagnostics.c_str())) {
            Navigate(Route::Diagnostics);
            model_.State().diagnostics_tab = DiagnosticTab::Overview;
        }
        EndShellBodyChild();
    }

    [[nodiscard]] const cabbird::RepositoryPluginView* RepositoryPlugin(
        std::string_view plugin_id) const noexcept {
        const auto& plugins = model_.Snapshot().repository.plugins;
        const auto found = std::ranges::find_if(plugins, [&](const auto& plugin) {
            return plugin.entry.internal_name == plugin_id;
        });
        return found == plugins.end() ? nullptr : &*found;
    }

    [[nodiscard]] const cabbird::RepositoryOperationView* RepositoryOperation(
        std::string_view plugin_id) const noexcept {
        const auto& operations = model_.Snapshot().repository.operations;
        const auto found = std::find_if(operations.rbegin(), operations.rend(),
            [&](const auto& operation) { return operation.plugin_id == plugin_id; });
        return found == operations.rend() ? nullptr : &*found;
    }

    void SubmitRepositoryInstall(const cabbird::RepositoryPluginView& plugin) {
        if (!diagnostics_.repository_install) {
            status_ = Text(cabbird::MessageId::RepositoryInstallProviderUnavailable);
            status_failure_ = true;
            return;
        }
        const auto submission = diagnostics_.repository_install(
            plugin.entry.internal_name, plugin.entry.version);
        if (submission.accepted) {
            status_ = Text(cabbird::MessageId::RepositoryDownloadQueued);
            status_failure_ = false;
        } else {
            const std::array<std::string_view, 1> arguments{submission.message};
            status_ = Format(cabbird::MessageId::RepositoryInstallFailed, arguments);
            status_failure_ = true;
        }
    }

    void RequestRepositoryUninstall(std::string_view plugin_id, std::string_view display_name) {
        const auto* operation = RepositoryOperation(plugin_id);
        if (operation != nullptr &&
            operation->state != cabbird::RepositoryOperationState::Succeeded &&
            operation->state != cabbird::RepositoryOperationState::Failed &&
            operation->state != cabbird::RepositoryOperationState::Cancelled) {
            status_ = Text(cabbird::MessageId::OperationInProgress);
            status_failure_ = false;
            return;
        }
        repository_uninstall_plugin_id_ = plugin_id;
        repository_uninstall_plugin_name_ = display_name.empty() ? plugin_id : display_name;
        repository_uninstall_popup_requested_ = true;
    }

    void SubmitRepositoryUninstall() {
        const std::string plugin_id = std::exchange(repository_uninstall_plugin_id_, {});
        repository_uninstall_plugin_name_.clear();
        if (!diagnostics_.repository_uninstall) {
            status_ = Text(cabbird::MessageId::RepositoryUninstallProviderUnavailable);
            status_failure_ = true;
            return;
        }
        const auto submission = diagnostics_.repository_uninstall(plugin_id);
        if (submission.accepted) {
            status_ = Text(cabbird::MessageId::RepositoryUninstallQueued);
            status_failure_ = false;
        } else {
            const std::array<std::string_view, 1> arguments{submission.message};
            status_ = Format(cabbird::MessageId::RepositoryUninstallFailed, arguments);
            status_failure_ = true;
        }
    }

    void DrawRepositoryWorkspace(bool updates_only) {
        auto& state = model_.State();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        SetAvailableItemWidth(0.0f, 360.0f);
        if (ImGui::InputTextWithHint("##RepositorySearch",
                Text(cabbird::MessageId::PluginsSearchRepositoryHint), search_.data(),
                search_.size())) {
            state.search = search_.data();
        }
        ImGui::PopStyleVar();
        ImGui::Separator();

        std::size_t visible{};
        for (const auto& plugin : model_.Snapshot().repository.plugins) {
            if (!state.search.empty() &&
                plugin.entry.internal_name.find(state.search) == std::string::npos &&
                plugin.entry.name.find(state.search) == std::string::npos) {
                continue;
            }
            const auto* installed = model_.Snapshot().FindPlugin(plugin.entry.internal_name);
            const auto* operation = RepositoryOperation(plugin.entry.internal_name);
            const auto install_state = cabbird::ResolveRepositoryPluginInstallState(
                installed, plugin);
            if (updates_only && !install_state.update_available) continue;
            if (updates_only && installed != nullptr && settings_snapshot_.ready &&
                !settings_snapshot_.values.updates_include_disabled && installed->IsDisabled()) {
                continue;
            }
            ++visible;

            ImGui::PushID(plugin.entry.internal_name.c_str());
            const bool pending = operation != nullptr &&
                operation->state != cabbird::RepositoryOperationState::Succeeded &&
                operation->state != cabbird::RepositoryOperationState::Failed &&
                operation->state != cabbird::RepositoryOperationState::Cancelled;
            const bool current = install_state.installed && !install_state.update_available;
            const bool enabled = !pending && (current || plugin.compatible);
            const char* label = pending ? DisplayRepositoryOperationState(operation->state)
                : current ? Text(cabbird::MessageId::CommonUninstall)
                : install_state.update_available ? Text(cabbird::MessageId::CommonUpdate)
                         : Text(cabbird::MessageId::CommonInstall);
            const float button_width = 104.0f;
            if (ImGui::BeginTable("RepositoryRow", 2,
                    ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, 0.0f))) {
                ImGui::TableSetupColumn(
                    Text(cabbird::MessageId::CommonPlugin), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(
                    Text(cabbird::MessageId::CommonAction),
                    ImGuiTableColumnFlags_WidthFixed, button_width);
                ImGui::TableNextRow(ImGuiTableRowFlags_None, 52.0f);
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(plugin.entry.name.empty()
                    ? plugin.entry.internal_name.c_str()
                    : plugin.entry.name.c_str());
                const std::array<std::string_view, 3> metadata_arguments{
                    plugin.entry.version, plugin.entry.author, plugin.entry.punchline};
                ImGui::TextDisabled("%s", Format(
                    cabbird::MessageId::RepositoryVersionMetadata,
                    metadata_arguments).c_str());
                if (!plugin.compatible) {
                    ImGui::TextColored(WarningColor(), "%s", plugin.compatibility_reason.c_str());
                }
                if (operation != nullptr &&
                    (operation->state == cabbird::RepositoryOperationState::Failed ||
                     operation->state == cabbird::RepositoryOperationState::Succeeded)) {
                    ImGui::TextDisabled("%s", operation->message.c_str());
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
                ImGui::BeginDisabled(!enabled);
                if (ImGui::Button(label, ImVec2(button_width, 30.0f))) {
                    if (current) {
                        RequestRepositoryUninstall(plugin.entry.internal_name,
                            plugin.entry.name);
                    } else {
                        SubmitRepositoryInstall(plugin);
                    }
                }
                ImGui::EndDisabled();
                ImGui::EndTable();
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        if (visible == 0) {
            ImGui::Spacing();
            ImGui::TextDisabled("%s", Text(updates_only
                ? cabbird::MessageId::PluginsInstalledUpToDate
                : cabbird::MessageId::PluginsNoRepositoryMatch));
        }
    }

    void DrawShellRoute() {
        switch (model_.State().route) {
        case Route::Plugins: DrawPluginsShell(); break;
        case Route::UnityCompatibility: DrawDiagnosticsShell(); break;
        case Route::Diagnostics: DrawDiagnosticsShell(); break;
        case Route::Settings: DrawSettingsShell(); break;
        }
    }

    void ResolveCompactPluginDetail() {
        if (!IsCompact() || compact_plugin_detail_pending_id_.empty()) return;
        if (ImGui::GetTime() - compact_plugin_detail_requested_at_ <
            ImGui::GetIO().MouseDoubleClickTime) {
            return;
        }
        if (model_.State().route == Route::Plugins &&
            model_.State().plugin_tab == Tab::Installed &&
            model_.State().selected_plugin_id == compact_plugin_detail_pending_id_) {
            compact_plugin_detail_ = true;
        }
        compact_plugin_detail_pending_id_.clear();
    }

    void DrawPluginsShell() {
        auto& state = model_.State();
        ResolveCompactPluginDetail();
        const bool compact_detail = IsCompact() && compact_plugin_detail_ &&
            state.plugin_tab == Tab::Installed;
        const auto* selected = model_.Snapshot().FindPlugin(state.selected_plugin_id);
        if (compact_detail && selected != nullptr) {
            DrawShellPageHeader(Text(cabbird::MessageId::ShellRoutePlugins), {},
                [](const ImVec2&, const ImVec2&) {});
            if (DrawShellIconButton("back-to-plugin-list", ShellGlyph::ChevronLeft,
                    Text(cabbird::MessageId::CommonBack))) {
                compact_plugin_detail_ = false;
            }
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                (Scaled(30.0f) - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::TextUnformatted(
                selected->name.empty() ? selected->id.c_str() : selected->name.c_str());
            ImGui::BeginChild("PlatformCompactPluginDetail", ImVec2(0.0f, 0.0f), false,
                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            DrawPluginPublicDetail(*selected);
            ImGui::EndChild();
            return;
        }

        const bool installed = state.plugin_tab == Tab::Installed;
        const bool third_party = state.plugin_tab == Tab::ThirdParty;
        const bool repository_catalog =
            state.plugin_tab == Tab::Available || state.plugin_tab == Tab::Updates;
        const auto& repository = model_.Snapshot().repository;
        const std::string_view repository_subtitle = repository.BrowseAvailable()
            ? DisplayRepositoryState(repository.state)
            : repository.reason;
        DrawShellPageHeader(Text(cabbird::MessageId::ShellRoutePlugins),
            installed ? std::string_view{} : repository_subtitle,
            [this, installed, repository_catalog](const ImVec2& origin, const ImVec2& size) {
                const float more_x = size.x - Scaled(42.0f);
                const ImVec2 disc = Offset(origin, more_x, Scaled(10.0f));
                DrawGlass(ImGui::GetWindowDrawList(), Offset(disc, -Scaled(4.0f), -Scaled(4.0f)),
                    Offset(disc, Scaled(34.0f), Scaled(34.0f)),
                    cabbird::glass::Variant::Regular, 12.0f);
                ImGui::SetCursorScreenPos(disc);
                if (repository_catalog) {
                    if (DrawShellIconButton("repository-refresh", ShellGlyph::Refresh,
                            Text(cabbird::MessageId::PluginsRefreshRepositories)) &&
                        diagnostics_.repository_refresh) {
                        const auto submission = diagnostics_.repository_refresh();
                        if (submission.accepted) {
                            status_ = Text(cabbird::MessageId::RepositoryRefreshQueued);
                            status_failure_ = false;
                        } else {
                            const std::array<std::string_view, 1> arguments{submission.message};
                            status_ = Format(
                                cabbird::MessageId::RepositoryRefreshFailed, arguments);
                            status_failure_ = true;
                        }
                    }
                    return;
                }
                if (installed && DrawShellIconButton("plugin-page-more", ShellGlyph::More,
                        Text(cabbird::MessageId::PluginsMoreActions))) {
                    ImGui::OpenPopup("Plugin page actions");
                }
                if (ImGui::BeginPopup("Plugin page actions")) {
                    const bool reload_pending = HasPendingMutation({}, Mutation::ReloadAll);
                    if (ImGui::MenuItem(Text(cabbird::MessageId::PluginsReloadAll),
                            nullptr, false, !reload_pending)) {
                        SubmitIntent(model_.NewIntent(
                            cabbird::PlatformUiIntentKind::ReloadAllInstalled, {}, Mutation::ReloadAll));
                    }
                    if (reload_pending) {
                        DrawTooltip(Text(cabbird::MessageId::PluginsReloadAllPending));
                    }
                    ImGui::EndPopup();
                }
            });

        ImGui::BeginChild("PlatformPluginsRoute", ImVec2(0.0f, 0.0f), false,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawPluginsTopBar(installed);
        if (third_party) {
            BeginShellBodyChild("PlatformThirdPartyPlugins", {}, true);
            DrawRepositoryChannelsEditor();
            EndShellBodyChild();
        } else if (!installed) {
            if (repository.BrowseAvailable()) {
                BeginShellBodyChild("PlatformRepositoryCatalog", {}, true);
                DrawRepositoryWorkspace(state.plugin_tab == Tab::Updates);
                EndShellBodyChild();
            } else {
                DrawShellProviderUnavailable(
                    Text(state.plugin_tab == Tab::Available
                        ? cabbird::MessageId::PluginsCatalogTitle
                        : cabbird::MessageId::PluginsUpdatesTitle),
                    repository.reason.c_str());
            }
        } else {
            DrawInstalledPluginWorkspace();
        }
        ImGui::EndChild();
    }

    void DrawPluginsTopBar(const bool installed) {
        auto& state = model_.State();
        const std::size_t installed_count = model_.Snapshot().runtime_summary.installed;
        const std::string installed_label =
            std::string(PluginTabLabel(Tab::Installed)) + "  " + std::to_string(installed_count);
        const std::array<const char*, 4> tab_labels{installed_label.c_str(),
            PluginTabLabel(Tab::Available), PluginTabLabel(Tab::Updates),
            PluginTabLabel(Tab::ThirdParty)};
        cabbird::glass::SegmentedOptions tab_options;
        tab_options.height = 40.0f;
        tab_options.scale = PlatformUiScale();
        tab_options.padding_x = IsCompact() ? 10.0f : 16.0f;
        const float tabs_width = cabbird::glass::SegmentedControlWidth(tab_labels, tab_options);
        const std::array<const char*, 5> filter_labels{FilterLabel(Filter::All),
            FilterLabel(Filter::Favorites), FilterLabel(Filter::Running),
            FilterLabel(Filter::Disabled), FilterLabel(Filter::Issues)};
        const float filter_width = IsCompact() ? Scaled(100.0f)
            : cabbird::glass::SegmentedControlWidth(filter_labels, FilterSegmentOptions());
        const float sort_width = Scaled(IsCompact() ? 86.0f : 112.0f);
        const float edge = Scaled(4.0f);
        const float gap = Scaled(10.0f);
        const float row_height = Scaled(52.0f);
        const float available = ImGui::GetContentRegionAvail().x;
        const float search_collapsed = Scaled(40.0f);
        const bool one_row = !installed || available >= edge + tabs_width + gap +
            search_collapsed + Scaled(8.0f) + filter_width + Scaled(8.0f) + sort_width + edge;
        ImGui::BeginChild("PlatformPluginsTopBar",
            ImVec2(0.0f, one_row ? row_height : row_height * 2.0f), false,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 origin = ImGui::GetWindowPos();
        const float width = ImGui::GetWindowSize().x;
        ImGui::SetCursorScreenPos(Offset(origin, edge, Scaled(6.0f)));
        int selected = static_cast<int>(state.plugin_tab);
        if (cabbird::glass::SegmentedControl("plugin-tabs", tab_labels, selected, tab_options)) {
            state.plugin_tab = static_cast<Tab>(selected);
            compact_plugin_detail_ = false;
            compact_plugin_detail_pending_id_.clear();
        }
        if (installed) {
            const float controls_y = one_row ? Scaled(8.0f) : row_height + Scaled(8.0f);
            const float sort_x = width - edge - sort_width;
            const float filter_x = sort_x - Scaled(8.0f) - filter_width;
            // The search field is anchored to the filter and grows to the
            // left; on its own row it may reach the left edge.
            const float search_right = filter_x - Scaled(8.0f);
            const float search_limit = one_row ? edge + tabs_width + gap : edge;
            DrawPluginSearchField(Offset(origin, search_right, controls_y),
                std::max(search_collapsed, std::min(Scaled(320.0f), search_right - search_limit)),
                search_collapsed);
            ImGui::SetCursorScreenPos(Offset(origin, filter_x, controls_y));
            DrawPluginFilterControl(IsCompact());
            ImGui::SetCursorScreenPos(Offset(origin, sort_x, controls_y));
            DrawPluginSortControl();
        }
        ImGui::EndChild();
    }

    // A search field that rests as a magnifier button and opens to the left,
    // toward `expanded` width, while the pointer is over it, while it has
    // keyboard focus and for as long as it holds text. `right` is the
    // top-right corner it is anchored to.
    void DrawPluginSearchField(const ImVec2 right, const float expanded, const float collapsed) {
        auto& state = model_.State();
        const auto& theme = cabbird::CabbirdUiTheme();
        const float height = Scaled(36.0f);
        const float shown_width = collapsed + (expanded - collapsed) * plugin_search_reveal_;
        const ImVec2 field_min(right.x - shown_width, right.y);
        const ImVec2 field_max(right.x, right.y + height);
        const bool pointer_over = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            ImGui::IsMouseHoveringRect(field_min, field_max);
        const bool open = pointer_over || plugin_search_active_ || focus_plugin_search_ ||
            !state.search.empty();
        const float target = open ? 1.0f : 0.0f;
        if (cabbird::glass::ReduceMotion()) {
            plugin_search_reveal_ = target;
        } else {
            const float step = 1.0f - std::exp(-16.0f * std::max(0.0f, ImGui::GetIO().DeltaTime));
            plugin_search_reveal_ += (target - plugin_search_reveal_) * step;
            if (std::abs(target - plugin_search_reveal_) < 0.002f) plugin_search_reveal_ = target;
        }
        const float reveal = plugin_search_reveal_;
        const float field_width = collapsed + (expanded - collapsed) * reveal;
        const ImVec2 min(right.x - field_width, right.y);
        ImDrawList* const list = ImGui::GetWindowDrawList();
        const ImVec4 fill = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
        if (reveal < 0.02f) {
            // Resting state: just the magnifier on a small rounded tile.
            ImGui::SetCursorScreenPos(min);
            if (ImGui::InvisibleButton("##plugin-search-button", ImVec2(collapsed, height))) {
                focus_plugin_search_ = true;
            }
            list->AddRectFilled(min, Offset(min, collapsed, height),
                ImGui::GetColorU32(fill), Scaled(11.0f));
            const char* const glyph = ShellGlyphText(ShellGlyph::Search);
            const ImVec2 glyph_size = ImGui::CalcTextSize(glyph);
            list->AddText(Offset(min, (collapsed - glyph_size.x) * 0.5f,
                    (height - ImGui::GetTextLineHeight()) * 0.5f + Scaled(1.0f)),
                ImGui::GetColorU32(ImGui::IsItemHovered()
                    ? ThemeColor(theme.text) : ThemeColor(theme.text_muted)), glyph);
            DrawTooltip(Text(cabbird::MessageId::PluginsSearchHint));
            plugin_search_active_ = false;
            return;
        }
        ImGui::SetCursorScreenPos(min);
        if (focus_plugin_search_) {
            ImGui::SetKeyboardFocusHere();
            focus_plugin_search_ = false;
        }
        ImGui::SetNextItemWidth(field_width);
        ImGui::SetNextItemAllowOverlap();
        const bool show_clear = !state.search.empty() && reveal > 0.6f;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Scaled(30.0f), FieldPaddingY()));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Scaled(11.0f));
        if (ImGui::InputTextWithHint("##plugin-search",
                reveal > 0.6f ? Text(cabbird::MessageId::PluginsSearchHint) : "",
                search_.data(), search_.size())) {
            state.search = search_.data();
            cabbird::ReconcileUiStateSelection(model_.Snapshot(), state, developer_mode_);
        }
        ImGui::PopStyleVar(2);
        plugin_search_active_ = ImGui::IsItemActive();
        const ImVec2 field_rect_min = ImGui::GetItemRectMin();
        const ImVec2 field_rect_max = ImGui::GetItemRectMax();
        list->AddText(FieldGlyphPosition(),
            ImGui::ColorConvertFloat4ToU32(ThemeColor(theme.text_muted)),
            ShellGlyphText(ShellGlyph::Search));
        if (show_clear) {
            ImGui::SetCursorScreenPos(ImVec2(field_rect_max.x - Scaled(33.0f),
                field_rect_min.y + (field_rect_max.y - field_rect_min.y - Scaled(30.0f)) * 0.5f));
            if (DrawShellIconButton("clear-plugin-search", ShellGlyph::Close,
                    Text(cabbird::MessageId::PluginsClearSearch))) {
                search_.fill('\0');
                state.search.clear();
                cabbird::ReconcileUiStateSelection(model_.Snapshot(), state, developer_mode_);
                focus_plugin_search_ = true;
            }
        }
    }

    void DrawPluginFilterControl(const bool compact_control) {
        auto& state = model_.State();
        constexpr std::array<Filter, 5> filters{
            Filter::All, Filter::Favorites, Filter::Running, Filter::Disabled, Filter::Issues};
        if (compact_control) {
            ImGui::SetNextItemWidth(Scaled(100.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Scaled(11.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                ImVec2(ImGui::GetStyle().FramePadding.x, FieldPaddingY()));
            const bool filter_open =
                cabbird::glass::BeginCombo("##plugin-filter", FilterLabel(state.plugin_filter));
            ImGui::PopStyleVar(2);
            if (filter_open) {
                for (const Filter filter : filters) {
                    if (ImGui::Selectable(FilterLabel(filter), state.plugin_filter == filter)) {
                        state.plugin_filter = filter;
                        cabbird::ReconcileUiStateSelection(
                            model_.Snapshot(), state, developer_mode_);
                    }
                }
                ImGui::EndCombo();
            }
            DrawTooltip(Text(cabbird::MessageId::PluginsFilterAll));
            return;
        }
        const std::array<const char*, 5> labels{FilterLabel(filters[0]),
            FilterLabel(filters[1]), FilterLabel(filters[2]), FilterLabel(filters[3]),
            FilterLabel(filters[4])};
        // The segmented control reports the displayed slot, and the slots no
        // longer follow the enum order, so both directions go through `filters`.
        int selected = 0;
        for (std::size_t index = 0; index < filters.size(); ++index) {
            if (filters[index] == state.plugin_filter) {
                selected = static_cast<int>(index);
                break;
            }
        }
        if (cabbird::glass::SegmentedControl(
                "plugin-filter", labels, selected, FilterSegmentOptions())) {
            if (selected >= 0 && static_cast<std::size_t>(selected) < filters.size()) {
                state.plugin_filter = filters[static_cast<std::size_t>(selected)];
            }
            cabbird::ReconcileUiStateSelection(model_.Snapshot(), state, developer_mode_);
        }
    }

    [[nodiscard]] static cabbird::glass::SegmentedOptions FilterSegmentOptions() noexcept {
        cabbird::glass::SegmentedOptions options;
        options.height = 36.0f;
        options.padding_x = 11.0f;
        options.scale = PlatformUiScale();
        options.glass_track = false;
        return options;
    }

    void DrawPluginSortControl() {
        auto& state = model_.State();
        ImGui::SetNextItemWidth(Scaled(IsCompact() ? 86.0f : 112.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Scaled(11.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
            ImVec2(ImGui::GetStyle().FramePadding.x, FieldPaddingY()));
        const bool sort_open = cabbird::glass::BeginCombo("##plugin-sort", SortLabel(state.plugin_sort));
        ImGui::PopStyleVar(2);
        if (sort_open) {
            constexpr std::array<Sort, 3> sorts{Sort::Name, Sort::State, Sort::Author};
            for (const Sort sort : sorts) {
                const char* label = SortLabel(sort);
                ImGui::PushID(static_cast<int>(sort));
                if (ImGui::Selectable(label, state.plugin_sort == sort)) {
                    state.plugin_sort = sort;
                    cabbird::ReconcileUiStateSelection(
                        model_.Snapshot(), state, developer_mode_);
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        DrawTooltip(Text(cabbird::MessageId::PluginsSortName));
    }

    void DrawPluginRows(const std::vector<cabbird::InstalledPluginView>& visible) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible.size()), Scaled(IsCompact() ? 60.0f : 64.0f));
        if (!keyboard_plugin_focus_id_.empty()) {
            const auto focus = std::find_if(visible.begin(), visible.end(), [&](const auto& plugin) {
                return plugin.id == keyboard_plugin_focus_id_;
            });
            if (focus != visible.end()) {
                clipper.IncludeItemByIndex(static_cast<int>(focus - visible.begin()));
            }
        }
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto index = static_cast<std::size_t>(row);
                DrawShellPluginRow(visible[index],
                    index == 0 ? std::string_view{} : std::string_view{visible[index - 1].id},
                    index + 1 == visible.size() ? std::string_view{}
                                               : std::string_view{visible[index + 1].id});
            }
        }
        if (layout_mode_ != LayoutMode::Wide) {
            ImGui::Dummy(ImVec2(0.0f, FloatingBarClearance()));
        }
    }

    void DrawInstalledPluginWorkspace() {
        const auto visible = model_.VisiblePlugins(developer_mode_);
        if (!visible.empty() && std::ranges::none_of(visible, [&](const auto& plugin) {
                return plugin.id == model_.State().selected_plugin_id;
            })) {
            model_.State().selected_plugin_id = visible.front().id;
        }
        keyboard_plugin_navigation_consumed_ = false;
        if (visible.empty()) {
            keyboard_plugin_focus_id_.clear();
            BeginShellBodyChild("PlatformPluginEmpty", {}, true);
            if (model_.Snapshot().installed_plugins.empty()) {
                ImGui::TextDisabled("%s", Text(cabbird::MessageId::PluginsNoInstalled));
            } else if (!model_.State().search.empty()) {
                const std::array<std::string_view, 1> arguments{model_.State().search};
                const std::string no_match =
                    Format(cabbird::MessageId::PluginsNoSearchMatch, arguments);
                ImGui::TextUnformatted(no_match.c_str());
                const std::string clear_search = StableLabel(
                    cabbird::MessageId::PluginsClearSearch, "empty-clear-search");
                if (ImGui::Button(clear_search.c_str())) {
                    search_.fill('\0');
                    model_.State().search.clear();
                    focus_plugin_search_ = true;
                }
            } else {
                ImGui::TextDisabled("%s", Text(cabbird::MessageId::PluginsNoFilterMatch));
                const std::string reset_filters = StableLabel(
                    cabbird::MessageId::PluginsResetFilters, "empty-reset-filters");
                if (ImGui::Button(reset_filters.c_str())) {
                    model_.State().plugin_filter = Filter::All;
                    cabbird::ReconcileUiStateSelection(
                        model_.Snapshot(), model_.State(), developer_mode_);
                }
            }
            EndShellBodyChild();
            return;
        }

        if (IsCompact()) {
            BeginCardChild("PlatformPluginList", ImVec2(0.0f, 0.0f), 6.0f);
            DrawPluginRows(visible);
            ImGui::EndChild();
            return;
        }

        const float available = ImGui::GetContentRegionAvail().x;
        const float list_width = PluginListWidth(available);
        BeginCardChild("PlatformPluginList", ImVec2(list_width, 0.0f), 6.0f);
        DrawPluginRows(visible);
        ImGui::EndChild();
        ImGui::SameLine(0.0f, 0.0f);
        const ImVec2 splitter_origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("PlatformPluginSplit",
            ImVec2(Scaled(10.0f), ImGui::GetContentRegionAvail().y));
        if (ImGui::IsItemActive()) {
            plugin_list_width_ =
                (list_width + ImGui::GetIO().MouseDelta.x) / PlatformUiScale();
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            const float middle = splitter_origin.y + ImGui::GetItemRectSize().y * 0.5f;
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(splitter_origin.x + Scaled(3.5f), middle - Scaled(18.0f)),
                ImVec2(splitter_origin.x + Scaled(6.5f), middle + Scaled(18.0f)),
                ImGui::GetColorU32(InkColor(ImGui::IsItemActive() ? 0.40f : 0.24f)), Scaled(2.0f));
        }
        ImGui::SameLine(0.0f, 0.0f);
        BeginCardChild("PlatformPluginDetail", ImVec2(0.0f, 0.0f), 0.0f,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const auto* selected = model_.Snapshot().FindPlugin(model_.State().selected_plugin_id);
        if (selected != nullptr) DrawPluginPublicDetail(*selected);
        else ImGui::TextDisabled("%s", Text(cabbird::MessageId::PluginsSelectDetails));
        ImGui::EndChild();
    }

    [[nodiscard]] bool PluginToggleAllowed(
        const cabbird::InstalledPluginView& plugin) const noexcept {
        if (HasPendingMutation(plugin.id, Mutation::None)) {
            return false;
        }
        const auto state = plugin.UiState();
        if (state == cabbird::PlatformUiPluginState::Quarantined ||
            state == cabbird::PlatformUiPluginState::Stopping) {
            return false;
        }
        return plugin.enabled || state == cabbird::PlatformUiPluginState::Disabled;
    }

    [[nodiscard]] std::string PluginToggleDisabledReason(
        const cabbird::InstalledPluginView& plugin) const {
        if (HasPendingMutation(plugin.id, Mutation::None)) {
            return Text(cabbird::MessageId::OperationInProgress);
        }
        switch (plugin.UiState()) {
        case cabbird::PlatformUiPluginState::Quarantined:
            return Text(cabbird::MessageId::PluginStateQuarantined);
        case cabbird::PlatformUiPluginState::Stopping:
            return Text(cabbird::MessageId::PluginStateStopping);
        default: return Text(cabbird::MessageId::PluginsNeedsAttention);
        }
    }

    void SubmitPluginToggle(const cabbird::InstalledPluginView& plugin) {
        Intent intent = model_.NewIntent(cabbird::PlatformUiIntentKind::SetPluginEnabled, plugin.id);
        intent.bool_value = !plugin.enabled;
        SubmitIntent(std::move(intent));
    }

    [[nodiscard]] bool IsPluginFavorite(const cabbird::InstalledPluginView& plugin) const {
        return model_.State().favorite_plugin_ids.contains(plugin.id);
    }

    [[nodiscard]] bool DrawFavoriteToggle(const char* id, const bool favorite) const {
        const auto& theme = cabbird::CabbirdUiTheme();
        ImGui::PushID(id);
        const bool pressed = ImGui::InvisibleButton("##favorite", Scaled(30.0f, 30.0f));
        const bool hovered = ImGui::IsItemHovered();
        DrawGlyphCentred(favorite ? ShellGlyph::StarFilled : ShellGlyph::Star,
            ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
            ImGui::GetColorU32(ThemeColor(favorite
                ? (hovered ? theme.accent_hovered : theme.accent)
                : (hovered ? theme.text : theme.text_muted))), ImGui::GetFontSize() * 1.45f);
        ImGui::PopID();
        if (hovered) ImGui::SetTooltip("%s", Text(favorite
            ? cabbird::MessageId::PluginsRemoveFavorite : cabbird::MessageId::PluginsAddFavorite));
        return pressed;
    }

    void TogglePluginFavorite(const cabbird::InstalledPluginView& plugin) {
        auto& favorites = model_.State().favorite_plugin_ids;
        const bool favorite = !favorites.contains(plugin.id);
        if (favorite) favorites.insert(plugin.id);
        else favorites.erase(plugin.id);
        plugins_.SetPluginFavorite(plugin.id, favorite);
        cabbird::ReconcileUiStateSelection(model_.Snapshot(), model_.State(), developer_mode_);
    }

    void DrawShellPluginRow(const cabbird::InstalledPluginView& plugin,
        const std::string_view previous_plugin_id, const std::string_view next_plugin_id) {
        const float height = Scaled(IsCompact() ? 60.0f : 64.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 toggle_size = Scaled(40.0f, 24.0f);
        const float toggle_x = width - toggle_size.x - Scaled(12.0f);
        const ImVec2 toggle_position = Offset(
            origin, toggle_x, (height - toggle_size.y) * 0.5f);
        const bool toggle_hovered = ImGui::IsMouseHoveringRect(
            toggle_position, Offset(toggle_position, toggle_size.x, toggle_size.y));
        const bool selected = model_.State().selected_plugin_id == plugin.id;
        ImGui::PushID(plugin.id.c_str());
        // Keep the row hit target clear of the toggle. Overlapping invisible
        // buttons otherwise leave the later toggle unable to own the click.
        if (keyboard_plugin_focus_id_ == plugin.id) {
            ImGui::SetKeyboardFocusHere();
            keyboard_plugin_focus_id_.clear();
        }
        const bool row_clicked = ImGui::InvisibleButton("row", ImVec2(
            std::max(0.0f, toggle_x - Scaled(4.0f)), height));
        const bool row_focused = ImGui::IsItemFocused();
        const bool row_right_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        const bool enter_pressed = row_focused && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        const bool row_hovered = ImGui::IsWindowHovered() &&
            ImGui::IsMouseHoveringRect(origin, Offset(origin, width, height));
        if (row_clicked) {
            model_.State().selected_plugin_id = plugin.id;
            if (IsCompact() && !enter_pressed) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    compact_plugin_detail_pending_id_.clear();
                    if (!plugin.IsRunning() || !plugin.visibility_control) {
                        compact_plugin_detail_ = true;
                    }
                } else {
                    compact_plugin_detail_pending_id_ = plugin.id;
                    compact_plugin_detail_requested_at_ = ImGui::GetTime();
                }
            }
        }
        if (row_focused && !keyboard_plugin_navigation_consumed_) {
            std::string_view next_focus;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) next_focus = previous_plugin_id;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) next_focus = next_plugin_id;
            if (!next_focus.empty()) {
                model_.State().selected_plugin_id = std::string(next_focus);
                keyboard_plugin_focus_id_ = next_focus;
                compact_plugin_detail_pending_id_.clear();
                keyboard_plugin_navigation_consumed_ = true;
            }
            if (enter_pressed && plugin.IsRunning() &&
                plugin.visibility_control) {
                Intent intent = model_.NewIntent(
                    cabbird::PlatformUiIntentKind::SetPluginVisible, plugin.id,
                    Mutation::SetVisible);
                intent.bool_value = !plugin.visible;
                SubmitIntent(std::move(intent));
            }
        }
        if (row_hovered && !toggle_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            plugin.IsRunning() && plugin.visibility_control) {
            compact_plugin_detail_pending_id_.clear();
            Intent intent = model_.NewIntent(
                cabbird::PlatformUiIntentKind::SetPluginVisible, plugin.id, Mutation::SetVisible);
            intent.bool_value = !plugin.visible;
            SubmitIntent(std::move(intent));
        }
        if (row_right_clicked) {
            model_.State().selected_plugin_id = plugin.id;
            compact_plugin_detail_pending_id_.clear();
            ImGui::OpenPopup("Platform plugin context");
        }

        const auto& theme = cabbird::CabbirdUiTheme();
        ImDrawList* const draw_list = ImGui::GetWindowDrawList();
        if (selected) {
            cabbird::glass::DrawThumb(draw_list, Offset(origin, 0.0f, Scaled(2.0f)),
                Offset(origin, width, height - Scaled(2.0f)), Scaled(12.0f),
                PlatformUiScale(), false);
        }
        const float icon_size = Scaled(IsCompact() ? 36.0f : 40.0f);
        const ImVec2 icon_position = Offset(
            origin, Scaled(11.0f), (height - icon_size) * 0.5f);
        DrawGenericPluginIcon(icon_position, icon_size);
        const float text_x = icon_position.x + icon_size + Scaled(8.0f);
        const float status_x = toggle_x - Scaled(27.0f);
        const float text_width = std::max(
            Scaled(20.0f), origin.x + status_x - text_x - Scaled(8.0f));
        const bool favorite = IsPluginFavorite(plugin);
        const float star_size = ImGui::GetFontSize() * 0.85f;
        const ImVec2 star_extent = ImGui::GetFont()->CalcTextSizeA(
            star_size, FLT_MAX, 0.0f, ShellGlyphText(ShellGlyph::StarFilled));
        const std::string name = Ellipsize(plugin.name.empty() ? plugin.id : plugin.name,
            std::max(Scaled(20.0f), text_width - (favorite ? star_extent.x + Scaled(4.0f) : 0.0f)));
        const ImVec2 name_position(text_x, origin.y + Scaled(12.0f));
        draw_list->AddText(name_position,
            ImGui::ColorConvertFloat4ToU32(ThemeColor(theme.text)), name.c_str());
        if (favorite) {
            const ImVec2 star_min(name_position.x + ImGui::CalcTextSize(name.c_str()).x + Scaled(4.0f), name_position.y);
            DrawGlyphCentred(ShellGlyph::StarFilled, star_min,
                Offset(star_min, star_extent.x, ImGui::GetTextLineHeight()),
                ImGui::GetColorU32(ThemeColor(theme.accent)), star_size);
        }
        if (!plugin.author.empty() || !plugin.version.empty()) {
            std::string metadata;
            if (!plugin.author.empty()) metadata = plugin.author;
            if (!plugin.version.empty()) {
                if (!metadata.empty()) metadata += "  ";
                metadata += "v" + plugin.version;
            }
            metadata = Ellipsize(metadata, text_width);
            draw_list->AddText(ImVec2(text_x, origin.y + Scaled(33.0f)),
                ImGui::ColorConvertFloat4ToU32(ThemeColor(theme.text_muted)), metadata.c_str());
        }
        draw_list->AddText(Offset(origin, status_x,
                (height - ImGui::GetTextLineHeight()) * 0.5f),
            ImGui::ColorConvertFloat4ToU32(PluginStateColor(plugin.UiState())),
            ShellGlyphText(StateGlyph(plugin.UiState())));

        const bool toggle_allowed = PluginToggleAllowed(plugin);
        ImGui::SetCursorScreenPos(toggle_position);
        ImGui::BeginDisabled(!toggle_allowed);
        const bool toggle_clicked =
            ImGui::InvisibleButton("enable-toggle", toggle_size);
        const bool toggle_held = ImGui::IsItemActive();
        ImGui::EndDisabled();
        const bool current_toggle_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (toggle_clicked && toggle_allowed) {
            model_.State().selected_plugin_id = plugin.id;
            SubmitPluginToggle(plugin);
        }
        const bool pending = HasPendingMutation(plugin.id, Mutation::None);
        const ImGuiID switch_id = ImGui::GetID("enable-switch");
        const float on = cabbird::glass::Animate(switch_id, plugin.enabled ? 1.0f : 0.0f, 15.0f);
        const float press = cabbird::glass::Animate(switch_id ^ 0x9e3779b9U,
            toggle_held ? 1.0f : 0.0f, 22.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
            ImGui::GetStyle().Alpha * (toggle_allowed || pending ? 1.0f : 0.45f));
        cabbird::glass::DrawSwitch(draw_list, toggle_position,
            Offset(toggle_position, toggle_size.x, toggle_size.y), on,
            press, current_toggle_hovered && toggle_allowed, PlatformUiScale());
        ImGui::PopStyleVar();
        if (current_toggle_hovered && !toggle_allowed) {
            const std::string reason = PluginToggleDisabledReason(plugin);
            ImGui::SetTooltip("%s", reason.c_str());
        } else if (row_hovered && !toggle_hovered) {
            ImGui::SetTooltip("%s", DisplayPluginState(plugin.UiState()));
        }
        ImGui::SetCursorScreenPos(Offset(origin, 0.0f, height));
        DrawPluginContextMenu(plugin);
        ImGui::PopID();
    }

    void DrawPluginContextMenu(const cabbird::InstalledPluginView& plugin) {
        if (!ImGui::BeginPopup("Platform plugin context")) return;
        const bool frozen = HasPendingMutation(plugin.id, Mutation::None);
        const auto* repository_plugin = RepositoryPlugin(plugin.id);
        const auto* repository_operation = RepositoryOperation(plugin.id);
        const bool repository_removed = repository_operation != nullptr &&
            repository_operation->kind == cabbird::RepositoryOperationKind::Uninstall &&
            repository_operation->state == cabbird::RepositoryOperationState::Succeeded;
        const bool repository_pending = repository_operation != nullptr &&
            repository_operation->state != cabbird::RepositoryOperationState::Succeeded &&
            repository_operation->state != cabbird::RepositoryOperationState::Failed &&
            repository_operation->state != cabbird::RepositoryOperationState::Cancelled;
        const auto submit_visibility = [this, &plugin] {
            Intent intent = model_.NewIntent(
                cabbird::PlatformUiIntentKind::SetPluginVisible, plugin.id, Mutation::SetVisible);
            intent.bool_value = !plugin.visible;
            SubmitIntent(std::move(intent));
        };
        if (plugin.UiState() == cabbird::PlatformUiPluginState::Active &&
            plugin.visibility_control) {
            if (ImGui::MenuItem(Text(plugin.visible ? cabbird::MessageId::CommonHide
                                                   : cabbird::MessageId::CommonOpen),
                    nullptr, false, !frozen)) {
                submit_visibility();
            }
            if (frozen) DrawTooltip(Text(cabbird::MessageId::OperationInProgress));
        }

        const bool toggle_allowed = PluginToggleAllowed(plugin);
        if (ImGui::MenuItem(Text(plugin.enabled ? cabbird::MessageId::CommonDisable
                                               : cabbird::MessageId::CommonEnable),
                nullptr, false, toggle_allowed)) {
            SubmitPluginToggle(plugin);
        }
        if (!toggle_allowed) {
            const std::string reason = PluginToggleDisabledReason(plugin);
            DrawTooltip(reason.c_str());
        }

        if (plugin.has_runtime_view) {
            if (ImGui::MenuItem(Text(cabbird::MessageId::CommonReload),
                    nullptr, false, !frozen)) {
                SubmitIntent(model_.NewIntent(
                    cabbird::PlatformUiIntentKind::ReloadPlugin, plugin.id, Mutation::Reload));
            }
            if (frozen) DrawTooltip(Text(cabbird::MessageId::OperationInProgress));
        }

        ImGui::Separator();
        if (ImGui::MenuItem(Text(IsPluginFavorite(plugin) ? cabbird::MessageId::PluginsRemoveFavorite
                                                        : cabbird::MessageId::PluginsAddFavorite))) {
            TogglePluginFavorite(plugin);
        }

        if (repository_plugin != nullptr && !repository_removed) {
            ImGui::Separator();
            if (ImGui::MenuItem(Text(cabbird::MessageId::CommonUninstall),
                    nullptr, false, !repository_pending)) {
                RequestRepositoryUninstall(plugin.id,
                    plugin.name.empty() ? plugin.id : plugin.name);
            }
            if (repository_pending) {
                DrawTooltip(Text(cabbird::MessageId::OperationInProgress));
            }
        }

        if (developer_mode_) {
            ImGui::Separator();
            if (ImGui::MenuItem(Text(cabbird::MessageId::PluginsOpenDeveloperDetails))) {
                OpenDeveloperPlugin(plugin.id);
            }
            if (ImGui::MenuItem(Text(cabbird::MessageId::PluginsViewLogs))) {
                OpenPluginLogs(plugin.id);
            }
            if (ImGui::MenuItem(Text(cabbird::MessageId::PluginsCopyId))) {
                ImGui::SetClipboardText(plugin.id.c_str());
                status_ = Text(cabbird::MessageId::PluginsIdCopied);
                status_failure_ = false;
            }
        }
        ImGui::EndPopup();
    }

    [[nodiscard]] bool ShowPluginReloadAction(const cabbird::InstalledPluginView& plugin) const {
        const auto state = plugin.UiState();
        const bool pending = HasPendingMutation(plugin.id, Mutation::None);
        return (plugin.has_runtime_view || pending) &&
            (state != cabbird::PlatformUiPluginState::Stopping || pending) &&
            state != cabbird::PlatformUiPluginState::Faulted;
    }

    [[nodiscard]] float PluginPublicActionsWidth(
        const cabbird::InstalledPluginView& plugin,
        const bool compact_secondary) const {
        const auto state = plugin.UiState();
        const bool can_open = state == cabbird::PlatformUiPluginState::Active &&
            plugin.visibility_control;
        const char* primary_label = can_open
            ? Text(plugin.visible ? cabbird::MessageId::CommonHide
                                  : cabbird::MessageId::CommonOpen)
            : state == cabbird::PlatformUiPluginState::Disabled
                ? Text(cabbird::MessageId::CommonEnable)
            : state == cabbird::PlatformUiPluginState::Faulted
                ? Text(cabbird::MessageId::CommonReload)
            : state == cabbird::PlatformUiPluginState::DependencyBlocked
                ? Text(cabbird::MessageId::PluginsViewStatus)
                : Text(cabbird::MessageId::CommonDisable);
        float width = Scaled(36.0f) + std::max(
            Scaled(74.0f), ImGui::CalcTextSize(primary_label).x + Scaled(38.0f));
        const bool show_reload = ShowPluginReloadAction(plugin);
        if (show_reload) width += Scaled(compact_secondary ? 36.0f : 82.0f);
        const auto* repository_plugin = RepositoryPlugin(plugin.id);
        const auto* repository_operation = RepositoryOperation(plugin.id);
        const bool repository_removed = repository_operation != nullptr &&
            repository_operation->kind == cabbird::RepositoryOperationKind::Uninstall &&
            repository_operation->state == cabbird::RepositoryOperationState::Succeeded;
        if (developer_mode_ || (repository_plugin != nullptr && !repository_removed)) {
            width += Scaled(34.0f);
        }
        return width;
    }

    void DrawPluginPublicDetail(const cabbird::InstalledPluginView& plugin) {
        ImGui::PushID(plugin.id.c_str());
        const float available_width = ImGui::GetContentRegionAvail().x;
        const float icon_size = Scaled(IsCompact() ? 44.0f : 56.0f);
        const float inset = Scaled(IsCompact() ? 12.0f : 16.0f);
        const float title_x = inset + icon_size + Scaled(12.0f);
        const float full_actions_width = PluginPublicActionsWidth(plugin, false);
        const float minimum_identity_width = PluginStateBadgeWidth(plugin.UiState()) +
            Scaled(IsCompact() ? 64.0f : 88.0f);
        const bool compact_actions = available_width < title_x + full_actions_width +
            minimum_identity_width + inset;
        const float actions_width = compact_actions
            ? PluginPublicActionsWidth(plugin, true) : full_actions_width;
        const bool stack_actions = available_width < title_x + actions_width +
            minimum_identity_width + inset;
        const float height = Scaled(stack_actions
            ? (IsCompact() ? 118.0f : 132.0f)
            : (IsCompact() ? 84.0f : 96.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
            ThemeColor(cabbird::CabbirdUiTheme().panel_background));
        ImGui::BeginChild("PlatformPluginDetailHeader", ImVec2(0.0f, height), false,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 origin = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        DrawGenericPluginIcon(Offset(origin, inset,
            stack_actions ? inset : (height - icon_size) * 0.5f), icon_size);
        const float identity_width = std::max(
            Scaled(80.0f), size.x - title_x -
                (stack_actions ? inset : actions_width + inset + Scaled(12.0f)));
        const float name_width = std::max(Scaled(20.0f),
            identity_width - PluginStateBadgeWidth(plugin.UiState()) - Scaled(8.0f));
        const std::string name = Ellipsize(
            plugin.name.empty() ? plugin.id : plugin.name, name_width / 1.15f);
        ImGui::SetCursorScreenPos(Offset(
            origin, title_x, Scaled(IsCompact() ? 17.0f : 20.0f)));
        ImGui::SetWindowFontScale(1.15f);
        ImGui::TextUnformatted(name.c_str());
        ImGui::SetWindowFontScale(1.0f);
        // Centring the pill on the name line box keeps its text on the same
        // baseline as the label; hanging it from the bottom edge left it high.
        const ImVec2 name_rect_min = ImGui::GetItemRectMin();
        const ImVec2 name_rect_max = ImGui::GetItemRectMax();
        DrawPluginStateBadge(ImVec2(name_rect_max.x + Scaled(8.0f),
            (name_rect_min.y + name_rect_max.y) * 0.5f -
                PluginStateBadgeHeight() * 0.5f),
            plugin.UiState());
        if (!plugin.author.empty() || !plugin.version.empty()) {
            std::string metadata = plugin.author;
            if (!plugin.version.empty()) {
                if (!metadata.empty()) metadata += "  ";
                metadata += "v" + plugin.version;
            }
            ImGui::SetCursorScreenPos(Offset(
                origin, title_x, Scaled(IsCompact() ? 43.0f : 49.0f)));
            ImGui::TextDisabled("%s", metadata.c_str());
        }
        const ImVec2 actions_origin = stack_actions
            ? Offset(origin, title_x, Scaled(IsCompact() ? 76.0f : 86.0f))
            : Offset(origin, size.x - actions_width - inset,
                Scaled(IsCompact() ? 16.0f : 33.0f));
        DrawPluginPublicActions(plugin, actions_origin,
            stack_actions ? size.x - title_x - inset : actions_width,
            compact_actions && !stack_actions);
        ImGui::GetWindowDrawList()->AddLine(
            Offset(origin, inset, size.y - 1.0f), Offset(origin, size.x - inset, size.y - 1.0f),
            ImGui::GetColorU32(InkColor(0.08f)));
        ImGui::EndChild();
        ImGui::PopStyleColor();

        const float body_padding = Scaled(IsCompact() ? 12.0f : 16.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(body_padding, body_padding));
        ImGui::BeginChild("PlatformPluginDetailBody", ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
        ImGui::TextUnformatted(Text(cabbird::MessageId::PluginsAbout));
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
            std::min(Scaled(620.0f), ImGui::GetContentRegionAvail().x));
        ImGui::TextDisabled("%s", plugin.description.empty()
            ? Text(cabbird::MessageId::PluginsNoIntroduction)
            : plugin.description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, FloatingBarClearance()));
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopID();
    }

    void DrawPluginPublicActions(const cabbird::InstalledPluginView& plugin,
        const ImVec2 origin, const float available, const bool compact_secondary) {
        const bool frozen = HasPendingMutation(plugin.id, Mutation::None);
        ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha,
            frozen ? 1.0f : ImGui::GetStyle().DisabledAlpha);
        const auto* repository_plugin = RepositoryPlugin(plugin.id);
        const auto* repository_operation = RepositoryOperation(plugin.id);
        const bool repository_removed = repository_operation != nullptr &&
            repository_operation->kind == cabbird::RepositoryOperationKind::Uninstall &&
            repository_operation->state == cabbird::RepositoryOperationState::Succeeded;
        const bool repository_pending = repository_operation != nullptr &&
            repository_operation->state != cabbird::RepositoryOperationState::Succeeded &&
            repository_operation->state != cabbird::RepositoryOperationState::Failed &&
            repository_operation->state != cabbird::RepositoryOperationState::Cancelled;
        const auto state = plugin.UiState();
        const auto submit_visibility = [this, &plugin] {
            Intent intent = model_.NewIntent(
                cabbird::PlatformUiIntentKind::SetPluginVisible, plugin.id, Mutation::SetVisible);
            intent.bool_value = !plugin.visible;
            SubmitIntent(std::move(intent));
        };
        const auto submit_reload = [this, &plugin] {
            SubmitIntent(model_.NewIntent(
                cabbird::PlatformUiIntentKind::ReloadPlugin, plugin.id, Mutation::Reload));
        };
        const bool can_open = state == cabbird::PlatformUiPluginState::Active && plugin.visibility_control;
        const char* primary_label = can_open
            ? Text(plugin.visible ? cabbird::MessageId::CommonHide
                                  : cabbird::MessageId::CommonOpen)
            : state == cabbird::PlatformUiPluginState::Disabled
                ? Text(cabbird::MessageId::CommonEnable)
            : state == cabbird::PlatformUiPluginState::Faulted
                ? Text(cabbird::MessageId::CommonReload)
            : state == cabbird::PlatformUiPluginState::DependencyBlocked
                ? Text(cabbird::MessageId::PluginsViewStatus)
                : Text(cabbird::MessageId::CommonDisable);
        const ShellGlyph primary_glyph = can_open ? (plugin.visible ? ShellGlyph::EyeOff : ShellGlyph::Eye)
            : state == cabbird::PlatformUiPluginState::Disabled ? ShellGlyph::Play
            : state == cabbird::PlatformUiPluginState::Faulted ? ShellGlyph::Refresh
            : state == cabbird::PlatformUiPluginState::DependencyBlocked ? ShellGlyph::Info
            : ShellGlyph::Stop;
        const bool primary_allowed = can_open ? !frozen
            : state == cabbird::PlatformUiPluginState::Faulted
                ? !frozen && plugin.has_runtime_view
            : state == cabbird::PlatformUiPluginState::DependencyBlocked
                ? true
                : PluginToggleAllowed(plugin);
        const float primary_width = std::max(
            Scaled(74.0f), ImGui::CalcTextSize(primary_label).x + Scaled(38.0f));
        const float favorite_block = Scaled(36.0f);
        ImGui::SetCursorScreenPos(origin);
        if (DrawFavoriteToggle("detail-favorite", IsPluginFavorite(plugin))) TogglePluginFavorite(plugin);
        ImGui::SetCursorScreenPos(Offset(origin, favorite_block, 0.0f));
        if (DrawShellCommandButton("detail-primary", primary_label, primary_glyph, true,
                primary_allowed, ImVec2(primary_width, Scaled(30.0f)))) {
            if (can_open) submit_visibility();
            else if (state == cabbird::PlatformUiPluginState::Faulted) submit_reload();
            else if (state == cabbird::PlatformUiPluginState::DependencyBlocked) OpenDeveloperPlugin(plugin.id);
            else if (PluginToggleAllowed(plugin)) {
                SubmitPluginToggle(plugin);
            }
        }
        if (!primary_allowed) DrawTooltip(
            frozen ? Text(cabbird::MessageId::OperationInProgress)
                   : PluginToggleDisabledReason(plugin).c_str());
        const bool show_reload = ShowPluginReloadAction(plugin);
        const float reload_width = Scaled(compact_secondary ? 30.0f : 76.0f);
        if (show_reload && favorite_block + primary_width + Scaled(6.0f) + reload_width <= available) {
            ImGui::SameLine(0.0f, Scaled(6.0f));
            const bool reload_pressed = compact_secondary
                ? DrawShellIconButton("detail-reload", ShellGlyph::Refresh,
                    Text(cabbird::MessageId::CommonReload), !frozen)
                : DrawShellCommandButton("detail-reload",
                    Text(cabbird::MessageId::CommonReload), ShellGlyph::Refresh, false,
                    !frozen, ImVec2(reload_width, Scaled(30.0f)));
            if (reload_pressed) {
                submit_reload();
            }
        }
        const bool show_uninstall = repository_plugin != nullptr && !repository_removed;
        const bool show_more = developer_mode_ || show_uninstall;
        const float used_width = favorite_block + primary_width +
            (show_reload ? Scaled(6.0f) + reload_width : 0.0f);
        if (show_more && used_width + Scaled(34.0f) <= available) {
            ImGui::SameLine(0.0f, Scaled(4.0f));
            if (DrawShellIconButton("detail-menu", ShellGlyph::More,
                    Text(cabbird::MessageId::PluginsMoreActions))) {
                ImGui::OpenPopup("Platform plugin actions");
            }
        }
        if (show_more) {
            if (ImGui::BeginPopup("Platform plugin actions")) {
                if (show_uninstall) {
                    if (ImGui::MenuItem(Text(cabbird::MessageId::CommonUninstall),
                            nullptr, false, !repository_pending)) {
                        RequestRepositoryUninstall(plugin.id,
                            plugin.name.empty() ? plugin.id : plugin.name);
                    }
                    if (repository_pending) {
                        DrawTooltip(Text(cabbird::MessageId::OperationInProgress));
                    }
                }
                if (developer_mode_) {
                    if (show_uninstall) ImGui::Separator();
                    if (ImGui::MenuItem(Text(cabbird::MessageId::PluginsOpenDeveloperDetails))) {
                        OpenDeveloperPlugin(plugin.id);
                    }
                    if (ImGui::MenuItem(Text(cabbird::MessageId::PluginsViewLogs))) {
                        OpenPluginLogs(plugin.id);
                    }
                    if (ImGui::MenuItem(Text(cabbird::MessageId::PluginsCopyId))) {
                        ImGui::SetClipboardText(plugin.id.c_str());
                        status_ = Text(cabbird::MessageId::PluginsIdCopied);
                        status_failure_ = false;
                    }
                }
                ImGui::EndPopup();
            }
        }
        if (frozen) DrawTooltip(Text(cabbird::MessageId::OperationInProgress));
        ImGui::PopStyleVar();
    }

    void OpenDeveloperPlugin(const std::string_view plugin_id) {
        model_.State().selected_plugin_id = std::string(plugin_id);
        model_.State().route = Route::Diagnostics;
        model_.State().diagnostics_tab = DiagnosticTab::Developer;
        developer_panel_ = DeveloperPanel::Plugins;
        developer_plugin_tab_ = DeveloperPluginTab::Overview;
        compact_plugin_detail_ = false;
        compact_plugin_detail_pending_id_.clear();
    }

    void DrawUnityRuntimeDiagnostics() {
        const auto& compatibility = model_.Snapshot().unity_compatibility;
        ImGui::TextColored(compatibility.level == cabbird::UnityCompatibilityLevel::Supported
                ? SuccessColor() : WarningColor(), "%s", UnityLevelName(compatibility.level));
        if (!compatibility.reason.empty()) ImGui::TextWrapped("%s", compatibility.reason.c_str());
        ImGui::Spacing();
        DrawUnityFeatureMatrix();
        ImGui::Spacing();
        DrawUnityBuildPanel();
    }

    void DrawUnityFeatureMatrix() {
        const auto& compatibility = model_.Snapshot().unity_compatibility;
        ImGui::TextUnformatted(Text(cabbird::MessageId::PluginsFeatureMatrix));
        if (compatibility.features.empty()) {
            ImGui::TextDisabled("%s", Text(cabbird::MessageId::PluginsFeatureProviderMissing));
        } else {
            const int columns = IsCompact() ? 3 : 4;
            if (ImGui::BeginTable("PlatformUnityFeatures", columns,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonFeature),
                    ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonAvailability),
                    ImGuiTableColumnFlags_WidthFixed, 96.0f);
                ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonValidation),
                    ImGuiTableColumnFlags_WidthFixed, 88.0f);
                if (!IsCompact()) {
                    ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonReason),
                        ImGuiTableColumnFlags_WidthStretch, 1.2f);
                }
                ImGui::TableHeadersRow();
                for (const auto& feature : compatibility.features) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(feature.id.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextColored(feature.available ? SuccessColor() : WarningColor(),
                        "%s", Text(feature.available ? cabbird::MessageId::PluginsTabAvailable
                                                     : cabbird::MessageId::CommonUnavailable));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(Text(feature.validated
                        ? cabbird::MessageId::PluginsValidated
                        : cabbird::MessageId::PluginsMissing));
                    if (!IsCompact()) {
                        ImGui::TableNextColumn();
                        ImGui::TextWrapped("%s", feature.reason.empty() ? "-" : feature.reason.c_str());
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::Spacing();
        ImGui::TextUnformatted(Text(cabbird::MessageId::PluginsPublishedServices));
        if (compatibility.services.empty()) {
            ImGui::TextDisabled("%s", Text(cabbird::MessageId::PluginsNoPublishedServices));
        } else {
            for (const auto& service : compatibility.services) {
                ImGui::Separator();
                ImGui::TextUnformatted(service.id.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("v%u", service.version);
            }
        }
    }

    void DrawUnityBuildPanel() {
        const auto& compatibility = model_.Snapshot().unity_compatibility;
        ImGui::TextUnformatted(Text(cabbird::MessageId::PluginsBuildDetails));
        if (ImGui::BeginTable("PlatformUnityBuild", 2,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            const auto row = [this](const char* label, const std::string& value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", label);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", value.empty()
                    ? Text(cabbird::MessageId::CommonUnavailable) : value.c_str());
            };
            row(Text(cabbird::MessageId::PluginsBuildId), compatibility.build_id);
            row(Text(cabbird::MessageId::PluginsProfileSource), compatibility.profile_source);
            row(Text(cabbird::MessageId::PluginsProfileHash), compatibility.profile_hash);
            row(Text(cabbird::MessageId::PluginsResolverReason), compatibility.reason);
            ImGui::EndTable();
        }
    }

    void DrawDiagnosticsShell() {
        if (!developer_mode_ && model_.State().diagnostics_tab == DiagnosticTab::Developer) {
            model_.State().diagnostics_tab = DiagnosticTab::Overview;
        }
        DrawShellPageHeader(Text(cabbird::MessageId::ShellRouteDiagnostics), {},
            [](const ImVec2&, const ImVec2&) {});
        ImGui::BeginChild("PlatformDiagnosticsRoute", ImVec2(0.0f, 0.0f), false,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawDiagnosticTabs();
        switch (model_.State().diagnostics_tab) {
        case DiagnosticTab::Overview: DrawDiagnosticsOverviewShell(); break;
        case DiagnosticTab::PluginPerformance: DrawPerformanceShell(); break;
        case DiagnosticTab::Logs: DrawLogsShell(); break;
        case DiagnosticTab::Developer: DrawDeveloperShell(); break;
        }
        ImGui::EndChild();
    }

    void DrawDiagnosticTabs() {
        ImGui::BeginChild("PlatformDiagnosticTabs", ImVec2(0.0f, Scaled(54.0f)), false,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const std::array<const char*, 4> labels{
            DiagnosticTabLabel(DiagnosticTab::Overview),
            DiagnosticTabLabel(DiagnosticTab::PluginPerformance),
            DiagnosticTabLabel(DiagnosticTab::Logs),
            DiagnosticTabLabel(DiagnosticTab::Developer)};
        const std::span<const char* const> shown(
            labels.data(), developer_mode_ ? labels.size() : labels.size() - 1);
        ImGui::SetCursorPos(ImVec2(Scaled(IsCompact() ? 4.0f : 8.0f), Scaled(6.0f)));
        int selected = static_cast<int>(model_.State().diagnostics_tab);
        cabbird::glass::SegmentedOptions options;
        options.height = 40.0f;
        options.scale = PlatformUiScale();
        options.padding_x = IsCompact() ? 10.0f : 16.0f;
        if (cabbird::glass::SegmentedControl("diagnostic-tabs", shown, selected, options)) {
            model_.State().diagnostics_tab = static_cast<DiagnosticTab>(selected);
        }
        ImGui::EndChild();
    }

    void DrawDiagnosticsOverviewShell() {
        const auto& snapshot = model_.Snapshot();
        BeginShellBodyChild("PlatformDiagnosticsOverview", {}, true);
        const int summary_columns = IsCompact() ? 2 : 3;
        if (ImGui::BeginTable("PlatformDiagnosticsSummary", summary_columns,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            const auto summary = [](const char* label, const std::string& value) {
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", label);
                ImGui::TextUnformatted(value.c_str());
            };
            summary(Text(cabbird::MessageId::DiagnosticsRuntime),
                snapshot.diagnostics.runtime_version.empty()
                    ? std::string{Text(cabbird::MessageId::CommonUnavailable)}
                    : snapshot.diagnostics.runtime_version);
            summary(Text(cabbird::MessageId::DiagnosticsProcess),
                "PID " + std::to_string(snapshot.diagnostics.process_id));
            summary(Text(cabbird::MessageId::DiagnosticsInstalledRunningIssues),
                std::to_string(snapshot.runtime_summary.installed) +
                " / " + std::to_string(snapshot.runtime_summary.running) +
                " / " + std::to_string(snapshot.runtime_summary.issues));
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (ImGui::CollapsingHeader(Text(cabbird::MessageId::DiagnosticsGameRuntime))) {
            DrawUnityRuntimeDiagnostics();
        }
        ImGui::Spacing();
        ImGui::TextUnformatted(Text(cabbird::MessageId::DiagnosticsRecentFaults));
        ImGui::Separator();
        if (snapshot.diagnostics.recent_faults.empty()) {
            ImGui::TextDisabled("%s", Text(cabbird::MessageId::DiagnosticsNoRecentFaults));
        } else {
            for (const auto& fault : snapshot.diagnostics.recent_faults) {
                ImGui::TextColored(ErrorColor(), "!");
                ImGui::SameLine();
                ImGui::TextWrapped("%s", fault.c_str());
            }
        }
        ImGui::Spacing();
        ImGui::TextUnformatted(Text(cabbird::MessageId::DiagnosticsRecentOperations));
        ImGui::Separator();
        if (const auto* operation = PresentedOperation()) {
            const std::string label = OperationDisplayLabel(*operation);
            ImGui::TextColored(OperationStateColor(operation->state), "%s", label.c_str());
            ImGui::SameLine();
            const std::string view_details = StableLabel(
                cabbird::MessageId::DiagnosticsViewDetails, "diagnostics-view-operation");
            if (ImGui::Button(view_details.c_str())) RequestOperationDetailsPopup();
        } else {
            ImGui::TextDisabled("%s", Text(cabbird::MessageId::DiagnosticsNoCurrentOperations));
        }
        EndShellBodyChild();
    }

    void DrawPerformanceShell() {
        ImGui::BeginChild("PlatformPerformanceToolbar",
            ImVec2(0.0f, Scaled(IsCompact() ? 82.0f : 50.0f)), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        SetAvailableItemWidth(IsCompact() ? 40.0f : 0.0f, 260.0f);
        ImGui::InputTextWithHint("##performance-search",
            Text(cabbird::MessageId::PluginsSearchHint), performance_search_.data(),
            performance_search_.size());
        if (IsCompact()) ImGui::NewLine();
        else ImGui::SameLine();
        ImGui::SetNextItemWidth(Scaled(88.0f));
        const char* callbacks[] = {
            Text(cabbird::MessageId::PluginsFilterAll),
            Text(cabbird::MessageId::CommonUpdate),
            Text(cabbird::MessageId::CommonDraw)};
        cabbird::glass::Combo("##performance-callback", &performance_callback_filter_, callbacks, IM_ARRAYSIZE(callbacks));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Scaled(90.0f));
        const char* states[] = {
            Text(cabbird::MessageId::PluginsFilterAll),
            Text(cabbird::MessageId::PluginStateRunning),
            Text(cabbird::MessageId::PluginsFilterIssues),
            Text(cabbird::MessageId::PluginStateDisabled)};
        cabbird::glass::Combo("##performance-state", &performance_state_filter_, states, IM_ARRAYSIZE(states));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Scaled(106.0f));
        const char* sorts[] = {"p95", "p99", Text(cabbird::MessageId::CommonFaults),
            Text(cabbird::MessageId::DiagnosticsSlowCalls),
            Text(cabbird::MessageId::CommonName)};
        cabbird::glass::Combo("##performance-sort", &performance_sort_, sorts, IM_ARRAYSIZE(sorts));
        ImGui::EndChild();

        std::vector<PerformanceRow> rows;
        const std::string_view query(performance_search_.data());
        for (const auto& plugin : model_.Snapshot().installed_plugins) {
            if (!developer_mode_ && plugin.audience == cabbird::PluginAudience::Developer) continue;
            if (!query.empty() && !cabbird::MatchesPluginSearch(plugin, query)) continue;
            if (performance_state_filter_ == 1 && !plugin.IsRunning()) continue;
            if (performance_state_filter_ == 2 && !plugin.HasIssue()) continue;
            if (performance_state_filter_ == 3 && !plugin.IsDisabled()) continue;
            if (performance_callback_filter_ != 2) {
                rows.push_back({&plugin, &plugin.update_metrics, true});
            }
            if (performance_callback_filter_ != 1) {
                rows.push_back({&plugin, &plugin.draw_metrics, false});
            }
        }
        const auto metric_sort_value = [this](const PerformanceRow& row) {
            switch (performance_sort_) {
            case 0: return row.metrics->p95_milliseconds;
            case 1: return row.metrics->p99_milliseconds;
            case 2: return static_cast<double>(row.metrics->faults);
            case 3: return static_cast<double>(row.metrics->slow_calls);
            default: return 0.0;
            }
        };
        std::sort(rows.begin(), rows.end(), [&](const PerformanceRow& left, const PerformanceRow& right) {
            if (performance_sort_ == 4) {
                const std::string& left_name = left.plugin->name.empty() ? left.plugin->id : left.plugin->name;
                const std::string& right_name = right.plugin->name.empty() ? right.plugin->id : right.plugin->name;
                return left_name == right_name ? left.update > right.update : left_name < right_name;
            }
            const double left_value = metric_sort_value(left);
            const double right_value = metric_sort_value(right);
            if (left_value != right_value) return left_value > right_value;
            return left.plugin->id == right.plugin->id
                ? left.update > right.update : left.plugin->id < right.plugin->id;
        });

        if (layout_mode_ == LayoutMode::Wide) {
            const float table_width = std::max(420.0f, ImGui::GetContentRegionAvail().x * 0.62f);
            BeginShellBodyChild("PlatformPerformanceTablePane", ImVec2(table_width, 0.0f), true,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            DrawPerformanceTable(rows);
            EndShellBodyChild();
            ImGui::SameLine(0.0f, 0.0f);
            BeginShellBodyChild("PlatformPerformanceDetail", {}, true);
            DrawSelectedPerformanceDetail();
            EndShellBodyChild();
            return;
        }

        const float detail_height = IsCompact() ? 0.0f : 168.0f;
        BeginShellBodyChild("PlatformPerformanceTablePane", ImVec2(0.0f,
            detail_height == 0.0f ? 0.0f : -detail_height - 4.0f), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawPerformanceTable(rows);
        EndShellBodyChild();
        if (!IsCompact()) {
            BeginShellBodyChild("PlatformPerformanceDetail", ImVec2(0.0f, detail_height), true);
            DrawSelectedPerformanceDetail();
            EndShellBodyChild();
        }
    }

    void DrawPerformanceTable(const std::vector<PerformanceRow>& rows) {
        const int columns = IsCompact() ? 5 : 9;
        if (!ImGui::BeginTable("PlatformPerformanceTable", columns,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_SizingStretchProp, ShellScrollTableSize())) {
            return;
        }
        ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonPlugin),
            ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonCallback),
            ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonCalls),
            ImGuiTableColumnFlags_WidthFixed, 64.0f);
        if (IsCompact()) {
            ImGui::TableSetupColumn("p95", ImGuiTableColumnFlags_WidthFixed, 66.0f);
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonState),
                ImGuiTableColumnFlags_WidthFixed, 72.0f);
        } else {
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonFaults),
                ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonSlow),
                ImGuiTableColumnFlags_WidthFixed, 58.0f);
            ImGui::TableSetupColumn("p50", ImGuiTableColumnFlags_WidthFixed, 62.0f);
            ImGui::TableSetupColumn("p95", ImGuiTableColumnFlags_WidthFixed, 62.0f);
            ImGui::TableSetupColumn("p99", ImGuiTableColumnFlags_WidthFixed, 62.0f);
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonState),
                ImGuiTableColumnFlags_WidthFixed, 82.0f);
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                const PerformanceRow& row = rows[static_cast<std::size_t>(index)];
                const CallbackMetricsView& metrics = *row.metrics;
                ImGui::PushID((row.plugin->id + (row.update ? "-update" : "-draw")).c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string& name = row.plugin->name.empty() ? row.plugin->id : row.plugin->name;
                if (ImGui::Selectable(name.c_str(), performance_selected_plugin_id_ == row.plugin->id,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    performance_selected_plugin_id_ = row.plugin->id;
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(Text(row.update
                    ? cabbird::MessageId::CommonUpdate : cabbird::MessageId::CommonDraw));
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(metrics.calls));
                if (IsCompact()) {
                    ImGui::TableNextColumn();
                    if (metrics.calls == 0) {
                        ImGui::TextDisabled("%s", Text(cabbird::MessageId::CommonNone));
                    }
                    else ImGui::Text("%.3f", metrics.p95_milliseconds);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(PluginStateColor(row.plugin->UiState()), "%s",
                        DisplayPluginState(row.plugin->UiState()));
                } else {
                    ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(metrics.faults));
                    ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(metrics.slow_calls));
                    ImGui::TableNextColumn();
                    if (metrics.calls == 0) ImGui::TextDisabled(
                        "%s", Text(cabbird::MessageId::CommonNoSamples));
                    else ImGui::Text("%.3f", metrics.p50_milliseconds);
                    ImGui::TableNextColumn();
                    if (metrics.calls == 0) ImGui::TextDisabled(
                        "%s", Text(cabbird::MessageId::CommonNoSamples));
                    else ImGui::Text("%.3f", metrics.p95_milliseconds);
                    ImGui::TableNextColumn();
                    if (metrics.calls == 0) ImGui::TextDisabled(
                        "%s", Text(cabbird::MessageId::CommonNoSamples));
                    else ImGui::Text("%.3f", metrics.p99_milliseconds);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(PluginStateColor(row.plugin->UiState()), "%s",
                        DisplayPluginState(row.plugin->UiState()));
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    void DrawSelectedPerformanceDetail() {
        const auto* plugin = model_.Snapshot().FindPlugin(performance_selected_plugin_id_);
        if (plugin == nullptr) {
            ImGui::TextUnformatted(Text(cabbird::MessageId::DiagnosticsSelectedCallback));
            ImGui::TextDisabled("%s", Text(cabbird::MessageId::DiagnosticsSelectCallback));
            return;
        }
        ImGui::Text("%s", plugin->name.empty() ? plugin->id.c_str() : plugin->name.c_str());
        const std::string generation = std::to_string(plugin->generation);
        const std::array<std::string_view, 1> arguments{generation};
        const std::string generation_text =
            Format(cabbird::MessageId::DiagnosticsGeneration, arguments);
        ImGui::TextDisabled("%s", generation_text.c_str());
        ImGui::Separator();
        ImGui::TextDisabled("%s", Text(cabbird::MessageId::DiagnosticsUpdateBudget));
        ImGui::TextDisabled("%s", Text(cabbird::MessageId::DiagnosticsDrawBudget));
        ImGui::TextDisabled("%s", Text(cabbird::MessageId::DiagnosticsDeveloperHint));
    }

    void DrawLogsShell() {
        ImGui::BeginChild("PlatformLogsToolbar",
            ImVec2(0.0f, Scaled(50.0f)), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        SetAvailableItemWidth(IsCompact() ? 40.0f : 0.0f, 280.0f);
        if (ImGui::InputTextWithHint("##diagnostic-log-filter",
                Text(cabbird::MessageId::DiagnosticsSearchLogs), log_filter_.data(),
                log_filter_.size())) {
            model_.State().diagnostics_log_filter = log_filter_.data();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(model_.State().diagnostics_log_filter.empty());
        if (IconButton("x##clear-log-filter", Text(cabbird::MessageId::PluginsClearSearch))) {
            log_filter_.fill('\0');
            model_.State().diagnostics_log_filter.clear();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, logs_follow_live_ ? AccentColor() : SurfaceColor());
        const std::string live_label = StableLabel(
            cabbird::MessageId::DiagnosticsLive, "diagnostics-live");
        if (ImGui::Button(live_label.c_str())) logs_follow_live_ = !logs_follow_live_;
        ImGui::PopStyleColor();

        const auto events = plugins_.Events();
        const std::string_view text_filter(log_filter_.data());
        const std::string_view plugin_filter(model_.State().diagnostics_plugin_id);
        std::vector<std::size_t> filtered;
        filtered.reserve(events.size());
        for (std::size_t index = 0; index < events.size(); ++index) {
            if (!text_filter.empty() && events[index].find(text_filter) == std::string::npos) continue;
            if (!plugin_filter.empty() && events[index].find(plugin_filter) == std::string::npos) continue;
            filtered.push_back(index);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(filtered.empty());
        const std::string copy_filtered = StableLabel(
            cabbird::MessageId::DiagnosticsCopyFiltered, "diagnostics-copy-filtered");
        if (ImGui::Button(copy_filtered.c_str())) {
            std::string copied;
            for (const std::size_t index : filtered) {
                copied += events[index];
                copied += '\n';
            }
            ImGui::SetClipboardText(copied.c_str());
            status_ = Text(cabbird::MessageId::DiagnosticsFilteredCopied);
            status_failure_ = false;
        }
        ImGui::EndDisabled();
        ImGui::EndChild();

        if (selected_log_index_ >= events.size() && !filtered.empty()) selected_log_index_ = filtered.front();
        const float detail_height = IsCompact() ? 0.0f : 160.0f;
        BeginShellBodyChild("PlatformLogRecords", ImVec2(0.0f,
            detail_height == 0.0f ? 0.0f : -detail_height - 4.0f), true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextUnformatted(Text(cabbird::MessageId::DiagnosticsRawLogStream));
        ImGui::SameLine();
        const std::string event_count = std::to_string(events.size());
        const std::array<std::string_view, 1> count_arguments{event_count};
        const std::string buffered =
            Format(cabbird::MessageId::DiagnosticsBufferedRecords, count_arguments);
        ImGui::TextDisabled("%s", buffered.c_str());
        if (ImGui::BeginTable("PlatformLogTable", IsCompact() ? 1 : 2,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_SizingStretchProp, ShellScrollTableSize())) {
            if (!IsCompact()) ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonRecord),
                ImGuiTableColumnFlags_WidthFixed, 84.0f);
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonMessage),
                ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(filtered.size()));
            while (clipper.Step()) {
                for (int display_index = clipper.DisplayStart; display_index < clipper.DisplayEnd; ++display_index) {
                    const std::size_t event_index = filtered[static_cast<std::size_t>(display_index)];
                    ImGui::PushID(static_cast<int>(event_index));
                    ImGui::TableNextRow();
                    if (!IsCompact()) {
                        ImGui::TableNextColumn();
                        ImGui::Text("%zu", event_index + 1);
                    }
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(events[event_index].c_str(), selected_log_index_ == event_index,
                            ImGuiSelectableFlags_SpanAllColumns)) {
                        selected_log_index_ = event_index;
                    }
                    ImGui::PopID();
                }
            }
            if (logs_follow_live_ && !filtered.empty()) ImGui::SetScrollHereY(1.0f);
            ImGui::EndTable();
        }
        EndShellBodyChild();
        if (!IsCompact()) {
            BeginShellBodyChild("PlatformLogDetail", ImVec2(0.0f, detail_height), true);
            ImGui::TextUnformatted(Text(cabbird::MessageId::DiagnosticsSelectedRecord));
            if (selected_log_index_ < events.size()) {
                ImGui::Separator();
                ImGui::TextWrapped("%s", events[selected_log_index_].c_str());
            } else {
                ImGui::TextDisabled("%s", Text(cabbird::MessageId::DiagnosticsSelectRecord));
            }
            EndShellBodyChild();
        }
    }

    [[nodiscard]] const char* DeveloperPanelLabel(const DeveloperPanel panel) const noexcept {
        switch (panel) {
        case DeveloperPanel::Plugins: return Text(cabbird::MessageId::DeveloperPanelPlugins);
        case DeveloperPanel::Services: return Text(cabbird::MessageId::DeveloperPanelServices);
        case DeveloperPanel::Hooks: return Text(cabbird::MessageId::DeveloperPanelHooks);
        case DeveloperPanel::Memory: return Text(cabbird::MessageId::DeveloperPanelMemory);
        case DeveloperPanel::UnityProfile:
            return Text(cabbird::MessageId::DeveloperPanelUnityProfile);
        }
        return Text(cabbird::MessageId::DeveloperPanelPlugins);
    }

    void DrawDeveloperPluginSelector() {
        const auto& plugins = model_.Snapshot().installed_plugins;
        if (plugins.empty()) return;
        const auto* selected = model_.Snapshot().FindPlugin(model_.State().selected_plugin_id);
        if (selected == nullptr) selected = &plugins.front();
        if (cabbird::glass::BeginCombo("##developer-plugin", selected->name.empty()
                ? selected->id.c_str() : selected->name.c_str())) {
            for (const auto& plugin : plugins) {
                ImGui::PushID(plugin.id.c_str());
                const char* name = plugin.name.empty() ? plugin.id.c_str() : plugin.name.c_str();
                if (ImGui::Selectable(name, plugin.id == selected->id))
                    model_.State().selected_plugin_id = plugin.id;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }

    void DrawDeveloperShell() {
        const auto draw_content = [this] {
            switch (developer_panel_) {
            case DeveloperPanel::Plugins: DrawDeveloperPluginShell(); break;
            case DeveloperPanel::Services: DrawServices(); break;
            case DeveloperPanel::Hooks: DrawHooks(); break;
            case DeveloperPanel::Memory: DrawMemory(); break;
            case DeveloperPanel::UnityProfile: DrawDeveloperUnityCompatibility(); break;
            }
        };
        const auto draw_content_panel = [this, &draw_content] {
            if (developer_panel_ == DeveloperPanel::Plugins) {
                ImGui::BeginChild("PlatformDeveloperContent", ImVec2(0.0f, 0.0f), false,
                    ImGuiWindowFlags_NoBackground);
                draw_content();
                ImGui::EndChild();
                return;
            }
            BeginShellBodyChild("PlatformDeveloperContent", {}, true);
            draw_content();
            EndShellBodyChild();
        };
        if (IsCompact()) {
            ImGui::BeginChild("PlatformDeveloperPicker", ImVec2(0.0f, Scaled(48.0f)), false,
                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            const bool show_plugin_selector = developer_panel_ == DeveloperPanel::Plugins &&
                !model_.Snapshot().installed_plugins.empty();
            if (show_plugin_selector)
                ImGui::SetNextItemWidth(std::max(1.0f, (ImGui::GetContentRegionAvail().x - Scaled(10.0f)) * 0.4f));
            else
                SetAvailableItemWidth();
            if (cabbird::glass::BeginCombo("##developer-panel", DeveloperPanelLabel(developer_panel_))) {
                constexpr std::array<DeveloperPanel, 5> panels{
                    DeveloperPanel::Plugins, DeveloperPanel::Services, DeveloperPanel::Hooks,
                    DeveloperPanel::Memory, DeveloperPanel::UnityProfile};
                for (const DeveloperPanel panel : panels) {
                    ImGui::PushID(static_cast<int>(panel));
                    if (ImGui::Selectable(DeveloperPanelLabel(panel), developer_panel_ == panel)) {
                        developer_panel_ = panel;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if (show_plugin_selector && developer_panel_ == DeveloperPanel::Plugins) {
                ImGui::SameLine(0.0f, Scaled(10.0f));
                ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x));
                DrawDeveloperPluginSelector();
            }
            ImGui::EndChild();
            draw_content_panel();
            return;
        }

        constexpr std::array<DeveloperPanel, 5> panels{
            DeveloperPanel::Plugins, DeveloperPanel::Services, DeveloperPanel::Hooks,
            DeveloperPanel::Memory, DeveloperPanel::UnityProfile};
        BeginCardChild("PlatformDeveloperRail", ImVec2(Scaled(168.0f), 0.0f), 8.0f);
        std::size_t panel_index = static_cast<std::size_t>(
            std::ranges::find(panels, developer_panel_) - panels.begin());
        DrawLensRail("developer-panels", panels.size(), panel_index,
            [&](const std::size_t index) { return std::string(DeveloperPanelLabel(panels[index])); },
            40.0f);
        if (panel_index < panels.size()) developer_panel_ = panels[panel_index];
        ImGui::Dummy(ImVec2(0.0f, FloatingBarClearance()));
        EndCardChild();
        ImGui::SameLine(0.0f, Scaled(10.0f));
        draw_content_panel();
    }

    void DrawDeveloperPluginShell() {
        const auto& plugins = model_.Snapshot().installed_plugins;
        if (plugins.empty()) {
            DrawShellProviderUnavailable(
                Text(cabbird::MessageId::DeveloperPluginsTitle),
                Text(cabbird::MessageId::DeveloperCatalogUnavailable));
            return;
        }
        const auto* selected = model_.Snapshot().FindPlugin(model_.State().selected_plugin_id);
        if (selected == nullptr) selected = &plugins.front();
        if (!IsCompact()) {
            ImGui::BeginChild("PlatformDeveloperPluginToolbar", ImVec2(0.0f, Scaled(48.0f)), false,
                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::TextDisabled("%s", Text(cabbird::MessageId::CommonPlugin));
            ImGui::SameLine();
            SetAvailableItemWidth(56.0f, 300.0f);
            DrawDeveloperPluginSelector();
            ImGui::EndChild();
            selected = model_.Snapshot().FindPlugin(model_.State().selected_plugin_id);
            if (selected == nullptr) selected = &plugins.front();
        }

        ImGui::PushID(selected->id.c_str());
        // The selector already identifies the plugin in compact layouts.
        // Keep the detail body reachable when large fonts consume the fixed bands.
        if (!IsCompact()) {
            BeginCardChild("PlatformDeveloperPluginHeader", ImVec2(0.0f, Scaled(84.0f)), 0.0f,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            const ImVec2 icon_position = Offset(ImGui::GetCursorScreenPos(), Scaled(12.0f), Scaled(14.0f));
            DrawGenericPluginIcon(icon_position, Scaled(56.0f));
            ImGui::Dummy(Scaled(68.0f, 0.0f));
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::SetWindowFontScale(1.12f);
            ImGui::TextUnformatted(selected->name.empty() ? selected->id.c_str() : selected->name.c_str());
            ImGui::SetWindowFontScale(1.0f);
            ImGui::SameLine();
            ImGui::TextColored(PluginStateColor(selected->UiState()), "%s",
                DisplayPluginState(selected->UiState()));
            ImGui::TextDisabled("%s", selected->id.c_str());
            ImGui::EndGroup();
            EndCardChild();
        }

        ImGui::Dummy(Scaled(0.0f, 4.0f));
        {
            std::vector<DeveloperPluginTab> tabs{DeveloperPluginTab::Overview};
            if (selected->has_runtime_view) {
                tabs.push_back(DeveloperPluginTab::Capabilities);
                tabs.push_back(DeveloperPluginTab::Performance);
            }
            tabs.push_back(DeveloperPluginTab::Logs);
            std::vector<const char*> labels;
            labels.reserve(tabs.size());
            for (const DeveloperPluginTab tab : tabs) {
                switch (tab) {
                case DeveloperPluginTab::Overview:
                    labels.push_back(Text(cabbird::MessageId::DiagnosticsTabOverview));
                    break;
                case DeveloperPluginTab::Capabilities:
                    labels.push_back(Text(cabbird::MessageId::DeveloperTabCapabilities));
                    break;
                case DeveloperPluginTab::Performance:
                    labels.push_back(Text(cabbird::MessageId::DeveloperTabPerformance));
                    break;
                case DeveloperPluginTab::Logs:
                    labels.push_back(Text(cabbird::MessageId::DiagnosticsTabLogs));
                    break;
                }
            }
            const auto found = std::ranges::find(tabs, developer_plugin_tab_);
            int current = found == tabs.end() ? 0 : static_cast<int>(found - tabs.begin());
            cabbird::glass::SegmentedOptions options;
            options.height = 36.0f;
            options.scale = PlatformUiScale();
            options.padding_x = 14.0f;
            if (cabbird::glass::SegmentedControl(
                    "developer-plugin-tabs", labels, current, options)) {
                developer_plugin_tab_ = tabs[static_cast<std::size_t>(current)];
            }
        }
        ImGui::Dummy(Scaled(0.0f, 4.0f));

        if (!selected->has_runtime_view &&
            (developer_plugin_tab_ == DeveloperPluginTab::Capabilities ||
             developer_plugin_tab_ == DeveloperPluginTab::Performance)) {
            developer_plugin_tab_ = DeveloperPluginTab::Overview;
        }
        BeginShellBodyChild("PlatformDeveloperPluginBody", {}, true);
        switch (developer_plugin_tab_) {
        case DeveloperPluginTab::Overview: DrawDeveloperPluginOverview(*selected); break;
        case DeveloperPluginTab::Capabilities: DrawDeveloperPluginCapabilities(*selected); break;
        case DeveloperPluginTab::Performance: DrawDeveloperPluginPerformance(*selected); break;
        case DeveloperPluginTab::Logs: DrawDeveloperPluginLogs(*selected); break;
        }
        EndShellBodyChild();
        ImGui::PopID();
    }

    void DrawDeveloperPluginOverview(const cabbird::InstalledPluginView& plugin) {
        std::string dependencies;
        for (const auto& dependency : plugin.dependency_ids) {
            if (!dependencies.empty()) dependencies += ", ";
            dependencies += dependency;
        }
        if (dependencies.empty()) dependencies = Text(cabbird::MessageId::CommonNone);
        const std::string generation = std::to_string(plugin.generation);
        const std::array<std::string_view, 1> generation_arguments{generation};
        const std::string runtime_identity = plugin.has_runtime_view
            ? Format(cabbird::MessageId::DeveloperCurrentAbi, generation_arguments)
            : Text(cabbird::MessageId::DeveloperNoGeneration);
        if (ImGui::BeginTable("PlatformDeveloperOverview", 2,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            const auto row = [](const char* label, const std::string_view value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", label);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%.*s", static_cast<int>(value.size()), value.data());
            };
            row(Text(cabbird::MessageId::CommonState), DisplayPluginState(plugin.UiState()));
            const std::string_view reason = plugin.Reason();
            row(Text(cabbird::MessageId::DeveloperStatusReason), reason.empty()
                ? Text(cabbird::MessageId::DeveloperNoCompatibilityIssues) : reason);
            row(Text(cabbird::MessageId::DeveloperDependencies), dependencies);
            row(Text(cabbird::MessageId::DeveloperRuntimeIdentity), runtime_identity);
            row(Text(cabbird::MessageId::DeveloperPackage), plugin.package_directory.empty()
                ? Text(cabbird::MessageId::CommonUnavailable)
                : std::string_view{plugin.package_directory.string()});
            row(Text(cabbird::MessageId::DeveloperPluginId), plugin.id);
            ImGui::EndTable();
        }
        const std::string copy_id = StableLabel(
            cabbird::MessageId::PluginsCopyId, "developer-copy-plugin-id");
        if (ImGui::Button(copy_id.c_str())) {
            ImGui::SetClipboardText(plugin.id.c_str());
            status_ = Text(cabbird::MessageId::PluginsIdCopied);
            status_failure_ = false;
        }
    }

    void DrawDeveloperPluginCapabilities(const cabbird::InstalledPluginView& plugin) {
        if (!plugin.has_runtime_view) {
            ImGui::TextDisabled("%s", Text(cabbird::MessageId::DeveloperNoGeneration));
            return;
        }
        const auto& diagnostics = plugin.platform_diagnostics;
        ImGui::TextUnformatted(Text(cabbird::MessageId::DeveloperGrantedCapabilities));
        ImGui::Separator();
        if (diagnostics.capabilities.empty()) ImGui::TextDisabled(
            "%s", Text(cabbird::MessageId::DeveloperNoCapabilities));
        else for (const auto& capability : diagnostics.capabilities) ImGui::BulletText("%s", capability.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted(Text(cabbird::MessageId::DeveloperServiceVersions));
        if (diagnostics.services.empty()) ImGui::TextDisabled(
            "%s", Text(cabbird::MessageId::DeveloperNoServices));
        else for (const auto& service : diagnostics.services) {
            ImGui::BulletText("%s v%u", service.id.c_str(), service.version);
        }
        ImGui::Spacing();
        ImGui::TextUnformatted(Text(cabbird::MessageId::DeveloperResourceCounts));
        if (ImGui::BeginTable("PlatformDeveloperResources", 2,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            const PluginResourceCountsView& resources = diagnostics.resources;
            const std::array<std::pair<const char*, std::size_t>, 13> rows{{
                {Text(cabbird::MessageId::DeveloperResourceLedger), resources.ledger_resources},
                {Text(cabbird::MessageId::DeveloperResourceConfig), resources.configs},
                {Text(cabbird::MessageId::DeveloperResourceSelfTest), resources.self_tests},
                {Text(cabbird::MessageId::DeveloperResourceTask), resources.tasks},
                {Text(cabbird::MessageId::DeveloperResourceIpc), resources.ipc_resources},
                {Text(cabbird::MessageId::DeveloperResourceCommand), resources.commands},
                {Text(cabbird::MessageId::DeveloperResourceNotification), resources.notifications},
                {Text(cabbird::MessageId::DeveloperResourceHook), resources.hooks},
                {Text(cabbird::MessageId::DeveloperResourcePatch), resources.patches},
                {Text(cabbird::MessageId::DeveloperResourceWindow), resources.windows},
                {Text(cabbird::MessageId::DeveloperResourceFont), resources.fonts},
                {Text(cabbird::MessageId::DeveloperResourceTexture), resources.textures},
                {Text(cabbird::MessageId::DeveloperResourceHotkey), resources.hotkeys},
            }};
            for (const auto& [label, count] : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
                ImGui::TableNextColumn(); ImGui::Text("%zu", count);
            }
            ImGui::EndTable();
        }
        if (!diagnostics.deny_reasons.empty()) {
            ImGui::Spacing();
            ImGui::TextUnformatted(Text(cabbird::MessageId::DeveloperCapabilityDenies));
            for (const auto& reason : diagnostics.deny_reasons) ImGui::BulletText("%s", reason.c_str());
        }
    }

    void DrawDeveloperPluginPerformance(const cabbird::InstalledPluginView& plugin) {
        if (!plugin.has_runtime_view) {
            ImGui::TextDisabled("%s", Text(cabbird::MessageId::DeveloperNoGeneration));
            return;
        }
        if (ImGui::BeginTable("PlatformDeveloperPluginPerformance", 7,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonCallback));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonCalls));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonFaults));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonSlow));
            ImGui::TableSetupColumn("p50"); ImGui::TableSetupColumn("p95");
            ImGui::TableSetupColumn("p99"); ImGui::TableHeadersRow();
            const auto row = [this](const char* label, const CallbackMetricsView& metrics) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(metrics.calls));
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(metrics.faults));
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(metrics.slow_calls));
                for (const double percentile : {metrics.p50_milliseconds, metrics.p95_milliseconds,
                                                metrics.p99_milliseconds}) {
                    ImGui::TableNextColumn();
                    if (metrics.calls == 0) ImGui::TextDisabled(
                        "%s", Text(cabbird::MessageId::CommonNoSamples));
                    else ImGui::Text("%.3f ms", percentile);
                }
            };
            row(Text(cabbird::MessageId::CommonUpdate), plugin.update_metrics);
            row(Text(cabbird::MessageId::CommonDraw), plugin.draw_metrics);
            ImGui::EndTable();
        }
        ImGui::TextDisabled("%s", Text(cabbird::MessageId::DeveloperCallbackBudget));
        const auto& endpoints = plugin.platform_diagnostics.ipc_endpoints;
        ImGui::Spacing();
        ImGui::TextUnformatted(Text(cabbird::MessageId::DeveloperIpcEndpoints));
        if (endpoints.empty()) {
            ImGui::TextDisabled("%s", Text(cabbird::MessageId::DeveloperNoIpcEndpoints));
            return;
        }
        if (ImGui::BeginTable("PlatformDeveloperPluginIpc", 8,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollX)) {
            ImGui::TableSetupColumn(Text(cabbird::MessageId::DeveloperEndpoint));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::DeveloperRole));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonVersion));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::DeveloperConsumers));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonCalls));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::DeveloperFailuresTimeouts));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::DeveloperSubscriptionsPending));
            ImGui::TableSetupColumn("p95");
            ImGui::TableHeadersRow();
            for (const cabbird::IpcEndpointDiagnostics& endpoint : endpoints) {
                std::string consumers;
                for (const std::string& consumer : endpoint.consumers) {
                    if (!consumers.empty()) consumers += ", ";
                    consumers += consumer;
                }
                if (consumers.empty()) consumers = Text(cabbird::MessageId::CommonNone);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(endpoint.id.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(
                    Text(endpoint.provider == plugin.id
                        ? cabbird::MessageId::CommonProvider
                        : cabbird::MessageId::CommonConsumer));
                ImGui::TableNextColumn(); ImGui::Text("%u.%u",
                    endpoint.major_version, endpoint.minor_version);
                ImGui::TableNextColumn(); ImGui::TextWrapped("%s", consumers.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%llu",
                    static_cast<unsigned long long>(endpoint.calls));
                ImGui::TableNextColumn(); ImGui::Text("%llu / %llu",
                    static_cast<unsigned long long>(endpoint.failures),
                    static_cast<unsigned long long>(endpoint.timeouts));
                ImGui::TableNextColumn(); ImGui::Text("%zu / %zu",
                    endpoint.subscriptions, endpoint.pending_calls);
                ImGui::TableNextColumn(); ImGui::Text("%.3f ms", endpoint.p95_milliseconds);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        for (const cabbird::IpcEndpointDiagnostics& endpoint : endpoints) {
            ImGui::TextUnformatted(endpoint.id.c_str());
            const std::array<std::string_view, 1> request_arguments{
                endpoint.request_schema_hash};
            const std::array<std::string_view, 1> response_arguments{
                endpoint.response_schema_hash};
            const std::array<std::string_view, 1> event_arguments{
                endpoint.event_schema_hash};
            ImGui::TextWrapped("%s", Format(
                cabbird::MessageId::DeveloperRequestSchema, request_arguments).c_str());
            ImGui::TextWrapped("%s", Format(
                cabbird::MessageId::DeveloperResponseSchema, response_arguments).c_str());
            ImGui::TextWrapped("%s", Format(
                cabbird::MessageId::DeveloperEventSchema, event_arguments).c_str());
        }
    }

    void DrawDeveloperPluginLogs(const cabbird::InstalledPluginView& plugin) {
        ImGui::TextUnformatted(Text(cabbird::MessageId::DeveloperRecentRecords));
        ImGui::TextDisabled("%s", Text(cabbird::MessageId::DeveloperRawRecordsHint));
        const std::string open_logs = StableLabel(
            cabbird::MessageId::DeveloperOpenFullLogs, "developer-open-full-logs");
        if (ImGui::Button(open_logs.c_str())) OpenPluginLogs(plugin.id);
    }

    void DrawDeveloperUnityCompatibility() {
        const auto& compatibility = model_.Snapshot().unity_compatibility;
        ImGui::TextUnformatted(Text(cabbird::MessageId::ShellRouteUnityCompatibility));
        ImGui::TextDisabled("%s", Text(cabbird::MessageId::DeveloperExactBuildHint));
        ImGui::Separator();
        const std::array<std::string_view, 1> level_arguments{
            UnityLevelName(compatibility.level)};
        const std::string level = Format(cabbird::MessageId::DeveloperLevel, level_arguments);
        ImGui::TextUnformatted(level.c_str());
        const std::string_view reason = compatibility.reason.empty()
            ? Text(cabbird::MessageId::DeveloperNoCompatibilityProvider)
            : std::string_view{compatibility.reason};
        const std::array<std::string_view, 1> reason_arguments{reason};
        const std::string reason_text =
            Format(cabbird::MessageId::DeveloperReason, reason_arguments);
        ImGui::TextWrapped("%s", reason_text.c_str());
        ImGui::Spacing();
        if (IsCompact()) {
            DrawUnityFeatureMatrix();
            DrawUnityBuildPanel();
        } else if (ImGui::BeginTable("PlatformDeveloperUnityColumns", 2,
                       ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(Text(cabbird::MessageId::DeveloperMatrix),
                ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn(Text(cabbird::MessageId::DeveloperBuild),
                ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            DrawUnityFeatureMatrix();
            ImGui::TableNextColumn();
            DrawUnityBuildPanel();
            ImGui::EndTable();
        }
        ImGui::Separator();
        ImGui::TextUnformatted(Text(cabbird::MessageId::DeveloperRawProfileJson));
        if (!diagnostics_.profile_json) {
            ImGui::TextDisabled("%s", Text(cabbird::MessageId::DeveloperProfileUnavailable));
            return;
        }
        const std::string profile = diagnostics_.profile_json();
        if (profile.empty()) {
            ImGui::TextDisabled("%s", Text(cabbird::MessageId::DeveloperProfileUnavailable));
            return;
        }
        ImGui::TextWrapped("%s", profile.c_str());
    }

    const char* SettingsSectionName(const SettingsSection section) const noexcept {
        switch (section) {
        case SettingsSection::Interface: return Text(cabbird::MessageId::SettingsSectionInterface);
        case SettingsSection::Input: return Text(cabbird::MessageId::SettingsSectionInput);
        case SettingsSection::Updates: return Text(cabbird::MessageId::SettingsSectionUpdates);
        case SettingsSection::Diagnostics:
            return Text(cabbird::MessageId::SettingsSectionDiagnostics);
        case SettingsSection::Advanced: return Text(cabbird::MessageId::SettingsSectionAdvanced);
        case SettingsSection::About: return Text(cabbird::MessageId::SettingsSectionAbout);
        }
        return Text(cabbird::MessageId::ShellRouteSettings);
    }

    [[nodiscard]] bool SettingMatches(
        const std::string_view label,
        const std::string_view description,
        const std::string_view keywords = {}) const {
        const std::string_view query(settings_search_.data());
        if (query.empty()) return true;
        const auto contains = [](const std::string_view value, const std::string_view needle) {
            std::string haystack(value);
            std::string lowered(needle);
            std::ranges::transform(haystack, haystack.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            std::ranges::transform(lowered, lowered.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return haystack.find(lowered) != std::string::npos;
        };
        return contains(label, query) || contains(description, query) || contains(keywords, query);
    }

    [[nodiscard]] const char* SettingError(const std::string_view id) const noexcept {
        const auto found = std::ranges::find(settings_validation_errors_, id,
            &cabbird::PlatformSettingsValidationError::setting_id);
        if (found == settings_validation_errors_.end()) return nullptr;
        if (id == "interface.language") {
            return Text(cabbird::MessageId::SettingsValidationLanguage);
        }
        if (id == "interface.scale_percent") {
            return Text(cabbird::MessageId::SettingsValidationScale);
        }
        if (id == "interface.opacity_percent") {
            return Text(cabbird::MessageId::SettingsValidationOpacity);
        }
        if (id == "input.hide_all_toggle" && settings_draft_ &&
            settings_draft_->input_hide_all_toggle == settings_draft_->input_menu_toggle) {
            return Text(cabbird::MessageId::SettingsValidationHotkeyConflict);
        }
        if (id == "input.menu_toggle" || id == "input.hide_all_toggle") {
            return Text(cabbird::MessageId::SettingsValidationMenuToggle);
        }
        if (id == "diagnostics.ring_capacity") {
            return Text(cabbird::MessageId::SettingsValidationRingCapacity);
        }
        if (id == "interface.custom_accent" || id == "interface.custom_text" ||
            id == "interface.custom_window_background" ||
            id == "interface.custom_child_background" || id == "interface.custom_border") {
            return Text(cabbird::MessageId::SettingsValidationColor);
        }
        return found->message.c_str();
    }

    template <typename DrawControl>
    bool DrawSettingRow(
        const char* id,
        const char* label,
        const char* consequence,
        const char* keywords,
        DrawControl&& draw_control,
        const char* badge = nullptr) {
        if (!SettingMatches(label, consequence, keywords)) return false;
        if (!settings_section_heading_drawn_) {
            ImGui::SetWindowFontScale(1.15f);
            ImGui::TextUnformatted(SettingsSectionName(settings_drawing_section_));
            ImGui::SetWindowFontScale(1.0f);
            ImGui::Separator();
            settings_section_heading_drawn_ = true;
        }
        ImGui::PushID(id);
        if (ImGui::BeginTable("##setting-row", 2,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("##setting", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn(
                "##control", ImGuiTableColumnFlags_WidthFixed, Scaled(250.0f));
            ImGui::TableNextRow(ImGuiTableRowFlags_None, Scaled(58.0f));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            if (badge != nullptr) {
                ImGui::SameLine();
                ImGui::TextColored(InfoColor(), "%s", badge);
            }
            ImGui::TextDisabled("%s", consequence);
            ImGui::TableNextColumn();
            SetAvailableItemWidth(0.0f, 238.0f);
            draw_control();
            if (const char* error = SettingError(id)) {
                ImGui::TextColored(ErrorColor(), "%s", error);
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
        return true;
    }

    std::string VirtualKeyName(const std::uint32_t key) const {
        const auto fallback = [&] {
            const std::string key_text = std::to_string(key);
            const std::array<std::string_view, 1> arguments{key_text};
            return Format(cabbird::MessageId::SettingsVirtualKey, arguments);
        };
        const UINT scan_code = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
        wchar_t buffer[64]{};
        const LONG parameter = static_cast<LONG>(scan_code << 16U);
        if (GetKeyNameTextW(parameter, buffer, static_cast<int>(std::size(buffer))) <= 0) {
            return fallback();
        }
        const int size = WideCharToMultiByte(
            CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
        if (size <= 1) return fallback();
        std::string result(static_cast<std::size_t>(size), '\0');
        static_cast<void>(WideCharToMultiByte(
            CP_UTF8, 0, buffer, -1, result.data(), size, nullptr, nullptr));
        result.pop_back();
        return result;
    }

    [[nodiscard]] const char* LanguagePreferenceLabel(
        const cabbird::LanguagePreference value) const noexcept {
        switch (value) {
        case cabbird::LanguagePreference::Auto:
            return Text(cabbird::MessageId::SettingsAutomatic);
        case cabbird::LanguagePreference::EnUs:
            return Text(cabbird::MessageId::SettingsEnglish);
        case cabbird::LanguagePreference::ZhCn:
            return Text(cabbird::MessageId::SettingsSimplifiedChinese);
        }
        return Text(cabbird::MessageId::CommonUnknown);
    }

    [[nodiscard]] const char* PaletteLabel(
        const cabbird::CabbirdUiPalette value) noexcept {
        switch (value) {
        case cabbird::CabbirdUiPalette::Moss:
            return Text(cabbird::MessageId::SettingsPaletteMoss);
        case cabbird::CabbirdUiPalette::Aurora:
            return Text(cabbird::MessageId::SettingsPaletteAurora);
        case cabbird::CabbirdUiPalette::Ember:
            return Text(cabbird::MessageId::SettingsPaletteEmber);
        case cabbird::CabbirdUiPalette::Paper:
            return Text(cabbird::MessageId::SettingsPalettePaper);
        case cabbird::CabbirdUiPalette::CabbirdHub: return "CabbirdHub";
        case cabbird::CabbirdUiPalette::Custom:
            return Text(cabbird::MessageId::SettingsPaletteCustom);
        }
        return "CabbirdHub";
    }

    [[nodiscard]] const char* MinimumLogLevelLabel(
        const cabbird::PlatformMinimumLogLevel value) const noexcept {
        switch (value) {
        case cabbird::PlatformMinimumLogLevel::Trace:
            return Text(cabbird::MessageId::SettingsTrace);
        case cabbird::PlatformMinimumLogLevel::Debug:
            return Text(cabbird::MessageId::SettingsDebug);
        case cabbird::PlatformMinimumLogLevel::Info:
            return Text(cabbird::MessageId::SettingsInfo);
        case cabbird::PlatformMinimumLogLevel::Warning:
            return Text(cabbird::MessageId::SettingsWarning);
        case cabbird::PlatformMinimumLogLevel::Error:
            return Text(cabbird::MessageId::SettingsError);
        }
        return Text(cabbird::MessageId::CommonUnknown);
    }

    // Snapshot the real-time down state of every key so CaptureSettingsHotkey can
    // detect a fresh press by its rising edge. The async-input reconciler polls
    // GetAsyncKeyState every frame, which clears the "pressed since last call"
    // low-order bit, so that bit cannot be used to detect key presses here.
    void BeginSettingsHotkeyCapture(bool hide_all) {
        settings_hotkey_hide_all_ = hide_all;
        settings_hotkey_capture_ = true;
        for (std::uint32_t key = 0; key <= 0xff; ++key) {
            settings_hotkey_down_[key] = (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
        }
    }

    void CaptureSettingsHotkey() {
        if (!settings_hotkey_capture_ || !settings_draft_) return;
        if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            settings_hotkey_capture_ = false;
            return;
        }
        const auto is_down = [](std::uint32_t key) {
            return (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
        };
        for (std::uint32_t key = 8; key <= 0xff; ++key) {
            if (key >= VK_LBUTTON && key <= VK_XBUTTON2) continue;
            // Generic modifier VKs report both physical keys. Ignore the
            // aliases and capture VK_LSHIFT/VK_RSHIFT distinctly.
            if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU ||
                key == VK_LCONTROL || key == VK_RCONTROL ||
                key == VK_LMENU || key == VK_RMENU) continue;
            const bool down = is_down(key);
            const bool pressed = down && !settings_hotkey_down_[key];
            settings_hotkey_down_[key] = down;
            if (!pressed) continue;
            if (key == VK_ESCAPE) {
                settings_hotkey_capture_ = false;
                return;
            }
            if (settings_hotkey_hide_all_) settings_draft_->input_hide_all_toggle = key;
            else settings_draft_->input_menu_toggle = key;
            settings_hotkey_capture_ = false;
            return;
        }
    }

    // --- Third-party plugin source editor --------------------------------------

    static std::string TrimAscii(std::string_view value) {
        const auto begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string_view::npos) return {};
        const auto end = value.find_last_not_of(" \t\r\n");
        return std::string(value.substr(begin, end - begin + 1));
    }

    static void CopyToBuffer(std::array<char, 1024>& buffer, std::string_view value) {
        const std::size_t count = (std::min)(value.size(), buffer.size() - 1);
        value.copy(buffer.data(), count);
        buffer[count] = '\0';
    }

    [[nodiscard]] cabbird::PluginRepositoryConfig BuildRepositoryConfigFromEditor() const {
        cabbird::PluginRepositoryConfig config;
        config.enabled = repo_editor_master_enabled_;
        config.allow_insecure_sources = repo_editor_allow_insecure_;
        for (const auto& row : repo_editor_rows_) {
            std::string url = TrimAscii(row.url.data());
            if (url.empty()) continue;
            config.repositories.push_back({std::move(url), row.enabled});
        }
        return config;
    }

    void LoadRepositoryEditor(const cabbird::PluginRepositoryConfig& config) {
        repo_editor_master_enabled_ = config.enabled;
        repo_editor_allow_insecure_ = config.allow_insecure_sources;
        repo_editor_rows_.clear();
        for (const auto& entry : config.repositories) {
            RepositoryChannelRow row;
            row.enabled = entry.enabled;
            CopyToBuffer(row.url, entry.url);
            repo_editor_rows_.push_back(row);
        }
        repo_editor_baseline_ = config;
        repo_editor_loaded_ = true;
    }

    void EnsureRepositoryEditorLoaded() {
        if (repo_editor_loaded_ || !diagnostics_.repository_config) return;
        LoadRepositoryEditor(diagnostics_.repository_config());
    }

    [[nodiscard]] bool RepositoryEditorDirty() const {
        return repo_editor_loaded_ &&
            BuildRepositoryConfigFromEditor() != repo_editor_baseline_;
    }

    void ApplyRepositoryEditor() {
        if (!diagnostics_.repository_configure) {
            repo_editor_status_ = Text(cabbird::MessageId::SettingsRepositoriesProviderUnavailable);
            repo_editor_status_failure_ = true;
            return;
        }
        const auto config = BuildRepositoryConfigFromEditor();
        for (const auto& entry : config.repositories) {
            if (entry.enabled && !cabbird::IsPluginRepositoryUriAllowed(
                    entry.url, config.allow_insecure_sources)) {
                repo_editor_status_ = Text(cabbird::MessageId::SettingsRepositoriesInvalidUrl);
                repo_editor_status_failure_ = true;
                return;
            }
        }
        {
            std::scoped_lock operation_lock(operation_mutex_);
            pending_repository_configure_ = config;
        }
        repo_editor_save_pending_ = true;
        repo_editor_status_.clear();
    }

    void DrawRepositoryChannelsEditor() {
        if (!diagnostics_.repository_config || !diagnostics_.repository_configure) {
            ImGui::TextDisabled("%s",
                Text(cabbird::MessageId::SettingsRepositoriesProviderUnavailable));
            return;
        }
        EnsureRepositoryEditorLoaded();

        ImGui::BeginDisabled(repo_editor_save_pending_);
        cabbird::glass::Switch(StableLabel(cabbird::MessageId::SettingsRepositoriesEnable,
            "repo-master-enable").c_str(), &repo_editor_master_enabled_);
        ImGui::TextDisabled("%s", Text(cabbird::MessageId::SettingsRepositoriesEnableHint));
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(WarningColor(), "%s",
            Text(cabbird::MessageId::SettingsRepositoriesWarning));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        // Live channel status from the coordinator snapshot.
        const auto& repository = model_.Snapshot().repository;
        const std::string online = std::to_string(repository.online_sources);
        const std::string cached = std::to_string(repository.cached_sources);
        const std::array<std::string_view, 2> source_arguments{online, cached};
        ImGui::TextDisabled("%s", DisplayRepositoryState(repository.state));
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::TextDisabled("%s",
            Format(cabbird::MessageId::SettingsRepositoriesSources, source_arguments).c_str());
        ImGui::Separator();
        ImGui::Spacing();

        // One editable row per channel.
        int remove_index = -1;
        for (std::size_t index = 0; index < repo_editor_rows_.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            auto& row = repo_editor_rows_[index];
            cabbird::glass::Switch("##channel-enabled", &row.enabled);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", Text(cabbird::MessageId::SettingsRepositoriesToggleHint));
            }
            ImGui::SameLine();
            const float remove_width = 34.0f;
            ImGui::SetNextItemWidth(
                AvailableItemWidth(remove_width + ImGui::GetStyle().ItemSpacing.x));
            ImGui::InputTextWithHint("##channel-url",
                Text(cabbird::MessageId::SettingsRepositoriesUrlHint),
                row.url.data(), row.url.size());
            ImGui::SameLine();
            if (DrawShellIconButton("remove-channel", ShellGlyph::Close,
                    Text(cabbird::MessageId::SettingsRepositoriesRemove))) {
                remove_index = static_cast<int>(index);
            }
            ImGui::PopID();
        }
        if (remove_index >= 0) {
            repo_editor_rows_.erase(repo_editor_rows_.begin() + remove_index);
        }
        if (repo_editor_rows_.empty()) {
            ImGui::TextDisabled("%s", Text(cabbird::MessageId::SettingsRepositoriesEmpty));
        }

        ImGui::Spacing();
        const std::string add_label = cabbird::StableDisplayLabel(
            "+  " + std::string(Text(cabbird::MessageId::SettingsRepositoriesAdd)),
            "repo-add-channel");
        if (ImGui::Button(add_label.c_str())) {
            repo_editor_rows_.emplace_back();
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        const bool dirty = RepositoryEditorDirty();
        ImGui::BeginDisabled(!dirty || repo_editor_save_pending_);
        if (PrimaryButton(StableLabel(cabbird::MessageId::SettingsRepositoriesApply,
                "repo-apply").c_str(), ImVec2(0.0f, 32.0f))) {
            ApplyRepositoryEditor();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!dirty || repo_editor_save_pending_);
        if (ImGui::Button(StableLabel(cabbird::MessageId::SettingsRepositoriesReset,
                "repo-reset").c_str(), ImVec2(0.0f, 32.0f))) {
            LoadRepositoryEditor(repo_editor_baseline_);
            repo_editor_status_.clear();
        }
        ImGui::EndDisabled();
        if (!repo_editor_status_.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(repo_editor_status_failure_ ? ErrorColor() : InfoColor(),
                "%s", repo_editor_status_.c_str());
        }
    }

    bool DrawSettingsSection(const SettingsSection section) {
        if (!settings_draft_) return false;
        auto& values = *settings_draft_;
        bool visible{};
        settings_drawing_section_ = section;
        settings_section_heading_drawn_ = false;
        const auto heading = [&] {
            if (!settings_section_heading_drawn_) {
                ImGui::SetWindowFontScale(1.15f);
                ImGui::TextUnformatted(SettingsSectionName(section));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::Separator();
                settings_section_heading_drawn_ = true;
            }
            visible = true;
        };
        const auto row = [&](auto&&... arguments) {
            visible = DrawSettingRow(std::forward<decltype(arguments)>(arguments)...) || visible;
        };

        switch (section) {
        case SettingsSection::Interface:
            row("interface.language", Text(cabbird::MessageId::SettingsLanguage),
                Text(cabbird::MessageId::SettingsLanguageHint), "locale language", [&] {
                    if (cabbird::glass::BeginCombo(
                            "##value", LanguagePreferenceLabel(values.interface_language))) {
                        constexpr std::array<cabbird::LanguagePreference, 3> options{
                            cabbird::LanguagePreference::Auto,
                            cabbird::LanguagePreference::EnUs,
                            cabbird::LanguagePreference::ZhCn};
                        constexpr std::array<std::string_view, 3> ids{
                            "language-auto", "language-en-us", "language-zh-cn"};
                        for (std::size_t index = 0; index < options.size(); ++index) {
                            const std::string option = cabbird::StableDisplayLabel(
                                LanguagePreferenceLabel(options[index]), ids[index]);
                            if (ImGui::Selectable(option.c_str(),
                                    values.interface_language == options[index])) {
                                values.interface_language = options[index];
                            }
                        }
                        ImGui::EndCombo();
                    }
                }, Text(cabbird::MessageId::SettingsRestartRequired));
            row("interface.palette", Text(cabbird::MessageId::SettingsPalette),
                Text(cabbird::MessageId::SettingsPaletteHint),
                "palette theme color moss aurora ember paper cabbirdhub custom", [&] {
                    if (cabbird::glass::BeginCombo("##value", PaletteLabel(values.interface_palette))) {
                        constexpr std::array<cabbird::CabbirdUiPalette, 6> options{
                            cabbird::CabbirdUiPalette::Moss,
                            cabbird::CabbirdUiPalette::Aurora,
                            cabbird::CabbirdUiPalette::Ember,
                            cabbird::CabbirdUiPalette::Paper,
                            cabbird::CabbirdUiPalette::CabbirdHub,
                            cabbird::CabbirdUiPalette::Custom};
                        constexpr std::array<std::string_view, 6> ids{
                            "palette-moss", "palette-aurora", "palette-ember",
                            "palette-paper", "palette-cabbirdhub", "palette-custom"};
                        for (std::size_t index = 0; index < options.size(); ++index) {
                            const std::string option = cabbird::StableDisplayLabel(
                                PaletteLabel(options[index]), ids[index]);
                            if (ImGui::Selectable(option.c_str(),
                                    values.interface_palette == options[index])) {
                                values.interface_palette = options[index];
                            }
                        }
                        ImGui::EndCombo();
                    }
                });
            if (values.interface_palette == cabbird::CabbirdUiPalette::Custom) {
                const auto color_row = [&](const char* id, const cabbird::MessageId label,
                                           const cabbird::MessageId hint, const char* keywords,
                                           cabbird::CabbirdUiColor& color) {
                    row(id, Text(label), Text(hint), keywords, [&] {
                        std::array channels{color.red, color.green, color.blue};
                        if (ImGui::ColorEdit3("##value", channels.data())) {
                            color = {channels[0], channels[1], channels[2], 1.0f};
                        }
                    });
                };
                color_row("interface.custom_accent",
                    cabbird::MessageId::SettingsCustomAccent,
                    cabbird::MessageId::SettingsCustomAccentHint,
                    "custom palette accent highlight selection color",
                    values.interface_custom_colors.accent);
                color_row("interface.custom_text",
                    cabbird::MessageId::SettingsCustomText,
                    cabbird::MessageId::SettingsCustomTextHint,
                    "custom palette text foreground color",
                    values.interface_custom_colors.text);
                color_row("interface.custom_window_background",
                    cabbird::MessageId::SettingsCustomWindowBackground,
                    cabbird::MessageId::SettingsCustomWindowBackgroundHint,
                    "custom palette window navigation background color",
                    values.interface_custom_colors.window_background);
                color_row("interface.custom_child_background",
                    cabbird::MessageId::SettingsCustomChildBackground,
                    cabbird::MessageId::SettingsCustomChildBackgroundHint,
                    "custom palette content panel control background color",
                    values.interface_custom_colors.child_background);
                color_row("interface.custom_border",
                    cabbird::MessageId::SettingsCustomBorder,
                    cabbird::MessageId::SettingsCustomBorderHint,
                    "custom palette border outline separator color",
                    values.interface_custom_colors.border);
            }
            row("interface.background_image", Text(cabbird::MessageId::SettingsBackgroundImage),
                Text(cabbird::MessageId::SettingsBackgroundImageHint),
                "background wallpaper image fill crop stretch", [&] {
                    if (!theme_layout_available_) {
                        ImGui::TextDisabled("%s", Text(cabbird::MessageId::SettingsThemeLayoutUnavailable));
                        return;
                    }
                    const float line_start = ImGui::GetCursorPosX();
                    bool enabled = theme_layout_.background.mode != cabbird::PlatformUiBackgroundMode::Disabled;
                    if (cabbird::glass::Switch("##background-enabled", &enabled)) {
                        const int mode = static_cast<int>(enabled ? cabbird::PlatformUiBackgroundMode::Crop
                            : cabbird::PlatformUiBackgroundMode::Disabled);
                        { std::scoped_lock lock(theme_mutex_); theme_mode_request_ = mode; }
                        theme_layout_.background.mode = static_cast<cabbird::PlatformUiBackgroundMode>(mode);
                        theme_changed_.notify_all();
                    }
                    ImGui::SameLine();
                    bool picking;
                    { std::scoped_lock lock(theme_mutex_); picking = theme_pick_request_ || theme_picker_running_; }
                    ImGui::BeginDisabled(picking);
                    if (ImGui::Button(StableLabel(cabbird::MessageId::SettingsBackgroundOpen, "background-open").c_str())) {
                        { std::scoped_lock lock(theme_mutex_); theme_pick_request_ = true; }
                        theme_changed_.notify_all();
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(std::max(Scaled(48.0f),
                        Scaled(238.0f) - (ImGui::GetCursorPosX() - line_start)));
                    const std::array labels{Text(cabbird::MessageId::SettingsBackgroundModeCenter),
                        Text(cabbird::MessageId::SettingsBackgroundModeCrop),
                        Text(cabbird::MessageId::SettingsBackgroundModeStretch)};
                    int mode = std::clamp(static_cast<int>(theme_layout_.background.mode), 1, 3) - 1;
                    if (cabbird::glass::Combo("##background-mode", &mode, labels.data(), static_cast<int>(labels.size()))) {
                        { std::scoped_lock lock(theme_mutex_); theme_mode_request_ = mode + 1; }
                        theme_layout_.background.mode = static_cast<cabbird::PlatformUiBackgroundMode>(mode + 1);
                        theme_changed_.notify_all();
                    }
                    if (!theme_layout_message_.empty()) {
                        const std::array<std::string_view, 1> arguments{theme_layout_message_};
                        ImGui::TextWrapped("%s", Format(cabbird::MessageId::SettingsBackgroundFailed, arguments).c_str());
                    }
                });
            row("interface.background_visibility", Text(cabbird::MessageId::SettingsBackgroundVisibility),
                Text(cabbird::MessageId::SettingsBackgroundVisibilityHint),
                "background wallpaper veil dim opacity readability", [&] {
                    int value = static_cast<int>(std::lround(theme_opacity_edit_.value_or(theme_layout_.surface_opacity) * 100.0f));
                    if (cabbird::glass::SliderInt("##value", &value, 0, 100, "%d%%"))
                        theme_opacity_edit_ = static_cast<float>(value) / 100.0f;
                    if (theme_opacity_edit_ && !ImGui::IsItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        theme_layout_.surface_opacity = *theme_opacity_edit_;
                        { std::scoped_lock lock(theme_mutex_); theme_opacity_request_ = *theme_opacity_edit_; }
                        theme_opacity_edit_.reset();
                        theme_changed_.notify_all();
                    }
                });
            row("interface.scale_percent", Text(cabbird::MessageId::SettingsInterfaceScale),
                Text(cabbird::MessageId::SettingsInterfaceScaleHint),
                "dpi font size zoom", [&] {
                    int value = static_cast<int>(settings_scale_edit_percent_.value_or(
                        values.interface_scale_percent));
                    if (cabbird::glass::SliderInt("##value", &value,
                            static_cast<int>(cabbird::kPlatformInterfaceScaleMinimumPercent),
                            static_cast<int>(cabbird::kPlatformInterfaceScaleMaximumPercent),
                            "%d%%")) {
                        constexpr int step = static_cast<int>(
                            cabbird::kPlatformInterfaceScaleStepPercent);
                        value = ((value + step / 2) / step) * step;
                        settings_scale_edit_percent_ = static_cast<std::uint32_t>(value);
                    }
                    if (settings_scale_edit_percent_ && !ImGui::IsItemActive() &&
                        !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        values.interface_scale_percent = *settings_scale_edit_percent_;
                        settings_scale_edit_percent_.reset();
                    }
                }, Text(cabbird::MessageId::SettingsUiRebuild));
            row("interface.opacity_percent", Text(cabbird::MessageId::SettingsWindowOpacity),
                Text(cabbird::MessageId::SettingsWindowOpacityHint),
                "alpha transparency", [&] {
                    int value = static_cast<int>(values.interface_opacity_percent);
                    if (cabbird::glass::SliderInt("##value", &value, 10, 100, "%d%%")) {
                        value = ((value + 2) / 5) * 5;
                        values.interface_opacity_percent = static_cast<std::uint32_t>(value);
                    }
                });
            row("interface.reduced_motion", Text(cabbird::MessageId::SettingsReduceMotion),
                Text(cabbird::MessageId::SettingsReduceMotionHint),
                "animation accessibility", [&] {
                    cabbird::glass::Switch("##value", &values.interface_reduced_motion);
                });
            row("interface.reduced_transparency", Text(cabbird::MessageId::SettingsReduceTransparency),
                Text(cabbird::MessageId::SettingsReduceTransparencyHint),
                "glass blur transparency contrast accessibility", [&] {
                    cabbird::glass::Switch("##value", &values.interface_reduced_transparency);
                });
            row("interface.remember_last_route", Text(cabbird::MessageId::SettingsRememberLastPage),
                Text(cabbird::MessageId::SettingsRememberLastPageHint),
                "startup route page", [&] {
                    cabbird::glass::Switch("##value", &values.interface_remember_last_route);
                });
            break;
        case SettingsSection::Input:
            CaptureSettingsHotkey();
            row("input.menu_toggle", Text(cabbird::MessageId::SettingsMenuToggle),
                Text(cabbird::MessageId::SettingsMenuToggleHint),
                "hotkey keyboard insert", [&] {
                    settings_hotkey_capture_drawn_ = true;
                    const std::string label = settings_hotkey_capture_ && !settings_hotkey_hide_all_
                        ? Text(cabbird::MessageId::SettingsPressKey)
                        : VirtualKeyName(values.input_menu_toggle);
                    if (ImGui::Button((label + std::string("###settings-hotkey")).c_str(), FillAvailableSize(0.0f))) {
                        BeginSettingsHotkeyCapture(false);
                    }
                });
            row("input.hide_all_toggle", Text(cabbird::MessageId::SettingsHideAllToggle),
                Text(cabbird::MessageId::SettingsHideAllToggleHint),
                "hotkey keyboard delete hide all overlay", [&] {
                    settings_hotkey_capture_drawn_ = true;
                    const std::string label = settings_hotkey_capture_ && settings_hotkey_hide_all_
                        ? Text(cabbird::MessageId::SettingsPressKey)
                        : VirtualKeyName(values.input_hide_all_toggle);
                    if (ImGui::Button((label + std::string("###settings-hotkey")).c_str(), FillAvailableSize(0.0f))) {
                        BeginSettingsHotkeyCapture(true);
                    }
                });
            row("input.gamepad_navigation", Text(cabbird::MessageId::SettingsGamepadNavigation),
                Text(cabbird::MessageId::SettingsGamepadNavigationHint),
                "controller", [&] { cabbird::glass::Switch("##value", &values.input_gamepad_navigation); });
            break;
        case SettingsSection::Updates:
            row("updates.channel", Text(cabbird::MessageId::SettingsUpdateChannel),
                Text(cabbird::MessageId::SettingsUpdateChannelHint),
                "stable preview nightly", [&] {
                    const auto channel = values.updates_channel;
                    const std::string stable = StableLabel(
                        cabbird::MessageId::SettingsStable, "channel-stable");
                    if (ImGui::RadioButton(
                            stable.c_str(), channel == cabbird::PlatformUpdateChannel::Stable)) {
                        values.updates_channel = cabbird::PlatformUpdateChannel::Stable;
                    }
                    ImGui::SameLine();
                    const std::string preview = StableLabel(
                        cabbird::MessageId::SettingsPreview, "channel-preview");
                    if (ImGui::RadioButton(
                            preview.c_str(), channel == cabbird::PlatformUpdateChannel::Preview)) {
                        values.updates_channel = cabbird::PlatformUpdateChannel::Preview;
                    }
                    ImGui::SameLine();
                    const std::string nightly = StableLabel(
                        cabbird::MessageId::SettingsNightly, "channel-nightly");
                    if (ImGui::RadioButton(
                            nightly.c_str(), channel == cabbird::PlatformUpdateChannel::Nightly)) {
                        values.updates_channel = cabbird::PlatformUpdateChannel::Nightly;
                    }
                });
            row("updates.automatic_check", Text(cabbird::MessageId::SettingsAutomaticUpdates),
                Text(cabbird::MessageId::SettingsAutomaticUpdatesHint),
                "repository network", [&] { cabbird::glass::Switch("##value", &values.updates_automatic_check); });
            row("updates.include_disabled", Text(cabbird::MessageId::SettingsIncludeDisabled),
                Text(cabbird::MessageId::SettingsIncludeDisabledHint),
                "repository profile", [&] { cabbird::glass::Switch("##value", &values.updates_include_disabled); });
            break;
        case SettingsSection::Diagnostics:
            row("diagnostics.log_level", Text(cabbird::MessageId::SettingsMinimumLogLevel),
                Text(cabbird::MessageId::SettingsLogLevelHint),
                "trace debug info warning error", [&] {
                    if (cabbird::glass::BeginCombo(
                            "##value", MinimumLogLevelLabel(values.diagnostics_log_level))) {
                        constexpr std::array<cabbird::PlatformMinimumLogLevel, 5> levels{
                            cabbird::PlatformMinimumLogLevel::Trace,
                            cabbird::PlatformMinimumLogLevel::Debug,
                            cabbird::PlatformMinimumLogLevel::Info,
                            cabbird::PlatformMinimumLogLevel::Warning,
                            cabbird::PlatformMinimumLogLevel::Error};
                        constexpr std::array<std::string_view, 5> ids{
                            "log-trace", "log-debug", "log-info", "log-warning", "log-error"};
                        for (std::size_t index = 0; index < levels.size(); ++index) {
                            const std::string label = cabbird::StableDisplayLabel(
                                MinimumLogLevelLabel(levels[index]), ids[index]);
                            if (ImGui::Selectable(label.c_str(),
                                    values.diagnostics_log_level == levels[index])) {
                                values.diagnostics_log_level = levels[index];
                            }
                        }
                        ImGui::EndCombo();
                    }
                });
            row("diagnostics.ring_capacity", Text(cabbird::MessageId::SettingsRingCapacity),
                Text(cabbird::MessageId::SettingsRingCapacityHint),
                "ring buffer capacity", [&] {
                    int value = static_cast<int>(values.diagnostics_ring_capacity);
                    if (ImGui::InputInt("##value", &value, 1000, 10000)) {
                        values.diagnostics_ring_capacity = static_cast<std::uint32_t>(
                            (std::max)(0, value));
                    }
                });
            break;
        case SettingsSection::Advanced:
            row("advanced.developer_mode", Text(cabbird::MessageId::SettingsDeveloperMode),
                Text(cabbird::MessageId::SettingsDeveloperModeHint),
                "services hooks memory unity", [&] {
                    cabbird::glass::Switch("##value", &values.advanced_developer_mode);
                });
            row("advanced.detailed_performance_diagnostics",
                Text(cabbird::MessageId::SettingsDetailedPerformanceDiagnostics),
                Text(cabbird::MessageId::SettingsDetailedPerformanceDiagnosticsHint),
                "performance timing render ui tick worker plugin", [&] {
                    cabbird::glass::Switch(
                        "##value", &values.advanced_detailed_performance_diagnostics);
                });
            break;
        case SettingsSection::About:
            if (SettingMatches(Text(cabbird::MessageId::SettingsRuntimeIdentity),
                    Text(cabbird::MessageId::SettingsBuildVersions),
                    "about version sdk profile github contact qq")) {
                heading();
                if (ImGui::BeginTable("##about", 2,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                            ImGuiTableFlags_SizingStretchProp)) {
                    const auto fact = [this](const char* label, const std::string& value) {
                        ImGui::TableNextRow(ImGuiTableRowFlags_None, 38.0f);
                        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", label);
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(
                            value.empty() ? Text(cabbird::MessageId::CommonUnavailable) : value.c_str());
                    };
                    fact(Text(cabbird::MessageId::SettingsRuntime),
                        model_.Snapshot().diagnostics.runtime_version);
                    fact(Text(cabbird::MessageId::SettingsSdkApi),
                        "V" + std::to_string(CABBIRD_PLUGIN_API_V1_MAJOR));
                    fact(Text(cabbird::MessageId::SettingsRepository),
                        DisplayRepositoryState(model_.Snapshot().repository.state));
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, 38.0f);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled(
                        "%s", Text(cabbird::MessageId::SettingsProjectRepository));
                    ImGui::TableNextColumn();
                    if (contact_ && ImGui::TextLink(contact_->repository_label.c_str())) {
                        pending_external_url_ = contact_->repository_url;
                    } else if (!contact_) {
                        ImGui::TextUnformatted(Text(cabbird::MessageId::CommonUnavailable));
                    }
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, 38.0f);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", Text(cabbird::MessageId::SettingsQqGroup));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(contact_
                            ? contact_->qq_group.c_str()
                            : Text(cabbird::MessageId::CommonUnavailable));
                    ImGui::EndTable();
                }
            }
            break;
        }
        return visible;
    }

    void QueueSettingsSave(const std::optional<Route> route_after_save = std::nullopt) {
        if (!settings_draft_ || settings_save_pending_) return;
            settings_validation_errors_ = cabbird::ValidatePlatformSettings(*settings_draft_);
        if (!settings_validation_errors_.empty()) return;
        cabbird::PlatformSettingsApplyRequest request;
        request.expected_revision = settings_base_revision_;
        request.values = *settings_draft_;
        pending_settings_apply_ = request;
        settings_save_pending_ = true;
        settings_route_after_save_ = route_after_save;
    }

    void DiscardSettingsDraft() {
        if (!settings_snapshot_.ready) return;
        settings_draft_ = settings_snapshot_.values;
        settings_scale_edit_percent_.reset();
        settings_base_revision_ = settings_snapshot_.revision;
        settings_apply_error_.clear();
        settings_validation_errors_.clear();
        settings_hotkey_capture_ = false;
        ApplySettingsPreview();
    }

    void DrawSettingsLeavePopup() {
        const std::string popup_title = StableLabel(
            cabbird::MessageId::SettingsLeaveTitle, "settings-leave-popup");
        if (settings_leave_popup_requested_) {
            ImGui::OpenPopup(popup_title.c_str());
            settings_leave_popup_requested_ = false;
        }
        if (!ImGui::BeginPopupModal(popup_title.c_str(), nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            return;
        }
        ImGui::TextUnformatted(Text(cabbird::MessageId::SettingsLeaveMessage));
        ImGui::Spacing();
        const std::string keep_editing = StableLabel(
            cabbird::MessageId::CommonKeepEditing, "settings-keep-editing");
        if (ImGui::Button(keep_editing.c_str())) {
            settings_pending_route_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        const std::string discard = StableLabel(
            cabbird::MessageId::CommonDiscard, "settings-discard-leave");
        if (ImGui::Button(discard.c_str())) {
            const auto route = settings_pending_route_;
            settings_pending_route_.reset();
            DiscardSettingsDraft();
            ImGui::CloseCurrentPopup();
            if (route) Navigate(*route);
        }
        ImGui::SameLine();
        const auto errors = settings_draft_
            ? cabbird::ValidatePlatformSettings(*settings_draft_)
            : std::vector<cabbird::PlatformSettingsValidationError>{};
        ImGui::BeginDisabled(!settings_pending_route_ || !errors.empty() || settings_save_pending_);
        const std::string save = StableLabel(settings_save_pending_
                ? cabbird::MessageId::CommonSavingEllipsis
                : cabbird::MessageId::CommonSave,
            "settings-save-leave");
        if (PrimaryButton(save.c_str())) {
            QueueSettingsSave(settings_pending_route_);
            settings_pending_route_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    void DrawSettingsShell() {
        const std::string subtitle = settings_snapshot_.ready
            ? std::string{}
            : std::string(Text(cabbird::MessageId::SettingsProviderUnavailable));
        DrawShellPageHeader(Text(cabbird::MessageId::ShellRouteSettings), subtitle,
            [](const ImVec2&, const ImVec2&) {});
        if (!settings_snapshot_.ready || !settings_draft_) {
            BeginShellBodyChild("PlatformSettingsRoute");
            ImGui::SetCursorPosY(
                std::max(Scaled(32.0f), ImGui::GetContentRegionAvail().y * 0.16f));
            ImGui::TextColored(WarningColor(), "!  %s",
                Text(cabbird::MessageId::SettingsUnavailable));
            ImGui::PushTextWrapPos(
                std::min(Scaled(620.0f), ImGui::GetContentRegionAvail().x));
            ImGui::TextDisabled("%s", settings_snapshot_.reason.empty()
                ? Text(cabbird::MessageId::SettingsFacadeUnavailable)
                : settings_snapshot_.reason.c_str());
            ImGui::PopTextWrapPos();
            const std::string open_diagnostics = StableLabel(
                cabbird::MessageId::PluginsOpenDiagnostics, "settings-open-diagnostics");
            if (ImGui::Button(open_diagnostics.c_str())) {
                Navigate(Route::Diagnostics);
                model_.State().diagnostics_tab = DiagnosticTab::Overview;
            }
            ImGui::Separator();
            bool developer_mode = developer_mode_;
            const std::string developer_session = StableLabel(
                cabbird::MessageId::SettingsEnableDeveloperSession,
                "settings-developer-session");
            if (cabbird::glass::Switch(developer_session.c_str(), &developer_mode)) {
                SetDeveloperMode(developer_mode);
            }
            EndShellBodyChild();
            return;
        }

        settings_validation_errors_ = cabbird::ValidatePlatformSettings(*settings_draft_);
        ImGui::BeginChild("PlatformSettingsRoute", ImVec2(0.0f, 0.0f), false,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::BeginChild("PlatformSettingsSearch", ImVec2(0.0f, Scaled(48.0f)), false,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetCursorPos(Scaled(12.0f, 6.0f));
        SetAvailableItemWidth(16.0f, 520.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Scaled(30.0f), FieldPaddingY()));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Scaled(10.0f));
        ImGui::InputTextWithHint("##settings-search", Text(cabbird::MessageId::SettingsSearchHint),
            settings_search_.data(), settings_search_.size());
        ImGui::PopStyleVar(2);
        ImGui::GetWindowDrawList()->AddText(
            FieldGlyphPosition(),
            ImGui::GetColorU32(ThemeColor(cabbird::CabbirdUiTheme().text_muted)),
            ShellGlyphText(ShellGlyph::Search));
        ImGui::EndChild();

        ImGui::BeginChild("PlatformSettingsWorkspace", ImVec2(0.0f, 0.0f), false,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        constexpr std::array<SettingsSection, 6> sections{
            SettingsSection::Interface, SettingsSection::Input, SettingsSection::Updates,
            SettingsSection::Diagnostics, SettingsSection::Advanced, SettingsSection::About};
        const float rail_width = Scaled(
            layout_mode_ == LayoutMode::Wide ? 200.0f : 180.0f);
        BeginCardChild("PlatformSettingsSections", ImVec2(rail_width, 0.0f), 8.0f);
        std::size_t section_index = static_cast<std::size_t>(
            std::ranges::find(sections, settings_section_) - sections.begin());
        DrawLensRail("settings-sections", sections.size(), section_index,
            [&](const std::size_t index) {
                return cabbird::StableDisplayLabel(SettingsSectionName(sections[index]),
                    std::to_string(static_cast<int>(sections[index])));
            }, 40.0f);
        if (section_index < sections.size()) settings_section_ = sections[section_index];
        ImGui::Dummy(ImVec2(0.0f, SettingsScrollClearance()));
        EndCardChild();
        ImGui::SameLine(0.0f, Scaled(10.0f));
        BeginCardChild("PlatformSettingsContent", ImVec2(0.0f, 0.0f), 16.0f, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scaled(12.0f, 10.0f));
        bool any_visible{};
        if (settings_search_[0] == '\0') {
            any_visible = DrawSettingsSection(settings_section_);
        } else {
            for (const auto section : sections) {
                const bool section_visible = DrawSettingsSection(section);
                if (section_visible) ImGui::Spacing();
                any_visible = any_visible || section_visible;
            }
        }
        if (!any_visible) {
            ImGui::SetCursorPosY(Scaled(28.0f));
            const std::array<std::string_view, 1> search_arguments{
                std::string_view(settings_search_.data())};
            const std::string no_match = Format(
                cabbird::MessageId::SettingsNoSearchMatch, search_arguments);
            ImGui::TextDisabled("%s", no_match.c_str());
            const std::string clear_search = StableLabel(
                cabbird::MessageId::CommonClearSearch, "settings-clear-search");
            if (ImGui::Button(clear_search.c_str())) settings_search_.fill('\0');
        }
        ImGui::Dummy(ImVec2(0.0f, SettingsScrollClearance()));
        ImGui::PopStyleVar();
        EndCardChild();
        ImGui::EndChild();

        ImGui::EndChild();
    }

    // Unsaved settings float as a glass bar in the bottom-right corner, beside
    // the tab bar, so the page never changes height while it is edited. In a
    // window too narrow for both it rises above the tab bar.
    void DrawSettingsDraftBar() {
        if (model_.State().route != Route::Settings || !settings_snapshot_.ready ||
            !settings_draft_) {
            return;
        }
        if (!SettingsDirty() && !settings_save_pending_ && settings_apply_error_.empty()) return;
        const auto& theme = cabbird::CabbirdUiTheme();
        std::string status;
        ImVec4 status_ink = ThemeColor(theme.text_muted);
        if (!settings_apply_error_.empty()) {
            status = settings_apply_error_;
            status_ink = ErrorColor();
        } else if (!settings_validation_errors_.empty()) {
            const std::string error_count = std::to_string(settings_validation_errors_.size());
            const std::array<std::string_view, 1> error_arguments{error_count};
            status = Format(cabbird::MessageId::SettingsValidationErrors, error_arguments);
            status_ink = ErrorColor();
        } else {
            status = Text(settings_save_pending_
                ? cabbird::MessageId::SettingsSavingChanges
                : cabbird::MessageId::SettingsUnsavedChanges);
        }
        const float button_height = Scaled(36.0f);
        const float inset = Scaled(kTabBarInset + 2.0f);
        const float discard_width = Scaled(84.0f);
        const float save_width = Scaled(84.0f);
        const float gap = Scaled(8.0f);
        const float text_padding = Scaled(16.0f);
        status = Ellipsize(status, Scaled(320.0f));
        const float text_width = ImGui::CalcTextSize(status.c_str()).x;
        const float bar_width = text_padding + text_width + text_padding +
            discard_width + gap + save_width + inset;
        const float bar_height = button_height + inset * 2.0f;
        const ImVec2 window_position = ImGui::GetWindowPos();
        const ImVec2 window_size = ImGui::GetWindowSize();
        ImVec2 bar_min(
            std::round(window_position.x + window_size.x - Scaled(14.0f) - bar_width),
            std::round(tab_bar_rect_.GetCenter().y - bar_height * 0.5f));
        if (tab_bar_rect_.GetWidth() <= 0.0f || bar_min.x < tab_bar_rect_.Max.x + Scaled(12.0f)) {
            bar_min.y = std::round((tab_bar_rect_.GetWidth() > 0.0f ? tab_bar_rect_.Min.y
                : window_position.y + window_size.y) - Scaled(10.0f) - bar_height);
        }
        ImGui::SetCursorScreenPos(bar_min);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("PlatformSettingsDraftBar", ImVec2(bar_width, bar_height), false,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        ImDrawList* const list = ImGui::GetWindowDrawList();
        cabbird::glass::CaptureBackdrop(list);
        cabbird::glass::SurfaceOptions surface;
        surface.variant = cabbird::glass::Variant::Regular;
        surface.rounding = button_height * kTabBarRounding + inset;
        surface.scale = PlatformUiScale();
        surface.shadow = false;
        surface.tint_strength = cabbird::glass::DarkAppearance() ? 0.30f : 0.16f;
        cabbird::glass::DrawSurface(list, bar_min, Offset(bar_min, bar_width, bar_height), surface);
        list->AddText(Offset(bar_min, text_padding,
                (bar_height - ImGui::GetTextLineHeight()) * 0.5f),
            ImGui::GetColorU32(status_ink), status.c_str());

        ImGui::SetCursorScreenPos(Offset(bar_min,
            bar_width - inset - save_width - gap - discard_width, inset));
        ImGui::BeginDisabled(settings_save_pending_);
        const std::string discard = StableLabel(
            cabbird::MessageId::CommonDiscard, "settings-discard");
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, button_height * kTabBarRounding);
        ImGui::PushStyleColor(ImGuiCol_Button, InkColor(0.085f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, InkColor(0.085f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, InkColor(0.14f));
        const bool discard_pressed =
            ImGui::Button(discard.c_str(), ImVec2(discard_width, button_height));
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        if (discard_pressed) DiscardSettingsDraft();
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, gap);
        const bool stale = settings_base_revision_ != settings_snapshot_.revision;
        ImGui::BeginDisabled(
            settings_save_pending_ || !settings_validation_errors_.empty() || stale);
        const std::string save = StableLabel(settings_save_pending_
                ? cabbird::MessageId::CommonSaving
                : cabbird::MessageId::CommonSave,
            "settings-save");
        if (PrimaryButton(save.c_str(), ImVec2(save_width, button_height))) {
            QueueSettingsSave();
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
    }

    void UpdateStatusToast() {
        if (status_.empty() || status_ == last_toast_status_) return;
        last_toast_status_ = status_;
        toast_status_ = status_;
        toast_failure_ = status_failure_;
        toast_expires_at_ = ImGui::GetTime() + 3.2;
    }

    void DrawStatusToast(const ImVec2 origin, const ImVec2 size) {
        if (toast_status_.empty() || ImGui::GetTime() >= toast_expires_at_) {
            toast_status_.clear();
            return;
        }
        const float text_width = ImGui::CalcTextSize(toast_status_.c_str()).x;
        const float minimum_width = Scaled(200.0f);
        const float width = std::min(std::max(minimum_width, text_width + Scaled(64.0f)),
            std::max(minimum_width, size.x - Scaled(28.0f)));
        const float height = Scaled(kPlatformToastHeight + 4.0f);
        const float margin = Scaled(kPlatformTabBarReserve + 2.0f);
        // The toast rises into place and fades out over its last quarter second.
        const double now = ImGui::GetTime();
        const float age = static_cast<float>(now - (toast_expires_at_ - 3.2));
        const float remaining = static_cast<float>(toast_expires_at_ - now);
        const bool motion = !cabbird::glass::ReduceMotion();
        const float enter = motion ? std::clamp(age / 0.22f, 0.0f, 1.0f) : 1.0f;
        const float eased = 1.0f - (1.0f - enter) * (1.0f - enter) * (1.0f - enter);
        const float fade = std::clamp(remaining / 0.25f, 0.0f, 1.0f) *
            (motion ? std::clamp(age / 0.12f, 0.0f, 1.0f) : 1.0f);
        const ImVec2 position = Offset(origin, (size.x - width) * 0.5f,
            size.y - height - margin + (1.0f - eased) * Scaled(16.0f));
        const ImVec4 color = toast_failure_ ? ErrorColor() : SuccessColor();
        const auto& theme = cabbird::CabbirdUiTheme();
        ImDrawList* const draw_list = ImGui::GetForegroundDrawList();
        draw_list->PushClipRect(origin, Offset(origin, size.x, size.y), true);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * fade);
        cabbird::glass::CaptureBackdrop(draw_list);
        DrawGlass(draw_list, position, Offset(position, width, height),
            cabbird::glass::Variant::Regular, (kPlatformToastHeight + 4.0f) * kTabBarRounding,
            true, false);
        const float text_y = (height - ImGui::GetTextLineHeight()) * 0.5f;
        draw_list->AddCircleFilled(Offset(position, Scaled(22.0f), height * 0.5f), Scaled(9.0f),
            ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, 0.20f)));
        const char* const glyph =
            ShellGlyphText(toast_failure_ ? ShellGlyph::Warning : ShellGlyph::Check);
        const ImVec2 glyph_size = ImGui::CalcTextSize(glyph);
        draw_list->AddText(Offset(position, Scaled(22.0f) - glyph_size.x * 0.5f,
                height * 0.5f - glyph_size.y * 0.5f),
            ImGui::GetColorU32(color), glyph);
        draw_list->AddText(Offset(position, Scaled(42.0f), text_y),
            ImGui::GetColorU32(ThemeColor(theme.text)),
            Ellipsize(toast_status_, width - Scaled(56.0f)).c_str());
        ImGui::PopStyleVar();
        draw_list->PopClipRect();
    }

    void RequestOperationDetailsPopup() noexcept {
        operation_details_open_ = true;
        operation_details_popup_requested_ = true;
    }

    void DrawOperationDetailsPopup() {
        if (!operation_details_open_) return;
        const std::string popup_title = StableLabel(
            cabbird::MessageId::OperationDetails, "operation-details-popup");
        if (operation_details_popup_requested_) {
            ImGui::OpenPopup(popup_title.c_str());
            operation_details_popup_requested_ = false;
        }
        const ImVec2 host_size = ImGui::GetWindowSize();
        const float modal_width = std::min(640.0f, std::max(320.0f, host_size.x - 48.0f));
        const float modal_height = std::min(520.0f, std::max(220.0f, host_size.y - 48.0f));
        ImGui::SetNextWindowSize(ImVec2(modal_width, modal_height), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(popup_title.c_str(), &operation_details_open_,
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            operation_details_open_ = false;
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        const auto* operation = PresentedOperation();
        ImGui::SetWindowFontScale(1.12f);
        ImGui::TextUnformatted(Text(cabbird::MessageId::OperationDetails));
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("%s", operation == nullptr
            ? Text(cabbird::MessageId::OperationNoneSelected)
            : OperationDisplayLabel(*operation).c_str());
        if (operation != nullptr && !operation->message.empty()) {
            ImGui::TextWrapped("%s", operation->message.c_str());
        }
        ImGui::Separator();
        if (operation != nullptr && ImGui::BeginTable("OperationDetails", 3,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                    ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                    ImGuiTableFlags_SizingStretchProp,
                ImVec2(0.0f, std::max(80.0f,
                    std::min(250.0f, ImGui::GetContentRegionAvail().y - 48.0f))))) {
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonPlugin),
                ImGuiTableColumnFlags_WidthStretch, 0.38f);
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonResult),
                ImGuiTableColumnFlags_WidthFixed, 92.0f);
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonReason),
                ImGuiTableColumnFlags_WidthStretch, 0.62f);
            ImGui::TableHeadersRow();
            for (const auto& plugin : operation->plugins) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(plugin.plugin_id.c_str());
                ImGui::TableNextColumn();
                const bool failure = plugin.outcome == cabbird::PluginOperationOutcome::Failed;
                ImGui::TextColored(failure ? ErrorColor() : SuccessColor(), "%s",
                    Text(plugin.outcome == cabbird::PluginOperationOutcome::Succeeded
                        ? cabbird::MessageId::CommonSucceeded
                        : failure ? cabbird::MessageId::CommonFailed
                                  : cabbird::MessageId::CommonSkipped));
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", plugin.message.empty()
                    ? cabbird::ToString(plugin.reason).data() : plugin.message.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
        const std::string close = StableLabel(
            cabbird::MessageId::CommonClose, "operation-details-close");
        const ImVec2 close_size = ImGui::CalcTextSize(close.c_str());
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
            ImGui::GetWindowContentRegionMax().x - close_size.x - ImGui::GetStyle().FramePadding.x * 2.0f));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button(close.c_str())) {
            operation_details_open_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void DrawServices() {
        ImGui::TextUnformatted(Text(cabbird::MessageId::DeveloperServiceGraph));
        if (!diagnostics_.service_graph) {
            ImGui::TextDisabled("%s",
                Text(cabbird::MessageId::DeveloperServiceProviderUnavailable));
            return;
        }
        const auto graph = diagnostics_.service_graph();
        const std::array<std::string, 4> summary_values{
            graph.built ? "1" : "0",
            graph.blocking_startup_complete ? "1" : "0",
            graph.async_startup_complete ? "1" : "0",
            std::to_string(graph.error)};
        const std::array<std::string_view, 4> summary_arguments{
            summary_values[0], summary_values[1], summary_values[2], summary_values[3]};
        ImGui::TextDisabled("%s", Format(
            cabbird::MessageId::DeveloperServiceSummary, summary_arguments).c_str());
        if (ImGui::BeginTable("DeveloperServices", 7,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonService));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonState));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::DeveloperServiceAffinity));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonVersion));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::DeveloperServiceStartMs));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::DeveloperServiceStartThread));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::DeveloperServiceStopThread));
            ImGui::TableHeadersRow();
            for (const auto& service : graph.services) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(service.id.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ServiceStateName(service.state));
                ImGui::TableNextColumn(); ImGui::Text("%u", static_cast<unsigned>(service.affinity));
                ImGui::TableNextColumn(); ImGui::Text("%u", service.version);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", service.startup_duration.count() / 1000.0);
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(service.start_thread_id));
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(service.stop_thread_id));
            }
            ImGui::EndTable();
        }
    }

    void DrawHooks() {
        ImGui::TextUnformatted(Text(cabbird::MessageId::DeveloperHooksTitle));
        const auto hooks = diagnostics_.hooks ? diagnostics_.hooks() : std::vector<cabbird::HookRecordView>{};
        if (hooks.empty()) {
            ImGui::TextDisabled("%s", Text(cabbird::MessageId::DeveloperHooksNone));
            return;
        }
        if (ImGui::BeginTable("DeveloperHooks", 5,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonOwner));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonLabel));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonGeneration));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonTarget));
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonState));
            ImGui::TableHeadersRow();
            for (const auto& hook : hooks) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(hook.owner.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(hook.label.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(hook.generation));
                ImGui::TableNextColumn(); ImGui::Text("0x%llX", reinterpret_cast<unsigned long long>(hook.target));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(Text(hook.enabled
                    ? cabbird::MessageId::CommonEnabled
                    : cabbird::MessageId::CommonDisabled));
            }
            ImGui::EndTable();
        }
    }

    void DrawMemory() {
        ImGui::TextUnformatted(Text(cabbird::MessageId::DeveloperMemoryTitle));
        ImGui::TextDisabled("%s", Text(cabbird::MessageId::DeveloperMemoryWarning));
        ImGui::Separator();
        ImGui::SetNextItemWidth(230.0f);
        ImGui::InputTextWithHint("##memory-address",
            Text(cabbird::MessageId::DeveloperMemoryAddressHint),
            address_.data(), address_.size());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        const std::string bytes_label = StableLabel(
            cabbird::MessageId::CommonBytes, "memory-read-bytes");
        ImGui::InputInt(bytes_label.c_str(), &read_size_, 16, 64);
        read_size_ = std::clamp(read_size_, 1, 4096);
        ImGui::SameLine();
        const std::string read = StableLabel(
            cabbird::MessageId::CommonRead, "memory-read");
        if (ImGui::Button(read.c_str())) read_requested_ = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", memory_status_.c_str());
        ImGui::BeginChild("MemoryHex", ImVec2(0, 220), ImGuiChildFlags_Borders);
        for (std::size_t row = 0; row < bytes_.size(); row += 16) {
            ImGui::TextDisabled("%08llX", static_cast<unsigned long long>(last_address_ + row));
            ImGui::SameLine(100.0f);
            std::string hex;
            std::string ascii;
            for (std::size_t column = 0; column < 16; ++column) {
                if (row + column < bytes_.size()) {
                    char byte[4]{};
                    std::snprintf(byte, sizeof(byte), "%02X ", bytes_[row + column]);
                    hex += byte;
                    const auto value = bytes_[row + column];
                    ascii += value >= 32 && value < 127 ? static_cast<char>(value) : '.';
                } else {
                    hex += "   ";
                }
            }
            ImGui::TextUnformatted(hex.c_str());
            ImGui::SameLine(520.0f);
            ImGui::TextDisabled("%s", ascii.c_str());
        }
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::TextDisabled("%s", Text(cabbird::MessageId::DeveloperMemoryWrite));
        SetAvailableItemWidth(220.0f);
        ImGui::InputTextWithHint("##memory-write",
            Text(cabbird::MessageId::DeveloperMemoryHexHint),
            write_bytes_.data(), write_bytes_.size());
        ImGui::SameLine();
        const std::string patch_label = StableLabel(
            cabbird::MessageId::DeveloperMemoryPatchProtection, "memory-patch-protection");
        cabbird::glass::Switch(patch_label.c_str(), &patch_);
        ImGui::SameLine();
        const std::string apply = StableLabel(
            cabbird::MessageId::CommonApply, "memory-apply");
        if (ImGui::Button(apply.c_str())) RequestMemoryWrite();
    }

    void SetDeveloperMode(const bool enabled) noexcept {
        if (developer_mode_ == enabled) return;
        developer_mode_ = enabled;
        cabbird::SetHostUiDeveloperMode(enabled);
        cabbird::ReconcileUiStateSelection(
            model_.Snapshot(), model_.State(), developer_mode_);
    }

    bool HasPendingMutation(std::string_view subject, Mutation mutation) const {
        for (const auto& operation : model_.PendingOperations()) {
            if (!subject.empty() && operation.subject_id != subject &&
                std::none_of(operation.plugins.begin(), operation.plugins.end(),
                    [&](const auto& plugin) { return plugin.plugin_id == subject; })) {
                continue;
            }
            if (mutation != Mutation::None && operation.mutation != mutation) continue;
            if (subject.empty() && mutation == Mutation::None) continue;
            return true;
        }
        return false;
    }

    void Navigate(Route route) {
        if (route == Route::UnityCompatibility) route = Route::Diagnostics;
        if (model_.State().route == Route::Settings && route != Route::Settings &&
            SettingsDirty()) {
            settings_pending_route_ = route;
            settings_leave_popup_requested_ = true;
            return;
        }
        model_.State().route = route;
        if (route != Route::Settings) settings_hotkey_capture_ = false;
        if (diagnostics_.settings_record_route) {
            settings_route_to_record_ = std::string(cabbird::ToString(route));
        }
        if (route != Route::Plugins) compact_plugin_detail_pending_id_.clear();
        status_.clear();
        status_failure_ = false;
    }

    void OpenPluginLogs(std::string_view plugin_id) {
        model_.State().route = Route::Diagnostics;
        model_.State().diagnostics_tab = DiagnosticTab::Logs;
        model_.State().diagnostics_plugin_id = std::string(plugin_id);
        status_ = Text(cabbird::MessageId::OperationLogsFiltered);
        status_failure_ = false;
    }

    void SubmitIntent(Intent intent, bool confirmed_submission = false) {
        const auto submission = model_.Submit(intent);
        if (!submission.accepted) {
            if (submission.code == cabbird::PlatformUiResultCode::PreflightRequired &&
                submission.preflight && !confirmed_submission) {
                confirmation_intent_ = std::move(intent);
                confirmation_plan_ = *submission.preflight;
                confirmation_popup_requested_ = true;
                return;
            }
            status_ = submission.reason.empty()
                ? std::string(cabbird::ToString(submission.code)) : submission.reason;
            status_failure_ = true;
            return;
        }
        status_ = Text(cabbird::MessageId::OperationSubmitted);
        status_failure_ = false;
        QueuedIntent queued{std::move(intent), submission.operation_id};
        if (submission.preflight) {
            queued.affected = submission.preflight->affected;
        }
        if (queued.affected.empty() && !queued.intent.subject_id.empty()) {
            queued.affected.push_back({queued.intent.subject_id, true, "requested object"});
        }
        for (const auto& affected : queued.affected) {
            const auto* plugin = model_.Snapshot().FindPlugin(affected.id);
            queued.generations.emplace(
                affected.id, plugin == nullptr ? 0 : plugin->generation);
        }
        queued.execution_state = std::make_shared<std::atomic<IntentExecutionState>>(
            IntentExecutionState::Pending);
        queued_intents_.push_back(std::move(queued));
    }

    void DismissConfirmation(const char* status) {
        confirmation_intent_.reset();
        confirmation_plan_ = {};
        confirmation_popup_requested_ = false;
        if (status != nullptr) {
            status_ = status;
            status_failure_ = false;
        }
    }

    void DrawConfirmationPopup() {
        const std::string popup_title = StableLabel(
            cabbird::MessageId::OperationConfirmTitle, "confirm-plugin-operation");
        if (confirmation_popup_requested_) {
            ImGui::OpenPopup(popup_title.c_str());
            confirmation_popup_requested_ = false;
        }
        if (!confirmation_intent_) return;
        const ImVec2 host_size = ImGui::GetWindowSize();
        const float modal_width = std::min(640.0f, std::max(320.0f, host_size.x - 48.0f));
        const float table_height = std::clamp(
            34.0f + static_cast<float>(confirmation_plan_.affected.size()) * 31.0f,
            66.0f, 252.0f);
        const float requested_height = std::min(
            520.0f, std::max(210.0f, 126.0f + table_height));
        const float modal_height = std::min(
            requested_height, std::max(220.0f, host_size.y - 48.0f));
        ImGui::SetNextWindowSize(ImVec2(modal_width, modal_height), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(popup_title.c_str(), nullptr,
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) return;

        const auto action_verb = [this](const Mutation mutation) -> const char* {
            switch (mutation) {
            case Mutation::Start: return Text(cabbird::MessageId::CommonStart);
            case Mutation::Stop: return Text(cabbird::MessageId::CommonStop);
            case Mutation::Reload:
            case Mutation::ReloadAll: return Text(cabbird::MessageId::CommonReload);
            case Mutation::Enable: return Text(cabbird::MessageId::CommonEnable);
            case Mutation::Disable: return Text(cabbird::MessageId::CommonDisable);
            case Mutation::SetVisible: return Text(cabbird::MessageId::CommonUpdate);
            case Mutation::None: return Text(cabbird::MessageId::CommonConfirm);
            }
            return Text(cabbird::MessageId::CommonConfirm);
        };
        const Mutation mutation = confirmation_plan_.mutation;
        const std::size_t affected_count = confirmation_plan_.affected.size();
        std::string title{Text(cabbird::MessageId::OperationConfirmTitle)};
        std::string consequence;
        if (mutation == Mutation::ReloadAll) {
            title = Text(cabbird::MessageId::OperationReloadAllTitle);
            consequence = Text(cabbird::MessageId::OperationReloadAllConsequence);
        } else if (!confirmation_plan_.reason.empty()) {
            consequence = confirmation_plan_.reason;
        } else {
            consequence = Text(cabbird::MessageId::OperationStateConsequence);
        }
        const std::string affected_count_text = std::to_string(affected_count);
        const std::array<std::string_view, 2> action_arguments{
            action_verb(mutation), affected_count_text};
        const std::array<std::string_view, 1> single_action_argument{
            action_verb(mutation)};
        const std::string action_label = affected_count == 1
            ? Format(cabbird::MessageId::OperationAffectedOne, single_action_argument)
            : Format(cabbird::MessageId::OperationAffectedMany, action_arguments);
        const std::string action_button_label = action_label + "###confirm-operation-action";

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            DismissConfirmation(Text(cabbird::MessageId::OperationCancelled));
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        ImGui::SetWindowFontScale(1.15f);
        ImGui::TextUnformatted(title.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("%s", consequence.c_str());
        ImGui::Separator();
        if (ImGui::BeginTable("ConfirmationImpact", 3,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                    ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                    ImGuiTableFlags_SizingStretchProp,
                ImVec2(0.0f, table_height))) {
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonPlugin),
                ImGuiTableColumnFlags_WidthStretch, 0.36f);
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonScope),
                ImGuiTableColumnFlags_WidthFixed, 96.0f);
            ImGui::TableSetupColumn(Text(cabbird::MessageId::CommonWhy),
                ImGuiTableColumnFlags_WidthStretch, 0.64f);
            ImGui::TableHeadersRow();
            for (const auto& plugin : confirmation_plan_.affected) {
                const char* scope = Text(plugin.directly_requested
                    ? cabbird::MessageId::CommonRequested
                    : cabbird::MessageId::CommonDependency);
                const char* fallback_reason = Text(plugin.directly_requested
                    ? cabbird::MessageId::OperationSelectedByAction
                    : cabbird::MessageId::OperationRequiredByPlugin);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(plugin.id.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(scope);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", plugin.reason.empty()
                    ? fallback_reason : plugin.reason.c_str());
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        const std::string cancel_label = StableLabel(
            cabbird::MessageId::CommonCancel, "confirm-operation-cancel");
        const ImVec2 cancel_text_size = ImGui::CalcTextSize(cancel_label.c_str());
        const ImVec2 action_text_size = ImGui::CalcTextSize(action_button_label.c_str());
        const float cancel_width = cancel_text_size.x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float action_width = action_text_size.x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float footer_width = cancel_width + ImGui::GetStyle().ItemSpacing.x + action_width;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
            ImGui::GetWindowContentRegionMax().x - footer_width));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool cancel = ImGui::Button(cancel_label.c_str(), ImVec2(cancel_width, 0.0f));
        ImGui::SameLine();
        const bool confirm = PrimaryButton(
            action_button_label.c_str(), ImVec2(action_width, 0.0f));
        if (confirm) {
            Intent intent = std::move(*confirmation_intent_);
            intent.expected_revision = model_.Snapshot().revision;
            intent.confirmed = true;
            DismissConfirmation(nullptr);
            ImGui::CloseCurrentPopup();
            SubmitIntent(std::move(intent), true);
        }
        if (cancel) {
            DismissConfirmation(Text(cabbird::MessageId::OperationCancelled));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void DrawRepositoryUninstallPopup() {
        const std::string popup_title = StableLabel(
            cabbird::MessageId::PluginsUninstallTitle, "repository-uninstall-popup");
        if (repository_uninstall_popup_requested_) {
            ImGui::OpenPopup(popup_title.c_str());
            repository_uninstall_popup_requested_ = false;
        }
        if (!ImGui::BeginPopupModal(popup_title.c_str(), nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            return;
        }
        if (repository_uninstall_plugin_id_.empty()) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            repository_uninstall_plugin_id_.clear();
            repository_uninstall_plugin_name_.clear();
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        const std::array<std::string_view, 1> arguments{repository_uninstall_plugin_name_};
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 420.0f);
        ImGui::TextWrapped("%s", Format(
            cabbird::MessageId::PluginsUninstallBody, arguments).c_str());
        ImGui::TextDisabled("%s", Text(cabbird::MessageId::PluginsUninstallDataKept));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();

        const std::string cancel = StableLabel(
            cabbird::MessageId::CommonCancel, "repository-uninstall-cancel");
        const std::string uninstall = StableLabel(
            cabbird::MessageId::CommonUninstall, "repository-uninstall-confirm");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button(cancel.c_str())) {
            repository_uninstall_plugin_id_.clear();
            repository_uninstall_plugin_name_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (DestructiveButton(uninstall.c_str())) {
            ImGui::CloseCurrentPopup();
            SubmitRepositoryUninstall();
        }
        ImGui::EndPopup();
    }

    void RequestMemoryWrite() {
        const auto address = ParseAddress(address_.data());
        const auto bytes = ParseBytes(write_bytes_.data());
        if (!address || !bytes) {
            memory_status_ = Text(cabbird::MessageId::DeveloperMemoryInvalidInput);
            return;
        }
        pending_memory_write_ = PendingMemoryWrite{*address, std::move(*bytes), patch_};
        memory_confirmation_popup_requested_ = true;
    }

    void DrawMemoryConfirmationPopup() {
        const std::string popup_title = StableLabel(
            cabbird::MessageId::DeveloperMemoryConfirmTitle, "confirm-memory-write");
        if (memory_confirmation_popup_requested_) {
            ImGui::OpenPopup(popup_title.c_str());
            memory_confirmation_popup_requested_ = false;
        }
        if (!ImGui::BeginPopupModal(popup_title.c_str(), nullptr,
                ImGuiWindowFlags_AlwaysAutoResize)) return;
        if (!pending_memory_write_) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        const auto& write = *pending_memory_write_;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            pending_memory_write_.reset();
            memory_status_ = Text(cabbird::MessageId::DeveloperMemoryWriteCancelled);
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        const std::string byte_count = std::to_string(write.bytes.size());
        std::array<char, 32> address_text{};
        std::snprintf(address_text.data(), address_text.size(), "0x%llX",
            static_cast<unsigned long long>(write.address));
        const std::array<std::string_view, 2> write_arguments{
            byte_count, address_text.data()};
        ImGui::TextWrapped("%s", Format(
            cabbird::MessageId::DeveloperMemoryApplyBytes, write_arguments).c_str());
        const std::array<std::string_view, 1> mode_arguments{Text(write.patch
            ? cabbird::MessageId::DeveloperMemoryTrackedPatch
            : cabbird::MessageId::DeveloperMemoryRawWrite)};
        ImGui::Text("%s", Format(
            cabbird::MessageId::DeveloperMemoryMode, mode_arguments).c_str());
        ImGui::TextDisabled("%s", Text(cabbird::MessageId::DeveloperMemoryIrrecoverable));
        const std::string confirm_write = StableLabel(
            cabbird::MessageId::DeveloperMemoryConfirmWrite, "memory-confirm-write");
        if (ImGui::Button(confirm_write.c_str())) {
            memory_write_requested_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        const std::string cancel_write = StableLabel(
            cabbird::MessageId::DeveloperMemoryCancelWrite, "memory-cancel-write");
        if (ImGui::Button(cancel_write.c_str())) {
            pending_memory_write_.reset();
            memory_status_ = Text(cabbird::MessageId::DeveloperMemoryWriteCancelled);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void FlushIntents() {
        std::vector<QueuedIntent> queued;
        {
            std::scoped_lock operation_lock(operation_mutex_);
            queued.swap(queued_intents_);
        }
        for (auto& item : queued) {
            auto pending = std::make_shared<QueuedIntent>(std::move(item));
            try {
                const auto invocation = BeginInvocation();
                if (invocation == nullptr) {
                    static_cast<void>(ApplyDispatcherFailure(*pending, ERROR_CANCELLED));
                    SetStatus(cabbird::MessageId::OperationLifecycleRejected, true);
                    continue;
                }
                const auto self = invocation->owner;
                invocation->on_abandon = [self, pending] {
                    static_cast<void>(self->ApplyIntentFailure(
                        *pending, IntentExecutionState::Pending,
                        cabbird::PlatformUiResultCode::ProviderUnavailable,
                        cabbird::PluginOperationReason::ProviderUnavailable,
                        "lifecycle dispatcher cancelled operation before callback start", true));
                };
                const auto submit = diagnostics_.lifecycle_post
                    ? diagnostics_.lifecycle_post
                    : diagnostics_.lifecycle_invoke;
                if (submit) {
                    const auto result = submit(
                        [self, pending, invocation]() mutable {
                            invocation->MarkCallbackStarted();
                            self->ExecuteIntentCallback(pending);
                        });
                    if (result != ERROR_SUCCESS) {
                        if (ApplyDispatcherFailure(*pending, result)) {
                            SetStatus(cabbird::MessageId::OperationLifecycleRejected, true);
                        } else {
                            SetStatus(cabbird::MessageId::OperationStillSettling, false);
                        }
                    }
                } else {
                    invocation->MarkCallbackStarted();
                    ExecuteIntentCallback(pending);
                }
            } catch (...) {
                if (ApplyIntentFailure(
                        *pending, IntentExecutionState::Pending,
                        cabbird::PlatformUiResultCode::BackendFailure,
                        cabbird::PluginOperationReason::BackendFailure,
                        "lifecycle invocation raised an exception", true)) {
                    SetStatus(cabbird::MessageId::OperationFailedDiagnostics, true);
                } else {
                    SetStatus(cabbird::MessageId::OperationStillSettling, false);
                }
            }
        }
    }

    bool ApplyIntentFailure(
        const QueuedIntent& queued,
        IntentExecutionState expected_state,
        cabbird::PlatformUiResultCode code,
        cabbird::PluginOperationReason reason,
        std::string message,
        bool retryable) {
        if (!TrySettleIntent(queued, expected_state)) return false;
        std::scoped_lock operation_lock(operation_mutex_);
        cabbird::PlatformUiOperationResult result;
        result.operation_id = queued.operation_id;
        result.intent_id = queued.intent.intent_id;
        result.expected_revision = queued.intent.expected_revision;
        result.observed_revision = model_.Snapshot().revision;
        result.mutation = cabbird::ResolvePlatformUiMutation(queued.intent);
        result.subject_id = queued.intent.subject_id;
        result.message = std::move(message);
        result.retryable = retryable;
        for (const auto& affected : queued.affected) {
            const auto generation = queued.generations.contains(affected.id)
                ? queued.generations.at(affected.id) : 0;
            result.plugins.push_back({affected.id,
                affected.directly_requested
                    ? cabbird::PluginOperationOutcome::Failed
                    : cabbird::PluginOperationOutcome::Skipped,
                reason, result.message, generation, generation,
                retryable});
        }
        if (result.plugins.empty()) {
            result.plugins.push_back({queued.intent.subject_id,
                cabbird::PluginOperationOutcome::Failed,
                reason, result.message, 0, 0, retryable});
        }
        result.state = cabbird::PlatformUiOperationState::Failed;
        result.code = code;
        model_.ApplyOperationResult(std::move(result));
        return true;
    }

    bool ApplyDispatcherFailure(
        const QueuedIntent& queued, std::uint32_t error) {
        const bool unavailable = error == ERROR_NOT_READY || error == ERROR_CANCELLED;
        return ApplyIntentFailure(
            queued, IntentExecutionState::Pending,
            unavailable ? cabbird::PlatformUiResultCode::ProviderUnavailable
                        : cabbird::PlatformUiResultCode::BackendFailure,
            unavailable ? cabbird::PluginOperationReason::ProviderUnavailable
                        : cabbird::PluginOperationReason::BackendFailure,
            "lifecycle dispatcher rejected operation: " + std::to_string(error), true);
    }

    void FlushSettingsActions() {
        std::optional<cabbird::PlatformSettingsApplyRequest> apply;
        std::optional<cabbird::PluginRepositoryConfig> repository_configure;
        std::optional<std::string> route;
        std::optional<std::string> external_url;
        {
            std::scoped_lock operation_lock(operation_mutex_);
            apply.swap(pending_settings_apply_);
            repository_configure.swap(pending_repository_configure_);
            route.swap(settings_route_to_record_);
            external_url.swap(pending_external_url_);
        }
        if (!external_url) external_url = cabbird::ConsumeHostUiExternalUrlRequest();
        const auto submit = diagnostics_.lifecycle_post
            ? diagnostics_.lifecycle_post
            : diagnostics_.lifecycle_invoke;
        if (apply) {
            const auto mailbox = settings_apply_mailbox_;
            const auto provider = diagnostics_.settings_apply;
            const auto operation = [mailbox, provider, request = *apply] {
                cabbird::PlatformSettingsApplyResult result;
                if (provider) {
                    result = provider(request);
                } else {
                    result.code = cabbird::PlatformSettingsApplyCode::ProviderUnavailable;
                    result.message = "settings apply provider is unavailable";
                }
                std::scoped_lock lock(mailbox->mutex);
                mailbox->result = std::move(result);
            };
            std::uint32_t error = ERROR_SUCCESS;
            if (submit) error = submit(operation);
            else operation();
            if (error != ERROR_SUCCESS) {
                cabbird::PlatformSettingsApplyResult result;
                result.code = cabbird::PlatformSettingsApplyCode::ProviderUnavailable;
                result.message = "lifecycle dispatcher rejected settings save: " +
                    std::to_string(error);
                std::scoped_lock lock(mailbox->mutex);
                mailbox->result = std::move(result);
            }
        }
        if (repository_configure) {
            const auto mailbox = repository_configure_mailbox_;
            const auto provider = diagnostics_.repository_configure;
            const auto operation = [mailbox, provider, config = *repository_configure] {
                cabbird::RepositoryOperationSubmission submission;
                if (provider) {
                    submission = provider(config);
                } else {
                    submission.message = "repository configuration provider is unavailable";
                }
                std::scoped_lock lock(mailbox->mutex);
                mailbox->result = RepositoryConfigureResult{config, std::move(submission)};
            };
            std::uint32_t error = ERROR_SUCCESS;
            if (submit) error = submit(operation);
            else operation();
            if (error != ERROR_SUCCESS) {
                cabbird::RepositoryOperationSubmission submission;
                submission.message = "lifecycle dispatcher rejected repository configuration: " +
                    std::to_string(error);
                std::scoped_lock lock(mailbox->mutex);
                mailbox->result = RepositoryConfigureResult{
                    *repository_configure, std::move(submission)};
            }
        }
        if (route && diagnostics_.settings_record_route) {
            const auto provider = diagnostics_.settings_record_route;
            const auto operation = [provider, route = *route] {
                static_cast<void>(provider(route));
            };
            if (submit) static_cast<void>(submit(operation));
            else operation();
        }
        if (external_url) {
            const auto operation = [url = std::move(*external_url)] {
                static_cast<void>(OpenExternalUrl(url));
            };
            if (submit) static_cast<void>(submit(operation));
            else operation();
        }
    }

    void FlushActions() {
        FlushIntents();
        FlushSettingsActions();
        bool memory_pending{};
        {
            std::scoped_lock operation_lock(operation_mutex_);
            memory_pending = (read_requested_ || memory_write_requested_) &&
                !memory_invocation_pending_;
            if (memory_pending) memory_invocation_pending_ = true;
        }
        if (!memory_pending) return;
        try {
            const auto invocation = BeginInvocation();
            if (invocation == nullptr) {
                CancelMemoryWork();
                return;
            }
            const auto self = invocation->owner;
            invocation->on_abandon = [self] { self->CancelMemoryWork(); };
            const bool asynchronous = static_cast<bool>(diagnostics_.lifecycle_post);
            const auto submit = diagnostics_.lifecycle_post
                ? diagnostics_.lifecycle_post
                : diagnostics_.lifecycle_invoke;
            if (submit) {
                const auto result = submit(
                    [self, invocation] {
                        invocation->MarkCallbackStarted();
                        self->FlushMemoryCallback();
                    });
                if (result != ERROR_SUCCESS) {
                    if (asynchronous) CancelMemoryWork();
                    SetMemoryStatus(cabbird::MessageId::DeveloperMemoryLifecycleRejected);
                }
            } else {
                invocation->MarkCallbackStarted();
                FlushMemoryCallback();
            }
        } catch (...) {
            CancelMemoryWork();
            SetMemoryStatus(cabbird::MessageId::DeveloperMemoryOperationFailed);
        }
    }

    void SetStatus(const cabbird::MessageId message, const bool failure) {
        std::scoped_lock operation_lock(operation_mutex_);
        status_ = Text(message);
        status_failure_ = failure;
    }

    void SetMemoryStatus(const cabbird::MessageId message) {
        std::scoped_lock operation_lock(operation_mutex_);
        memory_status_ = Text(message);
    }

    void CancelMemoryWork() {
        std::scoped_lock operation_lock(operation_mutex_);
        read_requested_ = false;
        memory_write_requested_ = false;
        memory_invocation_pending_ = false;
        pending_memory_write_.reset();
        memory_status_ = Text(cabbird::MessageId::DeveloperMemoryOwnerClosed);
    }

    void ExecuteIntent(QueuedIntent queued) {
        if (!TryClaimIntent(queued)) return;
        std::unique_lock operation_lock(operation_mutex_);
        try {
            const auto& intent = queued.intent;
            const auto run_backend = [&](auto&& callback) {
                operation_lock.unlock();
                try {
                    callback();
                } catch (...) {
                    operation_lock.lock();
                    throw;
                }
                operation_lock.lock();
                next_snapshot_refresh_ = {};
            };
            const auto observed_revision = model_.Snapshot().revision;
            if (intent.expected_revision != observed_revision) {
                ApplyIntentFailure(
                    queued, IntentExecutionState::Running,
                    cabbird::PlatformUiResultCode::RevisionConflict,
                    cabbird::PluginOperationReason::RevisionConflict,
                    "the queued intent was based on an older Installed snapshot", false);
                return;
            }

            if (intent.kind == cabbird::PlatformUiIntentKind::ReloadAllInstalled) {
                AwaitingBatch batch;
                batch.generations = queued.generations;
                if (batch.generations.empty()) {
                    for (const auto& plugin : model_.Snapshot().installed_plugins) {
                        batch.generations.emplace(plugin.id, plugin.generation);
                    }
                }
                // ReloadAll is synchronous at the PluginManager boundary, but
                // snapshot publication can lag one render frame. Keep the
                // operation Running until the resulting generations are stable.
                run_backend([&] { plugins_.ReloadAll(); });
                batch.queued = queued;
                batch.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                awaiting_batches_.push_back(std::move(batch));
                status_ = Text(cabbird::MessageId::OperationReloadAllSubmitted);
                status_failure_ = false;
                return;
            }

            const bool target_exists = !intent.subject_id.empty() &&
                model_.Snapshot().FindPlugin(intent.subject_id) != nullptr;
            bool succeeded = false;
            bool has_backend = true;
            switch (intent.kind) {
            case cabbird::PlatformUiIntentKind::SetPluginEnabled:
                run_backend([&] {
                    succeeded = target_exists &&
                        plugins_.SetEnabled(intent.subject_id, intent.bool_value);
                });
                break;
            case cabbird::PlatformUiIntentKind::ReloadPlugin:
                run_backend([&] {
                    succeeded = target_exists && plugins_.Reload(intent.subject_id);
                });
                break;
            case cabbird::PlatformUiIntentKind::SetPluginVisible:
                succeeded = target_exists &&
                    plugins_.SetVisible(intent.subject_id, intent.bool_value);
                break;
            default:
                has_backend = false;
                break;
            }
            if (!has_backend) {
                ApplyIntentFailure(
                    queued, IntentExecutionState::Running,
                    cabbird::PlatformUiResultCode::InvalidIntent,
                    cabbird::PluginOperationReason::NotSupported,
                    "intent has no lifecycle backend", false);
                return;
            }

            cabbird::PlatformUiOperationResult result;
            result.operation_id = queued.operation_id;
            result.intent_id = intent.intent_id;
            result.expected_revision = intent.expected_revision;
            result.observed_revision = model_.Snapshot().revision;
            result.mutation = cabbird::ResolvePlatformUiMutation(intent);
            result.subject_id = intent.subject_id;
            result.message = succeeded
                ? "backend accepted operation" : "backend rejected operation";

            const auto after = plugins_.Plugins();
            auto affected = queued.affected;
            if (affected.empty() && !intent.subject_id.empty()) {
                affected.push_back({intent.subject_id, true, "requested object"});
            }
            for (const auto& item : affected) {
                const auto before = queued.generations.contains(item.id)
                    ? queued.generations.at(item.id) : 0;
                const auto found = std::find_if(after.begin(), after.end(), [&](const auto& plugin) {
                    return plugin.id == item.id;
                });
                const bool direct = item.directly_requested;
                const std::uint64_t generation = found == after.end() ? 0 : found->generation;
                const auto state = found == after.end()
                    ? cabbird::PlatformUiPluginState::Unknown
                    : cabbird::ClassifyPluginState(found->state);
                const auto state_reason = OperationReasonForState(state);
                cabbird::PluginOperationOutcome outcome;
                cabbird::PluginOperationReason reason;
                std::string message;
                bool retryable{};

                const auto mark_failure_or_skip = [&](cabbird::PluginOperationReason failure_reason,
                                                        std::string failure_message,
                                                        bool can_retry) {
                    outcome = direct ? cabbird::PluginOperationOutcome::Failed
                                     : cabbird::PluginOperationOutcome::Skipped;
                    reason = failure_reason;
                    message = std::move(failure_message);
                    retryable = can_retry;
                };
                const auto mark_success = [&](std::string success_message) {
                    outcome = cabbird::PluginOperationOutcome::Succeeded;
                    reason = cabbird::PluginOperationReason::None;
                    message = std::move(success_message);
                    retryable = false;
                };
                const auto backend_rejection_message = [&]() -> std::string {
                    if (found == after.end()) {
                        return "plugin was not present after backend operation";
                    }
                    std::string detail = "backend rejected operation";
                    detail += " (state=";
                    detail += cabbird::ToString(state);
                    detail += ")";
                    if (!found->status_reason.empty()) {
                        detail += ": ";
                        detail += found->status_reason;
                    }
                    return detail;
                };

                if (!succeeded) {
                    mark_failure_or_skip(
                        found == after.end() || state_reason == cabbird::PluginOperationReason::None
                            ? (found == after.end() ? cabbird::PluginOperationReason::NotFound
                                                    : cabbird::PluginOperationReason::BackendFailure)
                            : state_reason,
                        backend_rejection_message(),
                        true);
                } else {
                    switch (result.mutation) {
                    case Mutation::Reload:
                        if (found == after.end()) {
                            mark_failure_or_skip(cabbird::PluginOperationReason::NotFound,
                                "plugin was not present after reload", true);
                        } else if (state == cabbird::PlatformUiPluginState::Disabled) {
                            mark_success("disabled plugin package refreshed");
                        } else if (state_reason != cabbird::PluginOperationReason::None) {
                            mark_failure_or_skip(state_reason,
                                "plugin was not reloadable in the resulting snapshot", false);
                        } else if (before != 0 && generation != before) {
                            mark_success("new generation published");
                        } else {
                            mark_failure_or_skip(
                                cabbird::PluginOperationReason::BackendFailure,
                                "reload did not publish a replacement generation", true);
                        }
                        break;
                    case Mutation::Enable:
                        if (found == after.end() || !found->enabled ||
                            state == cabbird::PlatformUiPluginState::Disabled) {
                            mark_failure_or_skip(cabbird::PluginOperationReason::Disabled,
                                "plugin remained disabled after enable operation", false);
                        } else if (!IsHealthyPluginState(state)) {
                            mark_failure_or_skip(state_reason,
                                "plugin is not runnable after enable operation", false);
                        } else {
                            mark_success(item.reason.empty() ? "plugin is enabled" : item.reason);
                        }
                        break;
                    case Mutation::Disable:
                        if (found == after.end() && !direct) {
                            mark_success("dependent plugin is no longer installed");
                        } else if (found == after.end()) {
                            mark_failure_or_skip(cabbird::PluginOperationReason::NotFound,
                                "plugin was not present after disable operation", true);
                        } else if (state != cabbird::PlatformUiPluginState::Disabled &&
                                   state_reason != cabbird::PluginOperationReason::None) {
                            mark_failure_or_skip(state_reason,
                                "plugin entered a blocked state while disabling", false);
                        } else if (found->enabled) {
                            mark_failure_or_skip(cabbird::PluginOperationReason::BackendFailure,
                                "plugin remained enabled after disable operation", true);
                        } else if (state == cabbird::PlatformUiPluginState::Quarantined ||
                                   state == cabbird::PlatformUiPluginState::Faulted) {
                            mark_failure_or_skip(state_reason,
                                "plugin entered a failed state while disabling", true);
                        } else {
                            mark_success("plugin is disabled");
                        }
                        break;
                    case Mutation::SetVisible:
                        if (found == after.end()) {
                            mark_failure_or_skip(cabbird::PluginOperationReason::NotFound,
                                "plugin was not present after visibility operation", true);
                        } else if (!IsHealthyPluginState(state) || !found->visibility_control ||
                                   found->visible != intent.bool_value) {
                            mark_failure_or_skip(state_reason == cabbird::PluginOperationReason::None
                                    ? cabbird::PluginOperationReason::BackendFailure : state_reason,
                                "visibility state did not match the requested value", true);
                        } else {
                            mark_success("visibility state updated");
                        }
                        break;
                    default:
                        mark_success(result.message);
                        break;
                    }
                }
                result.plugins.push_back({item.id, outcome, reason, std::move(message),
                    before, generation, retryable});
            }
            if (result.plugins.empty()) {
                result.plugins.push_back({intent.subject_id,
                    succeeded ? cabbird::PluginOperationOutcome::Succeeded
                              : cabbird::PluginOperationOutcome::Failed,
                    succeeded ? cabbird::PluginOperationReason::None
                              : cabbird::PluginOperationReason::BackendFailure,
                    result.message, 0, 0, !succeeded});
            }
            if (!TrySettleIntent(queued, IntentExecutionState::Running)) return;
            cabbird::FinalizeOperation(result);
            model_.ApplyOperationResult(std::move(result));
            status_ = Text(succeeded
                ? cabbird::MessageId::OperationAccepted
                : cabbird::MessageId::OperationFailedDiagnostics);
            status_failure_ = !succeeded;
        } catch (...) {
            ApplyIntentFailure(
                queued, IntentExecutionState::Running,
                cabbird::PlatformUiResultCode::BackendFailure,
                cabbird::PluginOperationReason::BackendFailure,
                "backend operation raised an exception", true);
        }
    }

    void ResolveAwaitingBatch(const cabbird::PlatformUiSnapshot& next_snapshot) {
        if (awaiting_batches_.empty()) return;
        for (auto iterator = awaiting_batches_.begin(); iterator != awaiting_batches_.end();) {
            std::map<std::string, AwaitingBatch::Observation, std::less<>> observations;
            bool explicit_outcome = true;
            for (const auto& [id, before_generation] : iterator->generations) {
                const auto* after = next_snapshot.FindPlugin(id);
                AwaitingBatch::Observation observation;
                observation.present = after != nullptr;
                observation.generation = after == nullptr ? 0 : after->generation;
                observation.state = after == nullptr
                    ? cabbird::PlatformUiPluginState::Unknown : after->UiState();
                observation.enabled = after != nullptr && after->enabled;
                observations.emplace(id, observation);
                if (after == nullptr ||
                    ((observation.state == cabbird::PlatformUiPluginState::Active ||
                      observation.state == cabbird::PlatformUiPluginState::Loaded) &&
                     observation.generation == before_generation) ||
                    observation.state == cabbird::PlatformUiPluginState::Stopping ||
                    observation.state == cabbird::PlatformUiPluginState::Unknown) {
                    explicit_outcome = false;
                }
            }
            const bool stable = iterator->observed_once &&
                observations == iterator->last_observations;
            const bool timed_out = iterator->deadline != std::chrono::steady_clock::time_point{} &&
                std::chrono::steady_clock::now() >= iterator->deadline;
            iterator->last_observations = observations;
            iterator->observed_once = true;
            if (!explicit_outcome && !stable && !timed_out) {
                status_ = Text(cabbird::MessageId::OperationReloadAllAwaiting);
                status_failure_ = false;
                ++iterator;
                continue;
            }

            cabbird::PlatformUiOperationResult result;
            result.operation_id = iterator->queued.operation_id;
            result.intent_id = iterator->queued.intent.intent_id;
            result.expected_revision = iterator->queued.intent.expected_revision;
            result.observed_revision = next_snapshot.revision;
            result.state = cabbird::PlatformUiOperationState::Running;
            result.code = cabbird::PlatformUiResultCode::Accepted;
            result.mutation = Mutation::ReloadAll;
            result.message = timed_out
                ? "Reload all snapshot did not stabilize before the deadline"
                : "Reload all snapshot is complete";
            for (const auto& [id, before_generation] : iterator->generations) {
                const auto* after = next_snapshot.FindPlugin(id);
                if (after == nullptr) {
                    const auto reason = before_generation == 0
                        ? cabbird::PluginOperationReason::NotFound
                        : cabbird::PluginOperationReason::BackendFailure;
                    result.plugins.push_back({id,
                        before_generation == 0
                            ? cabbird::PluginOperationOutcome::Skipped
                            : cabbird::PluginOperationOutcome::Failed,
                        reason, "plugin was not present after reload", before_generation, 0, true});
                } else if (after->UiState() != cabbird::PlatformUiPluginState::Active &&
                           after->UiState() != cabbird::PlatformUiPluginState::Loaded) {
                    const auto reason = OperationReasonForState(after->UiState());
                    result.plugins.push_back({id, cabbird::PluginOperationOutcome::Skipped,
                        reason, "plugin was not reloadable in the resulting snapshot",
                        before_generation, after->generation, reason ==
                            cabbird::PluginOperationReason::BackendFailure});
                } else if (after->generation != before_generation) {
                    result.plugins.push_back({id, cabbird::PluginOperationOutcome::Succeeded,
                        cabbird::PluginOperationReason::None, "new generation published",
                        before_generation, after->generation, false});
                } else {
                    result.plugins.push_back({id, cabbird::PluginOperationOutcome::Failed,
                        cabbird::PluginOperationReason::BackendFailure,
                        "ReloadAll did not publish a per-plugin result", before_generation,
                        after->generation, true});
                }
            }
            if (result.plugins.empty()) {
                result.plugins.push_back({{}, cabbird::PluginOperationOutcome::Skipped,
                    cabbird::PluginOperationReason::NotFound,
                    "no installed plugins were available for reload", 0, 0, false});
            }
            if (!TrySettleIntent(iterator->queued, IntentExecutionState::Running)) {
                iterator = awaiting_batches_.erase(iterator);
                continue;
            }
            cabbird::FinalizeOperation(result);
            model_.ApplyOperationResult(std::move(result));
            status_ = Text(cabbird::MessageId::OperationReloadAllPublished);
            status_failure_ = false;
            iterator = awaiting_batches_.erase(iterator);
        }
    }

    void FlushDeferredMemory() {
        if (read_requested_) {
            read_requested_ = false;
            const auto address = ParseAddress(address_.data());
            const auto& memory = plugins_.MemoryServices().memory;
            if (!address || memory == nullptr) {
                memory_status_ = Text(cabbird::MessageId::DeveloperMemoryInvalidAddress);
            } else {
                const auto bytes = memory->ReadMemory(*address, static_cast<std::size_t>(read_size_));
                if (!bytes) {
                    memory_status_ = Text(cabbird::MessageId::DeveloperMemoryReadFailed);
                } else {
                    last_address_ = *address;
                    bytes_ = *bytes;
                    memory_status_ = Text(cabbird::MessageId::DeveloperMemoryReadComplete);
                }
            }
        }
        if (!memory_write_requested_) return;
        memory_write_requested_ = false;
        if (!pending_memory_write_) return;
        const auto write = std::move(*pending_memory_write_);
        pending_memory_write_.reset();
        const auto& memory = plugins_.MemoryServices().memory;
        if (memory == nullptr) {
            memory_status_ = Text(cabbird::MessageId::DeveloperMemoryProviderUnavailable);
            return;
        }
        const bool result = write.patch
            ? memory->PatchMemory(write.address, write.bytes.data(), write.bytes.size())
            : memory->WriteMemory(write.address, write.bytes.data(), write.bytes.size());
        memory_status_ = Text(result
            ? (write.patch ? cabbird::MessageId::DeveloperMemoryTrackedPatchApplied
                           : cabbird::MessageId::DeveloperMemoryRawWriteApplied)
            : cabbird::MessageId::DeveloperMemoryWriteFailed);
        if (result) {
            std::snprintf(address_.data(), address_.size(), "0x%llX",
                static_cast<unsigned long long>(write.address));
            read_requested_ = true;
        }
    }

    PluginManager& plugins_;
    // A quarantined UI callback may outlive the render worker. Keep the
    // composition-root PluginManager alive until that owner is retired.
    std::shared_ptr<PluginManager> plugin_owner_;
    std::shared_ptr<cabbird::PluginScope> management_window_scope_;
    cabbird::UiResourceHandle management_window_;
    cabbird::UiResourceHandle logo_texture_;
    PlatformDiagnostics diagnostics_;
    std::optional<ContactInformation> contact_;
    cabbird::PlatformUiModel model_;
    std::chrono::steady_clock::time_point next_snapshot_refresh_{};
    mutable std::mutex catalog_mutex_;
    mutable std::mutex submission_mutex_;
    mutable std::recursive_mutex operation_mutex_;
    mutable std::mutex lifetime_mutex_;
    std::condition_variable lifetime_condition_;
    std::size_t active_callbacks_{};
    std::size_t outstanding_invocations_{};
    bool closing_{};
    std::shared_ptr<const CatalogCache> catalog_cache_;
    std::jthread catalog_worker_;
    mutable std::mutex theme_mutex_;
    std::condition_variable_any theme_changed_;
    cabbird::CabbirdUiPalette theme_requested_palette_{cabbird::CabbirdUiPalette::Paper};
    bool theme_reload_{true};
    bool theme_pick_request_{};
    bool theme_picker_running_{};
    std::optional<int> theme_mode_request_;
    std::optional<float> theme_opacity_request_;
    std::optional<float> theme_opacity_edit_;
    std::shared_ptr<const cabbird::PlatformUiLayoutSnapshot> theme_published_;
    cabbird::PlatformUiLayoutDocument theme_layout_;
    bool theme_layout_available_{};
    std::string theme_layout_message_;
    ThemeImage theme_background_;
    std::vector<ThemeImage> theme_stickers_;
    std::jthread theme_worker_;
    std::vector<QueuedIntent> queued_intents_;
    std::vector<AwaitingBatch> awaiting_batches_;
    std::vector<std::shared_ptr<QueuedIntent>> rejected_callbacks_;
    std::optional<Intent> confirmation_intent_;
    cabbird::PlatformUiAffectedSetPreflight confirmation_plan_;
    bool confirmation_popup_requested_{};
    std::string repository_uninstall_plugin_id_;
    std::string repository_uninstall_plugin_name_;
    bool repository_uninstall_popup_requested_{};
    LayoutMode layout_mode_{LayoutMode::Standard};
    DeveloperPanel developer_panel_{DeveloperPanel::Plugins};
    DeveloperPluginTab developer_plugin_tab_{DeveloperPluginTab::Overview};
    bool management_shell_collapsed_{};
    bool management_shell_locked_{};
    bool management_shell_apply_initial_size_{true};
    bool management_shell_was_collapsed_{};
    float management_shell_ui_scale_{1.0f};
    ImVec2 management_shell_expanded_size_{};
    // Logical viewport size from the previous frame. Embedded windows keep a
    // manual size until the viewport changes; desktop hosts fill their client.
    ImVec2 management_shell_viewport_{};
    bool compact_plugin_detail_{};
    std::string compact_plugin_detail_pending_id_;
    double compact_plugin_detail_requested_at_{};
    std::function<void(const ImVec2&, const ImVec2&)> page_header_actions_;
    bool tab_bar_held_{};
    ImRect tab_bar_rect_;
    bool plugin_search_active_{};
    float plugin_search_reveal_{};
    bool focus_plugin_search_{};
    std::string keyboard_plugin_focus_id_;
    bool keyboard_plugin_navigation_consumed_{};
    bool logs_follow_live_{};
    bool operation_details_open_{};
    bool operation_details_popup_requested_{};
    float plugin_list_width_{};
    std::string performance_selected_plugin_id_;
    std::size_t selected_log_index_{};
    int performance_callback_filter_{};
    int performance_state_filter_{};
    int performance_sort_{};
    SettingsSection settings_section_{SettingsSection::Interface};
    SettingsSection settings_drawing_section_{SettingsSection::Interface};
    bool settings_section_heading_drawn_{};
    cabbird::PlatformSettingsSnapshot settings_snapshot_;
    std::optional<cabbird::PlatformSettingsValues> settings_draft_;
    std::optional<std::uint32_t> settings_scale_edit_percent_;
    std::uint64_t settings_base_revision_{};
    std::shared_ptr<SettingsApplyMailbox> settings_apply_mailbox_{
        std::make_shared<SettingsApplyMailbox>()};
    std::shared_ptr<RepositoryConfigureMailbox> repository_configure_mailbox_{
        std::make_shared<RepositoryConfigureMailbox>()};
    std::optional<cabbird::PlatformSettingsApplyRequest> pending_settings_apply_;
    std::optional<cabbird::PluginRepositoryConfig> pending_repository_configure_;
    std::optional<std::string> settings_route_to_record_;
    std::optional<Route> settings_pending_route_;
    std::optional<Route> settings_route_after_save_;
    std::vector<cabbird::PlatformSettingsValidationError> settings_validation_errors_;
    std::string settings_apply_error_;
    bool settings_save_pending_{};
    bool settings_route_restored_{};
    std::optional<std::string> pending_external_url_;
    bool settings_hotkey_capture_{};
    bool settings_hotkey_hide_all_{};
    bool settings_hotkey_capture_drawn_{};
    std::array<bool, 256> settings_hotkey_down_{};
    bool settings_leave_popup_requested_{};
    bool repo_editor_loaded_{};
    bool repo_editor_master_enabled_{true};
    bool repo_editor_allow_insecure_{};
    std::vector<RepositoryChannelRow> repo_editor_rows_;
    cabbird::PluginRepositoryConfig repo_editor_baseline_;
    std::string repo_editor_status_;
    bool repo_editor_status_failure_{};
    bool repo_editor_save_pending_{};
    std::array<char, 256> search_{};
    std::array<char, 256> performance_search_{};
    std::array<char, 256> settings_search_{};
    std::array<char, 128> log_filter_{};
    std::array<char, 32> address_{};
    std::array<char, 1024> write_bytes_{};
    int read_size_{128};
    bool patch_{true};
    bool developer_mode_{};
    bool read_requested_{};
    bool memory_write_requested_{};
    bool memory_invocation_pending_{};
    bool memory_confirmation_popup_requested_{};
    std::optional<PendingMemoryWrite> pending_memory_write_;
    std::uintptr_t last_address_{};
    std::vector<std::uint8_t> bytes_;
    std::string status_;
    std::string last_toast_status_;
    std::string toast_status_;
    bool status_failure_{};
    bool toast_failure_{};
    double toast_expires_at_{};
    std::string memory_status_{"Ready"};
};

std::shared_ptr<PlatformUi> g_platform_ui;
std::vector<std::shared_ptr<PlatformUi>> g_platform_ui_quarantine;
std::optional<cabbird::PlatformUiState> g_platform_ui_state;
std::mutex g_platform_ui_lifecycle_mutex;
bool g_platform_ui_shutdown_in_progress{};
bool g_platform_ui_shutdown_pending{};

void ReapPlatformUiQuarantineLocked() {
    std::erase_if(g_platform_ui_quarantine, [](const auto& owner) {
        return owner == nullptr || owner->CanReap();
    });
}

}  // namespace

bool StandaloneHostQuarantined(const PluginManager* owner) noexcept {
    return IsStandaloneHostQuarantined(owner);
}

bool InitializePlatformUi(
    PluginManager& plugins,
    PlatformDiagnostics diagnostics,
    std::shared_ptr<PluginManager> plugin_owner) {
    if (plugin_owner == nullptr || plugin_owner.get() != &plugins) return false;
    // A failed teardown leaves the old owner in place as a quarantine fence.
    // Retry its finalization before creating a new owner; otherwise a device
    // rebuild could run an old lifecycle callback beside a new UI instance.
    {
        std::shared_ptr<PlatformUi> previous;
        {
            std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
            ReapPlatformUiQuarantineLocked();
            for (const auto& quarantined : g_platform_ui_quarantine) {
                if (quarantined->BelongsTo(plugins)) return false;
            }
            if (g_platform_ui_shutdown_in_progress) return false;
            previous = g_platform_ui;
            if (previous == nullptr && g_platform_ui_shutdown_pending) return false;
            if (previous != nullptr && !g_platform_ui_shutdown_pending) {
                return previous->BelongsTo(plugins);
            }
            if (previous != nullptr) g_platform_ui_shutdown_in_progress = true;
        }
        if (previous != nullptr) {
            PlatformUi::ShutdownResult result;
            try {
                result = previous->BeginShutdown();
            } catch (...) {
                previous->Quarantine();
                result = {};
            }
            {
                std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
                g_platform_ui_shutdown_in_progress = false;
                if (!result.callbacks_drained) {
                    if (result.state) g_platform_ui_state = *result.state;
                    if (g_platform_ui == previous) g_platform_ui.reset();
                    try {
                        g_platform_ui_quarantine.push_back(previous);
                    } catch (...) {
                        g_platform_ui = std::move(previous);
                        g_platform_ui_shutdown_pending = true;
                        return false;
                    }
                    g_platform_ui_shutdown_pending = false;
                    return false;
                }
                if (!result.state || g_platform_ui != previous) {
                    g_platform_ui_shutdown_pending = true;
                    return false;
                }
                g_platform_ui_state = *result.state;
                g_platform_ui.reset();
                g_platform_ui_shutdown_pending = false;
            }
        }
    }

    if (diagnostics.settings_snapshot) {
        const cabbird::PlatformSettingsSnapshot settings = diagnostics.settings_snapshot();
        if (settings.ready) {
            cabbird::SetCabbirdUiCustomColors(settings.values.interface_custom_colors);
            cabbird::SetCabbirdUiPalette(settings.values.interface_palette);
        }
    }
    ApplyCabbirdUiStyle();
    cabbird::PlatformUiState state;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        if (g_platform_ui_shutdown_in_progress) return false;
        if (g_platform_ui != nullptr) return g_platform_ui->BelongsTo(plugins);
        if (g_platform_ui_state) state = std::move(*g_platform_ui_state);
        auto ui = std::make_shared<PlatformUi>(
            plugins, std::move(diagnostics), std::move(state), std::move(plugin_owner));
        if (!ui->Ready()) return false;
        g_platform_ui = std::move(ui);
    }
    return true;
}

bool RevealPlatformUi() noexcept {
    std::shared_ptr<PlatformUi> ui;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        if (g_platform_ui_shutdown_in_progress) return false;
        ui = g_platform_ui;
    }
    return ui != nullptr && ui->Reveal();
}

bool ApplyHostUiManagementExpansionRequest() noexcept {
    if (!cabbird::ConsumeHostUiManagementExpansionRequest()) return false;
    std::shared_ptr<PlatformUi> ui;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        if (!g_platform_ui_shutdown_in_progress) ui = g_platform_ui;
    }
    if (ui != nullptr) {
        static_cast<void>(ui->Reveal());
        ui->ExpandManagementShell();
    }
    cabbird::SetHostUiMenusCollapsed(false);
    return true;
}

void PreparePlatformUiResources() noexcept {
    std::shared_ptr<PlatformUi> ui;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        ui = g_platform_ui;
    }
    if (ui != nullptr) ui->PrepareResources();
}

void DrawPlatformUi() {
    if (cabbird::HostUiAllHidden()) return;
    std::shared_ptr<PlatformUi> ui;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        ui = g_platform_ui;
    }
    if (ui != nullptr) ui->Draw();
}

bool PlatformUiCapturingHotkey() noexcept {
    std::shared_ptr<PlatformUi> ui;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        ui = g_platform_ui;
    }
    try {
        return ui != nullptr && ui->CapturingSettingsHotkey();
    } catch (...) {
        return false;
    }
}

void FlushPlatformUiActions() {
    std::shared_ptr<PlatformUi> ui;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        ui = g_platform_ui;
    }
    if (ui != nullptr) ui->Flush();
}

bool ShutdownPlatformUi() {
    std::shared_ptr<PlatformUi> ui;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        ReapPlatformUiQuarantineLocked();
        if (g_platform_ui_shutdown_in_progress) return false;
        ui = g_platform_ui;
        // A quarantined owner still owns callbacks and the ImGui context's
        // lifetime fence. Treat it as an incomplete shutdown even though no
        // active owner is published, so callers cannot destroy a context that
        // late callbacks may still reference.
        if (ui == nullptr) {
            return g_platform_ui_quarantine.empty() && !g_platform_ui_shutdown_pending;
        }
        g_platform_ui_shutdown_in_progress = true;
    }

    PlatformUi::ShutdownResult result;
    try {
        result = ui->BeginShutdown();
    } catch (...) {
        ui->Quarantine();
        result = {};
    }

    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        g_platform_ui_shutdown_in_progress = false;
        if (!result.callbacks_drained) {
            if (result.state) g_platform_ui_state = *result.state;
            if (g_platform_ui == ui) g_platform_ui.reset();
            try {
                g_platform_ui_quarantine.push_back(ui);
            } catch (...) {
                g_platform_ui = std::move(ui);
                g_platform_ui_shutdown_pending = true;
                return false;
            }
            g_platform_ui_shutdown_pending = false;
            // The owner is now quarantined, not released. The caller must
            // retain the graphics generation until the quarantine is reaped.
            return false;
        }
        if (!result.state || g_platform_ui != ui) {
            g_platform_ui_shutdown_pending = true;
            return false;
        }
        g_platform_ui_state = *result.state;
        g_platform_ui.reset();
        g_platform_ui_shutdown_pending = false;
    }
    return true;
}

bool QuarantinePlatformUi(std::chrono::milliseconds wait_timeout) noexcept {
    const auto bounded_timeout = (std::max)(wait_timeout, std::chrono::milliseconds::zero());
    const auto deadline = bounded_timeout == std::chrono::milliseconds::max()
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + bounded_timeout;
    for (;;) {
        {
            std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
            if (!g_platform_ui_shutdown_in_progress) break;
        }
        if (deadline != std::chrono::steady_clock::time_point::max() &&
            std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::shared_ptr<PlatformUi> ui;
    {
        std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
        if (g_platform_ui_shutdown_in_progress) return false;
        ReapPlatformUiQuarantineLocked();
        ui = std::move(g_platform_ui);
        g_platform_ui_shutdown_pending = false;
        if (ui == nullptr) return true;
        ui->Quarantine();
        try {
            g_platform_ui_quarantine.push_back(ui);
        } catch (...) {
            // Keep the closed owner published so a later bounded shutdown can
            // retry the handoff instead of destroying it on allocation failure.
            g_platform_ui = std::move(ui);
            g_platform_ui_shutdown_pending = true;
            return false;
        }
    }
    return true;
}

bool PlatformUiQuarantined(const PluginManager* owner) noexcept {
    std::scoped_lock lock(g_platform_ui_lifecycle_mutex);
    ReapPlatformUiQuarantineLocked();
    const bool active_closing = g_platform_ui != nullptr && g_platform_ui->Closing() &&
        (owner == nullptr || g_platform_ui->BelongsTo(*owner));
    if (active_closing) return true;
    if (owner == nullptr) return !g_platform_ui_quarantine.empty();
    return std::any_of(g_platform_ui_quarantine.begin(), g_platform_ui_quarantine.end(),
        [owner](const auto& quarantined) {
            return quarantined != nullptr && quarantined->BelongsTo(*owner);
        });
}

void RunPlatform(
    const std::filesystem::path& root,
    const AnalyzerConfig& config,
    std::stop_token stop_token,
    cabbird::CoreMemoryServices memory_services,
    std::shared_ptr<cabbird::UnityAdapter> adapter,
    PlatformDiagnostics diagnostics,
    std::shared_ptr<PluginManager> plugin_owner) {
    static_cast<void>(memory_services);
    if (plugin_owner == nullptr) return;
    PluginManager& plugins = *plugin_owner;
    // Keep the native host state heap-backed so a deferred quarantine can
    // retain the ImGui/D3D generation after this worker returns.
    auto host_owner = std::make_unique<HostWindow>();
    HostWindow& host = *host_owner;
    host.plugin_owner = plugin_owner;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{
        sizeof(WNDCLASSEXW), CS_CLASSDC, WindowProc, 0, 0, instance, nullptr, nullptr, nullptr,
        nullptr, L"CabbirdPluginPlatformWindow", nullptr};
    bool class_registered{};
    bool imgui_context_created{};
    bool imgui_win32_initialized{};
    bool imgui_dx11_initialized{};
    bool plugin_context_published{};
    bool plugin_service_published{};
    bool tick_callback_installed{};
    bool ui_initialized{};
    bool cleanup_finished{};
    std::shared_ptr<std::mutex> plugin_mutex;
    auto retain_generation = [&]() noexcept {
        if (host.window != nullptr) ShowWindow(host.window, SW_HIDE);
        RetainStandaloneHostOwner(host.plugin_owner);
        // The heap owner keeps g_window, the ImGui context and D3D interfaces
        // valid until the process boundary if another transition still holds
        // the UI lifecycle lock.
        host_owner.release();
        cleanup_finished = true;
    };
    struct CleanupGuard final {
        std::function<void()> cleanup;
        ~CleanupGuard() noexcept {
            if (cleanup) cleanup();
        }
    } guard;
    guard.cleanup = [&]() noexcept {
        if (cleanup_finished) return;
        bool can_release = true;
        if (tick_callback_installed && adapter != nullptr) {
            const bool tick_drained = adapter->ClearTickCallback(std::chrono::seconds(5));
            tick_callback_installed = false;
            if (!tick_drained) {
                std::ofstream(root / L"cabbird-platform.log", std::ios::app)
                    << "standalone tick shutdown deadline exceeded; "
                       "retaining host generation\n";
                if (ui_initialized) {
                    try { static_cast<void>(QuarantinePlatformUi(std::chrono::milliseconds(100))); }
                    catch (...) { }
                }
                retain_generation();
                return;
            }
        }
        if (ui_initialized) {
            bool ui_released{};
            try {
                ui_released = ShutdownPlatformUi();
            } catch (...) {
                ui_released = false;
            }
            if (!ui_released) {
                try {
                    // Quarantine is a successful handoff, but it is not a
                    // release: callbacks may still hold the old ImGui
                    // context. Retain this host generation unconditionally.
                    static_cast<void>(QuarantinePlatformUi(std::chrono::milliseconds(100)));
                } catch (...) {
                }
                ui_released = false;
            }
            if (ui_released) {
                ui_initialized = false;
            } else {
                can_release = false;
            }
        }
        if (can_release && (plugin_context_published || plugin_service_published) &&
            plugin_mutex != nullptr) {
            std::unique_lock plugin_lock(*plugin_mutex, std::defer_lock);
            if (!plugin_lock.try_lock()) {
                can_release = false;
            } else {
                try {
                    if (plugin_service_published) {
                        plugins.SetUiService(nullptr);
                        plugin_service_published = false;
                    }
                    if (plugin_context_published) {
                        plugins.SetImGuiContext(nullptr);
                        plugin_context_published = false;
                    }
                } catch (...) {
                    can_release = false;
                }
            }
        }
        if (!can_release) {
            retain_generation();
            return;
        }
        if (imgui_dx11_initialized) {
            cabbird::glass::ShutdownRenderer();
            try { ImGui_ImplDX11_Shutdown(); } catch (...) { }
            imgui_dx11_initialized = false;
        }
        if (imgui_win32_initialized) {
            try { ImGui_ImplWin32_Shutdown(); } catch (...) { }
            imgui_win32_initialized = false;
        }
        if (imgui_context_created) {
            try {
                if (ImGui::GetCurrentContext() != nullptr) {
                    if (ImGui::GetCurrentContext()->SettingsLoaded) SaveStandaloneImGuiSettings(root);
                    ImGui::DestroyContext();
                }
            } catch (...) { }
            imgui_context_created = false;
        }
        try { DestroyDevice(host); } catch (...) { }
        if (host.window != nullptr) {
            try { DestroyWindow(host.window); } catch (...) { }
            host.window = nullptr;
        }
        if (class_registered) {
            try { UnregisterClassW(window_class.lpszClassName, instance); } catch (...) { }
            class_registered = false;
        }
        if (g_window == &host) g_window = nullptr;
        cleanup_finished = true;
    };
    host.visible = config.platform_visible;
    host.toggle_key = config.platform_toggle_key;
    host.hide_all_key = config.platform_hide_all_key;
    const auto settings_snapshot = diagnostics.settings_snapshot;
    if (settings_snapshot) {
        const auto settings = settings_snapshot();
        if (settings.ready) {
            host.toggle_key = settings.values.input_menu_toggle;
            host.hide_all_key = settings.values.input_hide_all_toggle;
        }
    }
    if (config.platform_attach_to_process_window) {
        for (int attempt = 0;
             attempt < 100 && host.target == nullptr && !stop_token.stop_requested();
             ++attempt) {
            host.target = FindProcessWindow();
            if (host.target == nullptr) Sleep(100);
        }
        if (stop_token.stop_requested()) {
            return;
        }
        host.attached = host.target != nullptr;
    }
    g_window = &host;

    class_registered = RegisterClassExW(&window_class) != 0;
    const DWORD extended_style = host.attached ? WS_EX_TOOLWINDOW : WS_EX_APPWINDOW;
    const DWORD window_style = host.attached ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    host.window = CreateWindowExW(
        extended_style, window_class.lpszClassName, L"Cabbird Plugin Platform", window_style,
        120, 100, 1080, 720, host.attached ? host.target : nullptr, nullptr, instance, nullptr);
    if (host.window == nullptr || !CreateDevice(host)) {
        std::ofstream(root / L"cabbird-platform.log", std::ios::app)
            << "platform window/device creation failed: " << GetLastError() << '\n';
        return;
    }
    // Makes the desktop compositor honour the swap chain's alpha, so the frame
    // stops being an opaque rectangle over the game. Failure only costs
    // transparency, so it is recorded rather than treated as fatal.
    //
    // DIVERGENCE FROM UPSTREAM.  Upstream calls this
    // unconditionally, but its own stated reason -- "over the game" -- only holds when the
    // host is actually drawing over something.  Line 6971 above already draws that exact
    // distinction for the window style (WS_POPUP when attached, WS_OVERLAPPEDWINDOW
    // otherwise); this call was the one place that did not follow it.
    //
    // The consequence was measured, not assumed.  Capturing the preview window's screen
    // rectangle with the preview running and again with it closed showed 45.87% of the
    // window's pixels BIT-IDENTICAL between the two -- and the per-row profile put 74-78%
    // of the top 60 rows in that class, falling to 8% at the bottom.  In other words the
    // upper half of the "preview" was the user's desktop, not this product's UI, so the
    // preview could not do the one job it exists for: showing what the UI looks like.
    // It also explains the report that the top bar "looks completely different" -- at the
    // top of the window there was mostly no top bar to look at.
    //
    // Gated on `attached`, so the in-game path is untouched: transparency is still
    // requested exactly when the frame is composited over the game, which is the only case
    // where it means anything.  With the window opaque the clear colour's zero alpha is
    // ignored by the compositor and the margin renders black, which is what an ordinary
    // window around a #1B1B1B shell should look like.
    host.alpha_composited = host.attached && EnableHostWindowTransparency(host.window);
    const bool alpha_composited = host.alpha_composited;
    {
        std::ofstream log(root / L"cabbird-platform.log", std::ios::app);
        log << "pid=" << GetCurrentProcessId() << " window=" << host.window
            << " attached=" << (host.attached ? 1 : 0) << " target=" << host.target
            << " alphaComposited=" << (alpha_composited ? 1 : 0) << '\n';
    }

    IMGUI_CHECKVERSION();
    imgui_context_created = ImGui::CreateContext() != nullptr;
    if (!imgui_context_created) return;
    static_cast<void>(ConfigureCabbirdUiFontAtlas(root));
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigNavCursorVisibleAuto = false;
    io.ConfigNavEscapeClearFocusWindow = true;
    LoadStandaloneImGuiSettings(root);
    imgui_win32_initialized = ImGui_ImplWin32_Init(host.window);
    if (!imgui_win32_initialized) return;
    imgui_dx11_initialized = ImGui_ImplDX11_Init(host.device, host.context);
    if (!imgui_dx11_initialized) return;
    if (!ImGui_ImplDX11_CreateDeviceObjects()) return;
    if (io.Fonts != nullptr) io.Fonts->ClearTexData();
    static_cast<void>(cabbird::glass::InitializeRenderer(host.device, host.context));
    ApplyCabbirdUiStyle();
    static_cast<void>(CreateStandaloneHeaderLogo(host));

    plugin_mutex = std::make_shared<std::mutex>();
    const auto lifecycle_invoke = diagnostics.lifecycle_invoke;
    auto* const plugin_manager = &plugins;
    diagnostics.lifecycle_invoke = [lifecycle_invoke, plugin_mutex, plugin_manager](
        std::function<void()> operation) -> std::uint32_t {
        auto guarded = [plugin_mutex, plugin_manager, operation = std::move(operation)]() mutable {
            plugin_manager->RunLifecycleOperation(*plugin_mutex, std::move(operation));
        };
        if (lifecycle_invoke) return lifecycle_invoke(std::move(guarded));
        try {
            guarded();
            return ERROR_SUCCESS;
        } catch (...) {
            return ERROR_UNHANDLED_EXCEPTION;
        }
    };
    const auto lifecycle_post = diagnostics.lifecycle_post;
    diagnostics.lifecycle_post = [
        lifecycle_post, lifecycle_invoke, plugin_mutex, plugin_manager](
        std::function<void()> operation) -> std::uint32_t {
        auto guarded = [plugin_mutex, plugin_manager, operation = std::move(operation)]() mutable {
            plugin_manager->RunLifecycleOperation(*plugin_mutex, std::move(operation));
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
    if (adapter != nullptr) {
        tick_callback_installed = true;
        adapter->SetTickCallback([plugin_mutex, &plugins](double delta_seconds) {
            std::unique_lock lock(*plugin_mutex, std::try_to_lock);
            if (!lock.owns_lock()) return;
            plugins.GameUpdate(delta_seconds);
        });
    }
    diagnostics.standalone_window = !host.attached;
    ui_initialized = InitializePlatformUi(plugins, std::move(diagnostics), plugin_owner);
    if (!ui_initialized) {
        // A pending owner is a teardown fence. Do not expose this new ImGui
        // context as a second management surface when the handoff is blocked.
        return;
    }
    {
        std::scoped_lock lock(*plugin_mutex);
        plugin_context_published = true;
        plugins.SetImGuiContext(ImGui::GetCurrentContext());
        plugin_service_published = true;
        plugins.SetUiService(cabbird::HostUiServiceTable());
    }
    // Match the embedded renderer: the native host remains available while
    // Insert collapses the management and plugin surfaces into their chrome.
    cabbird::SetHostUiMenusCollapsed(false);
    if (host.attached) PlaceAttachedWindow(host);
    ShowWindow(host.window, host.visible ? (host.attached ? SW_SHOWNOACTIVATE : SW_SHOWDEFAULT) : SW_HIDE);
    UpdateWindow(host.window);

    auto previous = std::chrono::steady_clock::now();
    HRESULT present_error = S_OK;
    bool running = true;
    while (running && !stop_token.stop_requested()) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) running = false;
        }
        if (settings_snapshot) {
            const auto settings = settings_snapshot();
            if (settings.ready) {
                host.toggle_key = settings.values.input_menu_toggle;
                host.hide_all_key = settings.values.input_hide_all_toggle;
            }
        }
        // The menu hotkey must only react while the game (or, in attached mode,
        // the overlay host itself) owns focus, not while the user is typing in
        // an unrelated application.
        const HWND foreground_window = GetForegroundWindow();
        const bool host_window_focused =
            host.window != nullptr && foreground_window == host.window;
        const bool game_window_focused =
            host.attached && host.target != nullptr &&
            foreground_window == host.target;
        const bool menu_focus = host_window_focused || game_window_focused;
        const int toggle_state = GetAsyncKeyState(static_cast<int>(host.toggle_key));
        const int hide_all_state = GetAsyncKeyState(static_cast<int>(host.hide_all_key));
        const bool recording_hotkey = PlatformUiCapturingHotkey();
        host.menu_hotkey.Configure(host.toggle_key, toggle_state);
        host.hide_all_hotkey.Configure(host.hide_all_key, hide_all_state);
        if (host.hide_all_hotkey.Consume(recording_hotkey, menu_focus, hide_all_state, host_window_focused) &&
            host.hide_all_key != host.toggle_key) {
            cabbird::SetHostUiAllHidden(!cabbird::HostUiAllHidden());
        }
        if (host.menu_hotkey.Consume(recording_hotkey, menu_focus, toggle_state, host_window_focused)) {
            if (!host.attached) {
                // A desktop host remains reachable through its taskbar entry.
                // Hiding it would make this focus-gated hotkey unable to reveal it.
                cabbird::SetHostUiMenusCollapsed(false);
                ShowWindow(host.window, SW_MINIMIZE);
            } else if (!host.visible) {
                host.visible = true;
                if (host.attached) PlaceAttachedWindow(host);
                ShowWindow(host.window, host.attached ? SW_SHOWNOACTIVATE : SW_SHOW);
                cabbird::SetHostUiMenusCollapsed(false);
                // The shell's own close button marks the management window
                // closed and that state is persisted, so without reopening it
                // here the hotkey would show an empty window forever -- in this
                // session and in every later one.
                static_cast<void>(RevealPlatformUi());
            } else if (!cabbird::HostUiMenusCollapsed()) {
                cabbird::SetHostUiMenusCollapsed(true);
            } else {
                // Third state: the window is up but collapsed, so the only thing
                // left on screen is its own opaque background sitting over the
                // game. This used to be a dead end -- the hotkey could collapse
                // the menus but never take the window back down, so the window
                // stayed on top of the game until it was closed by hand. The
                // hotkey now completes the cycle.
                host.visible = false;
                ShowWindow(host.window, SW_HIDE);
            }
        }
        if (ApplyHostUiManagementExpansionRequest() && !host.visible) {
            host.visible = true;
            if (host.attached) PlaceAttachedWindow(host);
            ShowWindow(host.window, host.attached ? SW_SHOWNOACTIVATE : SW_SHOW);
        }
        const auto now = std::chrono::steady_clock::now();
        const double delta = std::chrono::duration<double>(now - previous).count();
        previous = now;
        {
            std::scoped_lock lock(*plugin_mutex);
            if (!plugins.PluginLoadStepInProgress()) {
                static_cast<void>(plugins.MaintenancePluginState());
                if (adapter == nullptr) plugins.GameUpdate(delta);
            }
        }
        plugins.PersistUiWindowState();
        if (io.WantSaveIniSettings) SaveStandaloneImGuiSettings(root);
        if (host.attached && !IsWindow(host.target)) running = false;
        if (host.visible && host.attached && !IsIconic(host.target)) PlaceAttachedWindow(host);
        if (cabbird::HostUiAllHidden()) {
            UpdateHostWindowRegion(host);
            Sleep(10);
            continue;
        }
        if (!host.visible || IsIconic(host.window) || (host.attached && IsIconic(host.target))) {
            Sleep(50);
            continue;
        }
        if (host.render_target == nullptr && !CreateRenderTarget(host)) {
            Sleep(10);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        {
            std::scoped_lock lock(*plugin_mutex);
            PreparePlatformUiResources();
        }
        ImGui::NewFrame();
        cabbird::PrepareHostUiFrame();
        {
            std::scoped_lock lock(*plugin_mutex);
            DrawPlatformUi();
            if (!cabbird::HostUiAllHidden()) plugins.Draw(ImGui::GetCurrentContext());
        }
        ImGui::Render();
        cabbird::glass::PrepareDrawData(ImGui::GetDrawData());
        UpdateHostWindowRegion(host);
        host.context->OMSetRenderTargets(1, &host.render_target, nullptr);
        host.context->ClearRenderTargetView(host.render_target, kHostClearColor);
        cabbird::glass::RenderDrawData(host.context, ImGui::GetDrawData());
        const HRESULT presented = host.swap_chain->Present(1, 0);
        if (FAILED(presented)) {
            present_error = presented;
            running = false;
        }
        FlushPlatformUiActions();
    }

    if (FAILED(present_error)) {
        std::ofstream(root / L"cabbird-platform.log", std::ios::app)
            << "standalone presentation failed; stopping UI: "
            << static_cast<unsigned long>(present_error) << '\n';
    }

    // Stop the external tick source before draining lifecycle UI callbacks so
    // it cannot reacquire the shared plugin gate during the handoff.
    if (adapter != nullptr) {
        const bool tick_drained = adapter->ClearTickCallback(std::chrono::seconds(5));
        tick_callback_installed = false;
        if (!tick_drained) {
            std::ofstream(root / L"cabbird-platform.log", std::ios::app)
                << "standalone tick shutdown deadline exceeded; retaining host generation\n";
            if (ui_initialized) {
                try { static_cast<void>(QuarantinePlatformUi(std::chrono::milliseconds(100))); }
                catch (...) { }
            }
            retain_generation();
            return;
        }
    }
    const auto shutdown_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool ui_shutdown{};
    while (!ui_shutdown && std::chrono::steady_clock::now() < shutdown_deadline) {
        ui_shutdown = ShutdownPlatformUi();
        if (ui_shutdown) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!ui_shutdown) {
        std::ofstream(root / L"cabbird-platform.log", std::ios::app)
            << "standalone UI shutdown deadline exceeded; continuing host cleanup\n";
        // Quarantine never proves that callbacks have drained. Do not destroy
        // the ImGui context from this worker regardless of whether the bounded
        // handoff succeeds or another transition still owns the active UI.
        static_cast<void>(QuarantinePlatformUi(std::chrono::milliseconds(100)));
        std::ofstream(root / L"cabbird-platform.log", std::ios::app)
            << "standalone UI quarantine recorded; retaining graphics generation\n";
        retain_generation();
        return;
    }
    ui_initialized = false;
    guard.cleanup();
}

}  // namespace cabbird

#include "cabbird/unitymem_compat.hpp"
#include "cabbird/launcher/manual_map.hpp"
#include "cabbird/launcher/configuration.hpp"
#include "cabbird/i18n.hpp"
#include "cabbird/cabbird_ui_theme.hpp"
#include "cabbird/liquid_glass.hpp"
#include "cabbird/runtime_launch.hpp"
#include "cabbird/runtime_recovery.hpp"
#include "cabbird/ui_resource_decoder.hpp"
#include "cabbird/platform_ui_layout.hpp"
#include "cabbird/config.hpp"
#include "desktop_surface.hpp"
#include "cabbird/launcher/release_update.hpp"
#include "cabbird/platform_notice.hpp"
#include "cabbird/telemetry.hpp"
#include "cabbird/safe_zip.hpp"
#include "cabbird/sdk/version.h"
#include <winhttp.h>
#include <fstream>
#include <cstdio>

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kLogoResourceId = 101;
constexpr int kIconResourceId = 201;
constexpr float kCaptionHeight = 64.0f;
constexpr float kModeHeight = 76.0f;
constexpr float kLaunchActionRowBottom = 82.0f;
constexpr float kLaunchActionRowHeight = 62.0f;
constexpr float kLauncherBaseFontPixels = 13.0f;
constexpr float kLauncherFontScale = 20.0f / kLauncherBaseFontPixels;
constexpr std::array kLauncherFontScales{2.00f};
constexpr std::array<char32_t, 7> kLauncherIcons{
    0xe838, 0xe72c, 0xe768, 0xe73e, 0xe7ba, 0xe711, 0xe8b7};
constexpr float kDefaultDpi = 96.0f;
constexpr std::string_view kRepositoryUrl{"https://github.com/CabbirdAP/Cabbird"};
constexpr std::string_view kDiscordUrl{"https://discord.gg/tfeh8QQ9Vy"};

ImVec4 ThemeColor(const unitymem::CabbirdUiColor& color) noexcept {
    return {color.red, color.green, color.blue, color.alpha};
}

ImVec4 ThemeColorWithAlpha(
    const unitymem::CabbirdUiColor& color, const float alpha) noexcept {
    return {color.red, color.green, color.blue, alpha};
}

enum class LauncherMode : std::uint8_t { Launch, Settings };

enum class MessageKind : std::uint8_t { Neutral, Success, Error };
enum class ProcessAnnounce : std::uint8_t { Always, Never, OnChange };

struct LauncherMessage final {
    cabbird::MessageId id{cabbird::MessageId::LauncherStateReady};
    std::vector<std::string> arguments;
    std::string detail;
    friend bool operator==(const LauncherMessage&, const LauncherMessage&) = default;
};

struct LauncherTheme final {
    std::uint64_t revision{};
    cabbird::PlatformUiLayoutDocument layout;
    mutable cabbird::UiRgba8Image background;
    mutable std::vector<cabbird::UiRgba8Image> stickers;

    [[nodiscard]] bool HasBackground() const noexcept {
        return background.width != 0 && background.height != 0 &&
            layout.background.mode != cabbird::PlatformUiBackgroundMode::Disabled;
    }
};

enum class LauncherUpdatePhase : std::uint8_t {
    Idle,
    Checking,
    UpToDate,
    Available,
    Downloading,
    Installing,
    Installed,
    Failed,
};

// What a failed update step was. The launcher names the ones it can explain -- an unreachable host,
// a rate-limited API -- and leaves the rest to the installer, whose diagnostic says what could not
// be written.
enum class LauncherUpdateFailure : std::uint8_t {
    None,
    Network,
    RateLimited,
    Http,
    NotRelease,
    Check,
    Install,
};

// What the last check found and, once it is asked for, how far the update got. Everything here is
// read from the snapshot; the phase is the only field the page branches on.
struct LauncherUpdateStatus final {
    LauncherUpdatePhase phase{LauncherUpdatePhase::Idle};
    bool auto_check{true};
    // The release the user answered "skip this version" to: the check keeps reporting it, but the
    // launcher does not ask about that one version again.
    std::string skipped_version;
    std::string remote_tag;
    std::string remote_version;
    std::string page_url;
    // The package the check chose: its address, its published length and its published digest, which
    // is what a download is verified against before anything is extracted.
    cabbird::launcher::ReleaseAsset asset;
    // The release notes the dialog shows before the user decides.
    std::string notes;
    LauncherUpdateFailure failure{LauncherUpdateFailure::None};
    // The HTTP status of a failed lookup, and the diagnostic a failed install reported.
    int http_status{};
    std::string detail;

    [[nodiscard]] bool Busy() const noexcept {
        return phase == LauncherUpdatePhase::Checking ||
            phase == LauncherUpdatePhase::Downloading ||
            phase == LauncherUpdatePhase::Installing;
    }

    [[nodiscard]] bool CanInstall() const noexcept {
        return !asset.download_url.empty() &&
            (phase == LauncherUpdatePhase::Available ||
             (phase == LauncherUpdatePhase::Failed && failure == LauncherUpdateFailure::Install));
    }
};


struct LauncherSnapshot final {
    cabbird::launcher::AzurPromiliaClient selected_client{
        cabbird::launcher::AzurPromiliaClient::MainlandChina};
    std::filesystem::path game_directory;
    std::filesystem::path launcher_executable;
    std::optional<cabbird::RuntimeRecoveryState> recovery;
    std::string recovery_message;
    std::vector<cabbird::launcher::AttachableProcess> processes;
    DWORD attached_process{};
    bool core_available{};
    bool restart_pending{};
    LauncherUpdateStatus update;
    std::string runtime_version;
    std::string runtime_message;
    std::uint32_t toggle_key{VK_INSERT};
    std::string theme{"paper"};
    std::string language{"auto"};
    cabbird::launcher::LauncherThemeArtwork artwork;
    unitymem::CabbirdUiCustomColors custom_colors;
    int frame_rate_limit{cabbird::launcher::kLauncherDefaultFrameRate};
    std::shared_ptr<const cabbird::Translator> translator;
    std::shared_ptr<const LauncherTheme> theme_layout;
    std::string settings_error;
    bool busy{};
    LauncherMessage message;
    MessageKind message_kind{MessageKind::Neutral};
};

std::filesystem::path ExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path);
}

std::filesystem::path ExecutableDirectory() {
    return ExecutablePath().parent_path();
}

struct AdministratorLaunchResult final {
    bool run_current_process{};
    int exit_code{};
};

AdministratorLaunchResult EnsureAdministrator(PWSTR command_line) noexcept {
    HANDLE token{};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return {false, static_cast<int>(GetLastError())};
    }
    TOKEN_ELEVATION elevation{};
    DWORD returned{};
    const BOOL queried = GetTokenInformation(
        token, TokenElevation, &elevation, sizeof(elevation), &returned);
    const DWORD query_error = queried == FALSE ? GetLastError() : ERROR_SUCCESS;
    CloseHandle(token);
    if (queried == FALSE) return {false, static_cast<int>(query_error)};
    if (elevation.TokenIsElevated != 0) return {true, ERROR_SUCCESS};

    const auto executable = ExecutablePath();
    if (executable.empty()) return {false, ERROR_FILE_NOT_FOUND};
    const std::wstring working_directory = executable.parent_path().wstring();
    SHELLEXECUTEINFOW launch{
        .cbSize = sizeof(launch),
        .fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC,
        .lpVerb = L"runas",
        .lpFile = executable.c_str(),
        .lpParameters = command_line != nullptr && command_line[0] != L'\0'
            ? command_line : nullptr,
        .lpDirectory = working_directory.c_str(),
        .nShow = SW_SHOWNORMAL,
    };
    if (ShellExecuteExW(&launch) == FALSE) {
        return {false, static_cast<int>(GetLastError())};
    }
    if (launch.hProcess != nullptr) CloseHandle(launch.hProcess);
    return {false, ERROR_SUCCESS};
}

HMODULE LoadSystemDwmapi() {
    std::wstring directory(32768, L'\0');
    const UINT length = GetSystemDirectoryW(
        directory.data(), static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size()) return nullptr;
    directory.resize(length);
    return LoadLibraryExW(
        (std::filesystem::path(directory) / L"dwmapi.dll").c_str(),
        nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

std::string WideUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size, nullptr, nullptr) != size) {
        return {};
    }
    return result;
}

std::wstring Utf8Wide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size) != size) {
        return {};
    }
    return result;
}

std::string PathUtf8(const std::filesystem::path& path) {
    auto native = path;
    native.make_preferred();
    return WideUtf8(native.native());
}

std::string VirtualKeyName(const std::uint32_t key) {
    const std::string fallback = "Key " + std::to_string(key);
    const UINT scan_code = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    wchar_t buffer[64]{};
    const LONG parameter = static_cast<LONG>(scan_code << 16U);
    if (GetKeyNameTextW(parameter, buffer, static_cast<int>(std::size(buffer))) <= 0) {
        return fallback;
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return fallback;
    std::string result(static_cast<std::size_t>(size), '\0');
    static_cast<void>(WideCharToMultiByte(
        CP_UTF8, 0, buffer, -1, result.data(), size, nullptr, nullptr));
    result.pop_back();
    return result;
}

bool PathsEqual(
    const std::filesystem::path& left, const std::filesystem::path& right) noexcept {
    try {
        const std::wstring left_value = left.lexically_normal().wstring();
        const std::wstring right_value = right.lexically_normal().wstring();
        return !left_value.empty() && !right_value.empty() &&
            CompareStringOrdinal(
                left_value.c_str(), -1, right_value.c_str(), -1, TRUE) == CSTR_EQUAL;
    } catch (...) {
        return false;
    }
}

std::string EncodeUtf8(char32_t codepoint) {
    std::string result;
    if (codepoint <= 0x7fU) {
        result.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        result.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        result.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    return result;
}

std::string Ellipsize(std::string_view value, float width) {
    if (ImGui::CalcTextSize(value.data(), value.data() + value.size()).x <= width) {
        return std::string(value);
    }
    constexpr std::string_view suffix{"..."};
    if (ImGui::CalcTextSize(suffix.data(), suffix.data() + suffix.size()).x > width) {
        return {};
    }
    std::size_t end = value.size();
    while (end > 0) {
        --end;
        while (end > 0 &&
               (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U) {
            --end;
        }
        const auto candidate = std::string(value.substr(0, end)) + std::string(suffix);
        if (ImGui::CalcTextSize(candidate.c_str()).x <= width) return candidate;
    }
    return std::string(suffix);
}

const char* Glyph(char32_t codepoint) {
    struct Entry final { char32_t codepoint; std::string text; };
    static const std::array<Entry, kLauncherIcons.size()> entries = [] {
        std::array<Entry, kLauncherIcons.size()> result{};
        for (std::size_t index = 0; index < kLauncherIcons.size(); ++index)
            result[index] = {kLauncherIcons[index], EncodeUtf8(kLauncherIcons[index])};
        return result;
    }();
    const auto found = std::find_if(entries.begin(), entries.end(), [codepoint](const auto& entry) {
        return entry.codepoint == codepoint;
    });
    return found == entries.end() ? "?" : found->text.c_str();
}

LauncherMessage MakeLauncherMessage(
    cabbird::MessageId id,
    std::initializer_list<std::string_view> arguments = {},
    std::string detail = {}) {
    LauncherMessage message;
    message.id = id;
    message.arguments.reserve(arguments.size());
    for (const auto argument : arguments) message.arguments.emplace_back(argument);
    message.detail = std::move(detail);
    return message;
}

std::shared_ptr<const LauncherTheme> LoadLauncherTheme(
    const std::filesystem::path& runtime_root,
    const cabbird::launcher::LauncherConfiguration& configuration, const std::uint64_t revision) {
    auto prepared = cabbird::PreparePlatformUiLayout(runtime_root,
        cabbird::ResolveLauncherUiLayoutFile(runtime_root, configuration.theme), revision);
    auto result = std::make_shared<LauncherTheme>();
    result->revision = revision;
    result->layout = std::move(prepared.layout);
    result->background = std::move(prepared.background);
    result->stickers = std::move(prepared.stickers);
    const auto original_path = result->layout.background.path;
    if (!configuration.artwork.background.empty()) {
        result->layout.background.path = configuration.artwork.background;
        if (result->layout.background.mode == cabbird::PlatformUiBackgroundMode::Disabled &&
            configuration.artwork.background_mode < 0)
            result->layout.background.mode = cabbird::PlatformUiBackgroundMode::Crop;
        if (result->layout.background.opacity <= 0.0f) result->layout.background.opacity = 1.0f;
    }
    if (configuration.artwork.background_mode >= 0)
        result->layout.background.mode = static_cast<cabbird::PlatformUiBackgroundMode>(
            configuration.artwork.background_mode);
    if (configuration.artwork.surface_opacity >= 0.0f)
        result->layout.surface_opacity = configuration.artwork.surface_opacity;
    if (result->layout.background.mode == cabbird::PlatformUiBackgroundMode::Disabled) {
        result->background = {};
    } else if (original_path != result->layout.background.path || result->background.width == 0) {
        result->background = {};
        std::filesystem::path resolved;
        if (cabbird::ResolvePlatformUiLayoutPath(runtime_root, result->layout.background.path, resolved)) {
            const auto bytes = cabbird::ReadUiResourceBytes(resolved, cabbird::kDefaultUiResourceEncodedByteLimit);
            if (bytes) {
                auto decoded = cabbird::DecodeUiImageRgba8(bytes.bytes);
                if (decoded) result->background = std::move(decoded.image);
            }
        }
    }
    return result;
}

struct LauncherDownloadProgress final {
    std::atomic<long long> bytes{};
    std::atomic<long long> total{};
    std::atomic<bool> cancel{};
};

std::string LauncherHttpGetUrl(const std::string& url, bool use_system_proxy = false,
    int* status_code = nullptr, DWORD timeout_ms = 8000);
bool LauncherDownloadUrl(const std::string& url, const std::filesystem::path& destination,
    LauncherDownloadProgress* progress = nullptr);
void LauncherUpdateLog(const std::string& line);
std::wstring LauncherSystemProxy();

// The addresses one github.com URL is asked at, best-known first: the mirror that answered last time,
// then the direct address, then the rest. A launcher started on a network where github.com does not
// answer would otherwise spend its whole timeout budget on the direct attempt before it ever reaches
// the mirror that works.
std::vector<std::string> LauncherLookupCandidates(
    const std::string& url, const std::string& endpoint) {
    std::vector<std::string> candidates;
    candidates.reserve(1 + cabbird::launcher::kGithubProxyPrefixes.size());
    if (std::ranges::find(cabbird::launcher::kGithubProxyPrefixes, endpoint) !=
        cabbird::launcher::kGithubProxyPrefixes.end()) candidates.push_back(endpoint + url);
    candidates.push_back(url);
    for (const auto prefix : cabbird::launcher::kGithubProxyPrefixes) {
        if (std::string(prefix) != endpoint) candidates.push_back(std::string(prefix) + url);
    }
    return candidates;
}

// Which mirror a URL was asked through: the prefix it starts with, or nothing for the direct address.
std::string LauncherEndpointOfUrl(const std::string& url) {
    for (const auto prefix : cabbird::launcher::kGithubProxyPrefixes) {
        if (url.starts_with(prefix)) return std::string(prefix);
    }
    return {};
}



// Mod installs and launcher updates run on different workers and may overlap.

LauncherDownloadProgress g_update_download;


void LauncherUpdateLog(const std::string& line) {
    std::error_code error;
    const auto directory = ExecutableDirectory() / L"Cabbird" / L"logs";
    std::filesystem::create_directories(directory, error);
    std::ofstream(directory / L"launcher-update.log", std::ios::app)
        << line << "\n";
}

std::string LauncherHttpGetRequest(const std::wstring& host, const std::wstring& path,
    const INTERNET_PORT port, const bool secure, const bool use_system_proxy, int* status_code,
    const DWORD timeout_ms) {
    std::string result;
    if (status_code != nullptr) *status_code = 0;
    // Direct first: a stale or half-configured system proxy is a common cause of "works in curl,
    // fails in WinHTTP". The caller retries with the system proxy when this returns nothing.
    const std::wstring proxy = use_system_proxy ? std::wstring{} : LauncherSystemProxy();
    HINTERNET session = WinHttpOpen(L"CabbirdLauncher/1.0",
        proxy.empty() ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        LauncherUpdateLog("open session failed: " + std::to_string(GetLastError()));
        return result;
    }
    // TLS 1.2/1.3 explicitly: WinHTTP's default protocol set still excludes them on some builds,
    // which shows up as an intermittent handshake failure.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(session, static_cast<int>(timeout_ms), static_cast<int>(timeout_ms),
        static_cast<int>(timeout_ms), static_cast<int>(timeout_ms));
    HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
    if (connection != nullptr) {
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            secure ? WINHTTP_FLAG_SECURE : 0);
        if (request != nullptr) {
            // GameBanana answers with redirects for some hosts and rejects unknown clients, so the
            // policy follows redirects and the request presents a browser-style identity.
            DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
                sizeof(redirect_policy));
            const wchar_t* headers =
                L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) CabbirdLauncher/1.0\r\n"
                L"Accept: application/json\r\n"
                L"Accept-Encoding: identity\r\n";
            const BOOL sent = WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L),
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
            const DWORD send_error = sent == FALSE ? GetLastError() : 0;
            const BOOL received = sent != FALSE ? WinHttpReceiveResponse(request, nullptr) : FALSE;
            const DWORD receive_error = received == FALSE ? GetLastError() : 0;
            if (sent != FALSE && received != FALSE) {
                // The status is what separates "this repository publishes no release" from "the API
                // refused to answer at all", and the launcher reports the difference to the user.
                DWORD status{};
                DWORD status_size = sizeof(status);
                if (WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                        WINHTTP_NO_HEADER_INDEX) != FALSE && status_code != nullptr) {
                    *status_code = static_cast<int>(status);
                }
                // Read until the stream reports zero bytes: QueryDataAvailable returns 0 for
                // "nothing right now" too, which used to truncate the body mid-JSON.
                std::string chunk(16 * 1024, '\0');
                DWORD read{};
                while (result.size() <= (1u << 20) &&
                    WinHttpReadData(request, chunk.data(), static_cast<DWORD>(chunk.size()), &read) != FALSE &&
                    read > 0) {
                    result.append(chunk.data(), read);
                    read = 0;
                }
            }
            if (result.empty()) {
                DWORD status{};
                DWORD size = sizeof(status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                LauncherUpdateLog("get " + std::string(path.begin(), path.end()) +
                    " empty; status=" + std::to_string(status) +
                    " error=" + std::to_string(GetLastError()));
            }
            WinHttpCloseHandle(request);
        } else {
            LauncherUpdateLog("open request failed: " + std::to_string(GetLastError()));
        }
        WinHttpCloseHandle(connection);
    } else {
        LauncherUpdateLog("connect failed: " + std::to_string(GetLastError()));
    }
    WinHttpCloseHandle(session);
    return result;
}

std::string LauncherHttpGetUrl(const std::string& url, const bool use_system_proxy,
    int* status_code, const DWORD timeout_ms) {
    if (status_code != nullptr) *status_code = 0;
    const std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) == FALSE) {
        LauncherUpdateLog("url get: crack failed " + std::to_string(GetLastError()) +
            " url=" + url);
        return {};
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength != 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const INTERNET_PORT port = parts.nPort == 0 ? INTERNET_DEFAULT_HTTPS_PORT : parts.nPort;
    return LauncherHttpGetRequest(host, path, port, parts.nScheme == INTERNET_SCHEME_HTTPS,
        use_system_proxy, status_code, timeout_ms);
}

std::wstring LauncherSystemProxy() {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", 0, KEY_READ,
            &key) != ERROR_SUCCESS) {
        return {};
    }
    DWORD enabled{};
    DWORD size = sizeof(enabled);
    DWORD type{};
    std::wstring proxy;
    if (RegQueryValueExW(key, L"ProxyEnable", nullptr, &type,
            reinterpret_cast<LPBYTE>(&enabled), &size) == ERROR_SUCCESS && enabled != 0) {
        wchar_t buffer[512]{};
        DWORD bytes = sizeof(buffer);
        if (RegQueryValueExW(key, L"ProxyServer", nullptr, &type,
                reinterpret_cast<LPBYTE>(buffer), &bytes) == ERROR_SUCCESS) {
            proxy = buffer;
        }
    }
    RegCloseKey(key);
    return proxy;
}

bool LauncherDownloadUrl(const std::string& url, const std::filesystem::path& destination,
    LauncherDownloadProgress* const progress) {
    const std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) == FALSE) {
        LauncherUpdateLog("crack failed " + std::to_string(GetLastError()) + " url=" + url);
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength != 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const INTERNET_PORT port = parts.nPort == 0 ? INTERNET_DEFAULT_HTTPS_PORT : parts.nPort;
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    const std::wstring proxy = LauncherSystemProxy();
    HINTERNET session = WinHttpOpen(L"CabbirdLauncher/1.0",
        proxy.empty() ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        LauncherUpdateLog("url dl: session failed " + std::to_string(GetLastError()));
        return false;
    }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
    bool ok{};
    HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
    if (connection == nullptr) {
        LauncherUpdateLog("url dl: connect failed " + std::to_string(GetLastError()) +
            " host=" + std::string(host.begin(), host.end()));
    }
    if (connection != nullptr) {
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            secure ? WINHTTP_FLAG_SECURE : 0);
        if (request != nullptr) {
            DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
                sizeof(redirect_policy));
            const wchar_t* headers =
                L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) CabbirdLauncher/1.0\r\n"
                L"Accept: */*\r\n";
            if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA,
                    0, 0, 0) != FALSE &&
                WinHttpReceiveResponse(request, nullptr) != FALSE) {
                if (progress != nullptr) {
                    progress->bytes.store(0);
                    wchar_t length_text[32]{};
                    DWORD length_size = sizeof(length_text);
                    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX, length_text, &length_size,
                            WINHTTP_NO_HEADER_INDEX) != FALSE) {
                        progress->total.store(_wtoi64(length_text));
                    } else {
                        progress->total.store(0);
                    }
                }
                std::filesystem::create_directories(destination.parent_path());
                std::ofstream out(destination, std::ios::binary | std::ios::trunc);
                if (out) {
                    ok = true;
                    // Read until zero bytes come back (true EOF); QueryDataAvailable returning 0
                    // only means "nothing right now" and used to truncate the payload.
                    std::string chunk(64 * 1024, '\0');
                    const auto started = std::chrono::steady_clock::now();
                    long long received{};
                    DWORD read{};
                    for (;;) {
                        if (!WinHttpReadData(request, chunk.data(), static_cast<DWORD>(chunk.size()), &read)) {
                            ok = false;
                            break;
                        }
                        if (read == 0) break;
                        out.write(chunk.data(), static_cast<std::streamsize>(read));
                        if (!out) { ok = false; break; }
                        received += static_cast<long long>(read);
                        if (progress != nullptr) progress->bytes.fetch_add(static_cast<long long>(read));
                        read = 0;
                        if (progress != nullptr && progress->cancel.load()) {
                            LauncherUpdateLog("url dl: cancelled by user");
                            ok = false;
                            break;
                        }
                        if (std::chrono::steady_clock::now() - started > std::chrono::minutes(10)) {
                            LauncherUpdateLog("url dl: total timeout after " +
                                std::to_string(received) + " bytes");
                            ok = false;
                            break;
                        }
                    }
                    out.close();
                    if (!out) ok = false;
                    if (ok && received == 0) {
                        LauncherUpdateLog("url dl: empty body " + url);
                        ok = false;
                    }
                }
            } else {
                LauncherUpdateLog("url dl: request failed " + std::to_string(GetLastError()) +
                    " url=" + url);
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    if (!ok) {
        std::error_code error;
        std::filesystem::remove(destination, error);
    }
    return ok;
}

class LauncherController final {
public:
    explicit LauncherController(std::filesystem::path payload_root,
        const cabbird::launcher::LauncherConfiguration& initial_configuration = {},
        std::shared_ptr<const cabbird::Translator> initial_translator = {})
        : payload_root_(std::move(payload_root)),
          runtime_directory_(payload_root_ / L"Cabbird"),
          configuration_path_(cabbird::launcher::LauncherConfigurationPath(payload_root_)),
          state_{.theme = initial_configuration.theme, .language = initial_configuration.language,
              .artwork = initial_configuration.artwork, .custom_colors = initial_configuration.custom_colors,
              .frame_rate_limit = initial_configuration.frame_rate_limit,
              .translator = std::move(initial_translator)},
          worker_([this](std::stop_token stop) { WorkerMain(stop); }) {
        Queue(cabbird::MessageId::LauncherStatusScanningLocal, [this] {
            InitializePathsImpl();
        });
    }

    ~LauncherController() {
        g_update_download.cancel.store(true);
        update_check_worker_.request_stop();
        if (update_check_worker_.joinable()) update_check_worker_.join();
        telemetry_worker_.request_stop();
        if (telemetry_worker_.joinable()) telemetry_worker_.join();
        worker_.request_stop();
        queue_changed_.notify_all();
        worker_.join();
        if (state_.theme_layout != nullptr) static_cast<void>(PersistConfigurationImpl());
    }

    LauncherController(const LauncherController&) = delete;
    LauncherController& operator=(const LauncherController&) = delete;

    [[nodiscard]] LauncherSnapshot Snapshot() const {
        std::scoped_lock lock(state_mutex_);
        return state_;
    }

    void SelectGameDirectory(std::filesystem::path directory) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return;
            state_.game_directory = std::move(directory);
            configuration_.Selected().game_directory = state_.game_directory;
        }
        Queue(cabbird::MessageId::LauncherStatusInspectingCore, [this] {
            ReconcileRelatedPathsImpl();
            const auto saved = PersistConfigurationImpl();
            RefreshHotkeyImpl();
            RefreshRecoveryImpl();
            RefreshProcessesImpl(ProcessAnnounce::Never);
            if (!saved.Ok()) {
                PublishMessage(cabbird::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, saved.message);
            }
        });
    }

    void RestoreRecovery(cabbird::RuntimeRecoveryAxis axis) {
        Queue(cabbird::MessageId::LauncherStatusRestoringRecovery, [this, axis] {
            const auto runtime_root = GameDirectory() / L"Cabbird";
            cabbird::RuntimeRecoveryStore store(runtime_root);
            PublishRecovery(store.Restore(axis), true);
        });
    }

    void RefreshProcesses() {
        Queue(cabbird::MessageId::LauncherStatusScanningProcesses,
            [this] { RefreshProcessesImpl(); });
    }

    void PollProcesses() {
        constexpr auto interval = std::chrono::seconds(2);
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return;
            const auto now = std::chrono::steady_clock::now();
            if (last_process_poll_ != std::chrono::steady_clock::time_point{} &&
                now - last_process_poll_ < interval) return;
            last_process_poll_ = now;
        }
        {
            std::scoped_lock lock(queue_mutex_);
            queue_.push_back([this] { RefreshProcessesImpl(ProcessAnnounce::OnChange); });
        }
        queue_changed_.notify_one();
    }

    void SelectLauncherExecutable(std::filesystem::path executable) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return;
            state_.launcher_executable = std::move(executable);
            configuration_.Selected().launcher_executable = state_.launcher_executable;
        }
        Queue(cabbird::MessageId::LauncherStatusScanningLocal, [this] {
            ReconcileRelatedPathsImpl();
            const auto saved = PersistConfigurationImpl();
            RefreshProcessesImpl();
            if (!saved.Ok()) {
                PublishMessage(cabbird::MessageId::LauncherStatusUnexpectedFailure,
                    MessageKind::Error, saved.message);
            }
        });
    }

    void LaunchAndAttach() {
        Queue(cabbird::MessageId::LauncherStatusLaunchingAttach, [this] {
            const auto launcher = LauncherExecutable();
            const auto selected = ResolveLaunchRuntime();
            if (!selected.Ok()) {
                PublishRuntimeFailure(selected);
                return;
            }
            cabbird::launcher::ManualMapLaunchOptions options;
            options.launcher_path = launcher;
            options.working_directory = launcher.parent_path();
            options.manual_map.core_path = selected.core_path;
            options.manual_map.runtime_root = selected.runtime_root;
            options.manual_map.log_directory = options.manual_map.runtime_root / L"logs";
            const auto result =
                cabbird::launcher::LaunchAndManualMapRuntimeCore(options, worker_.get_stop_token());
            if (result.Ok()) RefreshProcessesImpl();

            std::scoped_lock lock(state_mutex_);
            if (result.Ok()) {
                state_.attached_process = result.process_id;
                const std::string process_id = std::to_string(result.process_id);
                state_.message = MakeLauncherMessage(
                    cabbird::MessageId::LauncherStatusLaunchAttached, {process_id});
                state_.message_kind = MessageKind::Success;
            } else {
                const std::string error = std::to_string(result.mapping.win32_error);
                state_.message = MakeLauncherMessage(
                    cabbird::MessageId::LauncherStatusLaunchAttachFailed, {error},
                    result.mapping.message);
                state_.message_kind = MessageKind::Error;
            }
        });
    }

    void SetAutoCheckUpdates(const bool enabled) {
        {
            std::scoped_lock lock(state_mutex_);
            if (configuration_.update.auto_check == enabled) return;
            configuration_.update.auto_check = enabled;
            state_.update.auto_check = enabled;
            settings_dirty_ = true;
        }
    }

    void SkipUpdateVersion(std::string version) {
        {
            std::scoped_lock lock(state_mutex_);
            if (version.empty() || configuration_.update.skipped_version == version) return;
            configuration_.update.skipped_version = std::move(version);
            state_.update.skipped_version = configuration_.update.skipped_version;
            settings_dirty_ = true;
        }
    }

    void AutoCheckUpdates() {
        {
            std::scoped_lock lock(state_mutex_);
            if (auto_check_done_ || state_.busy || !configuration_.update.auto_check) return;
            auto_check_done_ = true;
        }
        if (!StartUpdateCheck()) {
            std::scoped_lock lock(state_mutex_);
            auto_check_done_ = false;  // the worker was busy; the next frame tries again
        }
    }

    void CheckForUpdates() {
        StartUpdateCheck();
    }

    void InstallUpdate() {
        const auto snapshot = Snapshot();
        if (snapshot.busy || !snapshot.update.CanInstall()) return;
        g_update_download.cancel.store(false);
        g_update_download.bytes.store(0);
        g_update_download.total.store(0);
        QueueUpdate([this] { InstallUpdateImpl(); }, LauncherUpdateFailure::Install);
    }

    void SetTheme(std::string theme) {
        ChangeSettings([theme = std::move(theme)](auto& configuration) { configuration.theme = theme; });
    }

    void SetCustomColors(const unitymem::CabbirdUiCustomColors& colors) {
        std::scoped_lock lock(state_mutex_);
        if (state_.busy || configuration_.custom_colors == colors) return;
        configuration_.custom_colors = colors;
        state_.custom_colors = colors;
        settings_dirty_ = true;
    }

    void SetLanguage(const cabbird::LanguagePreference preference) {
        ChangeSettings([preference](auto& configuration) {
            configuration.language = cabbird::LanguagePreferenceName(preference);
        }, true);
    }

    void SetFrameRateLimit(const int limit) {
        ChangeSettings([limit](auto& configuration) {
            configuration.frame_rate_limit = cabbird::launcher::LauncherFrameRatePreset(limit);
        });
    }

    void SetBackground(const std::string& path, const int mode) {
        ChangeSettings([path, mode](auto& configuration) {
            configuration.artwork.background = path;
            configuration.artwork.background_mode = mode;
        });
    }

    void SetSurfaceOpacity(const float opacity) {
        ChangeSettings([opacity](auto& configuration) { configuration.artwork.surface_opacity = opacity; });
    }

    // The file dialog only returns a name. Copying and image decoding stay on Worker.
    void ImportBackground(std::filesystem::path source) {
        Queue(cabbird::MessageId::LauncherStatusSavingSettings, [this, source = std::move(source)] {
            const auto directory = runtime_directory_ / L"assets" / L"theme";
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (error) {
                PublishSettingsFailure(error.message());
                return;
            }
            const auto target = directory / source.filename();
            const bool same = std::filesystem::equivalent(source, target, error) && !error;
            error.clear();
            if (!same) std::filesystem::copy_file(source, target,
                std::filesystem::copy_options::overwrite_existing, error);
            if (error) {
                PublishSettingsFailure(error.message());
                return;
            }
            cabbird::launcher::LauncherConfiguration updated;
            {
                std::scoped_lock lock(state_mutex_);
                updated = configuration_;
            }
            updated.artwork.background = PathUtf8(std::filesystem::path(L"assets") / L"theme" / source.filename());
            if (updated.artwork.background_mode <= 0) updated.artwork.background_mode = 2;
            SaveChangedSettings(std::move(updated), false, true);
        });
    }

    // Color edits repaint from memory while dragging, then write once on release.
    void FlushSettings() {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy || !settings_dirty_) return;
        }
        ChangeSettings([](auto&) {});
    }

    void SetToggleKey(const std::uint32_t key) {
        Queue(cabbird::MessageId::LauncherStatusSavingSettings, [this, key] {
            if (!SaveToggleKeyImpl(key)) {
                PublishSettingsFailure("menu toggle preference could not be written");
                return;
            }
            std::scoped_lock lock(state_mutex_);
            state_.toggle_key = key;
            state_.settings_error.clear();
            state_.message = MakeLauncherMessage(
                cabbird::MessageId::LauncherStatusSettingsSaved);
            state_.message_kind = MessageKind::Success;
        });
    }

private:
    using Work = std::function<void()>;

    bool StartUpdateCheck() {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy || state_.update.Busy()) return false;
            state_.update.phase = LauncherUpdatePhase::Checking;
            auto_check_done_ = true;
        }
        // A release lookup must not reserve the launch/install worker for every network timeout.
        // The previous lookup has published its result before another check becomes available.
        if (update_check_worker_.joinable()) update_check_worker_.join();
        update_check_worker_ = std::jthread([this](std::stop_token stop) {
            RunUpdate([this, stop] { CheckForUpdatesImpl(stop); }, LauncherUpdateFailure::Check);
        });
        return true;
    }

    bool QueueSilent(Work work) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return false;
            state_.busy = true;
        }
        {
            std::scoped_lock lock(queue_mutex_);
            queue_.push_back(std::move(work));
        }
        queue_changed_.notify_one();
        return true;
    }

    bool QueueUpdate(Work work, LauncherUpdateFailure failure) {
        return QueueSilent([this, work = std::move(work), failure] {
            RunUpdate(work, failure);
        });
    }

    void RunUpdate(const Work& work, LauncherUpdateFailure failure) {
        try {
            work();
        } catch (...) {
            std::scoped_lock lock(state_mutex_);
            state_.update.phase = LauncherUpdatePhase::Failed;
            state_.update.failure = failure;
            state_.update.detail = "update operation could not be completed";
        }
    }

    void CheckForUpdatesImpl(std::stop_token stop) {
        // The mirror that answered last time, if any: asked first, so a launch on a network that
        // cannot reach api.github.com does not spend its budget on an address known not to answer.
        std::string endpoint;
        {
            std::scoped_lock lock(state_mutex_);
            endpoint = configuration_.update.endpoint;
            state_.update.phase = LauncherUpdatePhase::Checking;
            state_.update.failure = LauncherUpdateFailure::None;
            state_.update.detail.clear();
            state_.update.http_status = 0;
        }
        const std::string api_url = "https://api.github.com/repos/" +
            std::string(cabbird::launcher::kReleaseRepository) + "/releases/latest";
        std::optional<cabbird::launcher::ReleaseInfo> release;
        std::string answered_by;
        int status{};
        bool answered{};
        bool first = true;
        for (const std::string& url : LauncherLookupCandidates(api_url, endpoint)) {
            if (stop.stop_requested()) return;
            // The first endpoint gets both routes -- the system proxy the machine is configured with,
            // and a direct connection -- because a stale proxy is a common reason for a lookup to
            // return nothing. The rest are tried once: whoever answers, answers.
            int attempt_status{};
            std::string body = LauncherHttpGetUrl(url, false, &attempt_status, 3000);
            if (body.empty() && first && !stop.stop_requested())
                body = LauncherHttpGetUrl(url, true, &attempt_status, 3000);
            first = false;
            if (attempt_status != 0) status = attempt_status;
            if (body.empty()) continue;
            answered = true;
            if (auto parsed = cabbird::launcher::ParseLatestRelease(body); parsed.has_value()) {
                release = std::move(parsed);
                answered_by = url;
                break;
            }
        }

        {
            std::scoped_lock lock(state_mutex_);
            if (release.has_value() && !answered_by.empty()) {
                // Remember which endpoint answered, so the next launch starts there instead of
                // waiting out the addresses this network cannot reach.
                const std::string used = LauncherEndpointOfUrl(answered_by);
                if (configuration_.update.endpoint != used) {
                    configuration_.update.endpoint = used;
                    settings_dirty_ = true;
                }
            }
            state_.update.http_status = status;
            if (!release.has_value()) {
                state_.update.phase = LauncherUpdatePhase::Failed;
                if (status == 0 && !answered) {
                    state_.update.failure = LauncherUpdateFailure::Network;
                } else if (status == 403 || status == 429) {
                    state_.update.failure = LauncherUpdateFailure::RateLimited;
                } else if (status != 0 && status != 200) {
                    state_.update.failure = LauncherUpdateFailure::Http;
                } else {
                    state_.update.failure = LauncherUpdateFailure::NotRelease;
                }
                return;
            }
            state_.update.remote_tag = release->tag;
            state_.update.remote_version = release->version;
            state_.update.page_url = release->page_url;
            state_.update.notes = release->notes;
            const auto* const asset = cabbird::launcher::FindRuntimeAsset(*release);
            state_.update.asset = asset != nullptr
                ? *asset : cabbird::launcher::ReleaseAsset{};
            switch (cabbird::launcher::CompareRelease(CABBIRD_SDK_VERSION_STRING, *release)) {
            case cabbird::launcher::ReleaseComparison::Newer:
                if (asset != nullptr) {
                    state_.update.phase = LauncherUpdatePhase::Available;
                } else {
                    state_.update.phase = LauncherUpdatePhase::Failed;
                    state_.update.failure = LauncherUpdateFailure::Check;
                    state_.update.detail = "release publishes no verified runtime package for this version";
                }
                break;
            case cabbird::launcher::ReleaseComparison::UpToDate:
                state_.update.phase = LauncherUpdatePhase::UpToDate;
                break;
            case cabbird::launcher::ReleaseComparison::Unknown:
                state_.update.phase = LauncherUpdatePhase::Failed;
                state_.update.failure = LauncherUpdateFailure::Check;
                state_.update.detail = "release version could not be compared with this build";
                break;
            }
        }
    }

    void InstallUpdateImpl() {
        cabbird::launcher::ReleaseAsset asset;
        {
            std::scoped_lock lock(state_mutex_);
            if (!state_.update.CanInstall()) return;
            asset = state_.update.asset;
            if (asset.download_url.empty()) return;
            state_.update.phase = LauncherUpdatePhase::Downloading;
            state_.update.failure = LauncherUpdateFailure::None;
            state_.update.detail.clear();
        }
        const auto fail = [this](std::string detail) {
            std::scoped_lock lock(state_mutex_);
            state_.update.phase = LauncherUpdatePhase::Failed;
            state_.update.failure = LauncherUpdateFailure::Install;
            state_.update.detail = std::move(detail);
        };
        std::error_code error;
        GUID download_id{};
        wchar_t download_name[40]{};
        if (FAILED(CoCreateGuid(&download_id)) ||
            StringFromGUID2(download_id, download_name, static_cast<int>(std::size(download_name))) == 0) {
            fail("update download directory could not be allocated");
            return;
        }
        const auto directory = std::filesystem::temp_directory_path(error) /
            (std::wstring(L"cabbird-launcher-update-") + download_name);
        if (error) {
            fail("temporary directory is unavailable");
            return;
        }
        if (!std::filesystem::create_directory(directory, error)) {
            fail("update download directory could not be created");
            return;
        }
        struct DownloadDirectory final {
            std::filesystem::path path;
            ~DownloadDirectory() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } cleanup{directory};
        const auto archive = directory / L"runtime.zip";

        // The package is fetched from the release's own address, once: the gh-proxy mirrors exist to
        // reach the API when github.com does not answer, not to carry the payload, and a package that
        // arrived through a third party would be one more thing to trust. A user whose network cannot
        // reach github.com at all has the netdisk buttons in this dialog for exactly that case.
        cabbird::launcher::PackageVerification verification;
        std::string rejected;
        bool downloaded = LauncherDownloadUrl(asset.download_url, archive, &g_update_download);
        if (downloaded) {
            verification = cabbird::launcher::VerifyReleasePackage(archive, asset);
            if (!verification.Ok()) {
                rejected = std::string("package failed verification (") +
                    (verification.check == cabbird::launcher::PackageCheck::Size
                        ? "size " + std::to_string(verification.size) + " != " +
                            std::to_string(asset.size)
                        : verification.check == cabbird::launcher::PackageCheck::Digest
                            ? "sha256 " + verification.sha256
                            : std::string("unreadable")) + ')';
                LauncherUpdateLog(rejected);
                std::filesystem::remove(archive, error);
                downloaded = false;
            }
        } else {
            LauncherUpdateLog("package download failed: " + asset.download_url);
        }
        if (!downloaded || g_update_download.cancel.load()) {
            if (g_update_download.cancel.load()) {
                // Cancelling is not a failure: the release is still there to install, and the dialog
                // goes back to offering it.
                std::scoped_lock lock(state_mutex_);
                state_.update.phase = LauncherUpdatePhase::Available;
                state_.update.failure = LauncherUpdateFailure::None;
                state_.update.detail.clear();
                return;
            }
            fail(rejected.empty()
                ? std::string("update package could not be downloaded")
                : rejected);
            return;
        }
        LauncherUpdateLog("package verified: " + asset.name + " size=" +
            std::to_string(verification.size) + " sha256=" + verification.sha256);
        {
            std::scoped_lock lock(state_mutex_);
            state_.update.phase = LauncherUpdatePhase::Installing;
        }
        if (g_update_download.cancel.load()) {
            std::scoped_lock lock(state_mutex_);
            state_.update.phase = LauncherUpdatePhase::Available;
            return;
        }
        const auto staging = directory / L"staging";
        const auto extracted = cabbird::ExtractZip(archive, staging);
        if (!extracted.Ok()) {
            fail(extracted.message);
            return;
        }
        if (g_update_download.cancel.load()) {
            std::scoped_lock lock(state_mutex_);
            state_.update.phase = LauncherUpdatePhase::Available;
            return;
        }
        const auto published =
            cabbird::launcher::PublishStagedRelease(staging, payload_root_);
        if (!published.Ok()) {
            fail(published.failure);
            return;
        }
        std::string restart_failure;
        if (!RelaunchUpdatedLauncher(restart_failure)) {
            fail(std::move(restart_failure));
            return;
        }
        std::scoped_lock lock(state_mutex_);
        state_.update.phase = LauncherUpdatePhase::Installed;
        state_.restart_pending = true;
    }

    bool RelaunchUpdatedLauncher(std::string& failure) {
        const auto executable = payload_root_ / cabbird::launcher::kLauncherExecutableName;
        // Wait before reading configuration or removing the renamed image. The old controller
        // persists its final settings during destruction, after the window closes.
        std::wstring command = L"\"" + executable.wstring() + L"\" --wait-for-update " +
            std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{.cb = sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_UNICODE_ENVIRONMENT, nullptr, executable.parent_path().c_str(),
                &startup, &process) == FALSE) {
            failure = "new launcher could not be started (Win32 " +
                std::to_string(GetLastError()) + ')';
            return false;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }

    void ChangeSettings(std::function<void(cabbird::launcher::LauncherConfiguration&)> change,
        const bool reload_language = false) {
        Queue(cabbird::MessageId::LauncherStatusSavingSettings,
            [this, change = std::move(change), reload_language] {
            cabbird::launcher::LauncherConfiguration updated;
            {
                std::scoped_lock lock(state_mutex_);
                updated = configuration_;
                settings_dirty_ = false;
            }
            change(updated);
            SaveChangedSettings(std::move(updated), reload_language);
        });
    }

    void SaveChangedSettings(cabbird::launcher::LauncherConfiguration updated, const bool reload_language,
        const bool require_background = false) {
            std::shared_ptr<const cabbird::Translator> translator;
            if (reload_language) {
                const auto locale = cabbird::ResolveUserLocale(
                    cabbird::ParseLanguagePreference(updated.language).preference);
                translator = cabbird::LoadHostCatalog(locale.locale, runtime_directory_ / L"locales" / L"host").translator;
                if (translator == nullptr) {
                    PublishSettingsFailure("launcher language catalog could not be loaded");
                    return;
                }
            }
            bool reload_theme{};
            {
                std::scoped_lock lock(state_mutex_);
                reload_theme = require_background || state_.theme_layout == nullptr || configuration_.theme != updated.theme ||
                    configuration_.artwork != updated.artwork;
            }
            auto theme = reload_theme ? LoadLauncherTheme(runtime_directory_, updated, ++theme_revision_) : nullptr;
            if (require_background && (theme == nullptr || !theme->HasBackground())) {
                PublishSettingsFailure("the selected background image could not be decoded");
                return;
            }
            const auto saved = cabbird::launcher::SaveLauncherConfiguration(configuration_path_, updated);
            if (!saved.Ok()) {
                PublishSettingsFailure(saved.message);
                return;
            }
            std::scoped_lock lock(state_mutex_);
            updated.update = configuration_.update;
            configuration_ = std::move(updated);
            state_.theme = configuration_.theme;
            state_.language = configuration_.language;
            state_.artwork = configuration_.artwork;
            state_.custom_colors = configuration_.custom_colors;
            state_.frame_rate_limit = configuration_.frame_rate_limit;
            state_.update.auto_check = configuration_.update.auto_check;
            state_.update.skipped_version = configuration_.update.skipped_version;
            state_.settings_error.clear();
            if (translator != nullptr) state_.translator = std::move(translator);
            if (theme != nullptr) state_.theme_layout = std::move(theme);
            state_.message = MakeLauncherMessage(cabbird::MessageId::LauncherStatusSettingsSaved);
            state_.message_kind = MessageKind::Success;
    }

    [[nodiscard]] cabbird::RuntimeLaunchResult ResolveLaunchRuntime() const {
        auto bundled = cabbird::ResolveRuntimeLaunch({runtime_directory_});
        if (bundled.Ok()) return bundled;
        auto installed = cabbird::ResolveRuntimeLaunch({GameDirectory() / L"Cabbird"});
        return installed.Ok() ? installed : bundled;
    }

    void PublishRuntimeFailure(const cabbird::RuntimeLaunchResult& selected) {
        std::scoped_lock lock(state_mutex_);
        state_.runtime_version.clear();
        state_.runtime_message = selected.message;
        state_.core_available = false;
        state_.message = MakeLauncherMessage(
            cabbird::MessageId::LauncherStatusCoreUnavailable, {}, selected.message);
        state_.message_kind = MessageKind::Error;
    }

    void PublishMessage(cabbird::MessageId id, MessageKind kind, std::string detail = {}) {
        std::scoped_lock lock(state_mutex_);
        state_.message = MakeLauncherMessage(id, {}, std::move(detail));
        state_.message_kind = kind;
    }

    void PublishSettingsFailure(std::string detail) {
        std::scoped_lock lock(state_mutex_);
        state_.settings_error = detail;
        state_.message = MakeLauncherMessage(cabbird::MessageId::LauncherStatusUnexpectedFailure, {}, std::move(detail));
        state_.message_kind = MessageKind::Error;
    }

    [[nodiscard]] std::filesystem::path GameDirectory() const {
        std::scoped_lock lock(state_mutex_);
        return state_.game_directory;
    }

    [[nodiscard]] std::filesystem::path LauncherExecutable() const {
        std::scoped_lock lock(state_mutex_);
        return state_.launcher_executable;
    }

    [[nodiscard]] std::filesystem::path RuntimeSettingsRoot() const {
        const auto game_directory = GameDirectory();
        if (!game_directory.empty()) {
            const auto installed = game_directory / L"Cabbird";
            std::error_code error;
            if (std::filesystem::is_regular_file(
                    installed / L"Cabbird.Core.dll", error) && !error) {
                return installed;
            }
        }
        return runtime_directory_;
    }

    void RefreshHotkeyImpl() {
        const auto root = RuntimeSettingsRoot();
        const auto config = unitymem::AnalyzerConfig::Load(root / L"cabbird.ini");
        std::scoped_lock lock(state_mutex_);
        state_.toggle_key = config.platform_toggle_key;
    }

    [[nodiscard]] bool SaveToggleKeyImpl(const std::uint32_t key) const {
        const std::wstring value = std::to_wstring(key);
        return WritePrivateProfileStringW(
            L"Platform", L"ToggleKey", value.c_str(),
            (RuntimeSettingsRoot() / L"cabbird.ini").c_str()) != FALSE;
    }

    bool Queue(cabbird::MessageId activity, Work work) {
        {
            std::scoped_lock lock(state_mutex_);
            if (state_.busy) return false;
            state_.busy = true;
            state_.message = MakeLauncherMessage(activity);
            state_.message_kind = MessageKind::Neutral;
        }
        {
            std::scoped_lock lock(queue_mutex_);
            queue_.push_back(std::move(work));
        }
        queue_changed_.notify_one();
        return true;
    }

    void WorkerMain(std::stop_token stop) {
        while (!stop.stop_requested()) {
            Work work;
            {
                std::unique_lock lock(queue_mutex_);
                queue_changed_.wait(lock, stop, [this] { return !queue_.empty(); });
                if (stop.stop_requested()) break;
                work = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                work();
            } catch (...) {
                std::scoped_lock lock(state_mutex_);
                state_.message = MakeLauncherMessage(
                    cabbird::MessageId::LauncherStatusUnexpectedFailure);
                state_.message_kind = MessageKind::Error;
            }
            std::scoped_lock lock(state_mutex_);
            state_.busy = false;
        }
    }

    void RefreshRecoveryImpl() {
        const auto runtime_root = GameDirectory() / L"Cabbird";
        std::error_code error;
        if (!std::filesystem::is_directory(runtime_root, error) || error) {
            std::scoped_lock lock(state_mutex_);
            state_.recovery.reset();
            state_.recovery_message.clear();
            return;
        }
        cabbird::RuntimeRecoveryStore store(runtime_root);
        PublishRecovery(store.Load(), false);
    }

    void PublishRecovery(cabbird::RuntimeRecoveryResult result, bool announce) {
        std::scoped_lock lock(state_mutex_);
        if (result.Ok()) {
            state_.recovery = std::move(result.state);
            state_.recovery_message.clear();
            if (announce) {
                state_.message = MakeLauncherMessage(
                    cabbird::MessageId::LauncherStatusRecoveryRestored);
                state_.message_kind = MessageKind::Success;
            }
            return;
        }
        state_.recovery.reset();
        if (result.error == cabbird::RuntimeRecoveryError::StateUnavailable) {
            state_.recovery_message.clear();
            return;
        }
        state_.recovery_message = std::move(result.message);
        if (announce) {
            state_.message = MakeLauncherMessage(
                cabbird::MessageId::LauncherStatusRecoveryRestoreFailed, {},
                state_.recovery_message);
            state_.message_kind = MessageKind::Error;
        }
    }

    void InitializePathsImpl() {
        cabbird::launcher::RemoveUpdateLeftovers(payload_root_);
        const auto loaded = cabbird::launcher::LoadLauncherConfiguration(configuration_path_);
        const auto game_processes = cabbird::launcher::EnumerateAttachableProcesses();
        const auto launcher_processes =
            cabbird::launcher::EnumerateAttachableProcesses(L"launcher.exe");
        // Only 国服 is offered: the CN/Global buttons were removed at the operator's request.
        // Pinning here (rather than trusting the JSON) is what makes the removal real -- a
        // `selectedClient: global` already on disk would otherwise keep selecting the other
        // client's paths with no UI left to change it.  PersistConfigurationImpl writes this
        // back on the next save, so the file converges on mainlandChina.
        auto configuration = loaded.configuration;
        if (configuration.language.empty()) {
            const auto config = unitymem::AnalyzerConfig::Load(runtime_directory_ / L"cabbird.ini");
            configuration.language = cabbird::LanguagePreferenceName(config.platform_language);
        }
        configuration.selected_client = cabbird::launcher::AzurPromiliaClient::MainlandChina;
        const auto discover = [this, &game_processes, &configuration](
                                  const cabbird::launcher::AzurPromiliaClient client,
                                  const auto& launcher_processes) {
            cabbird::launcher::LauncherDiscoveryOptions options;
            options.client = client;
            options.payload_root = payload_root_;
            options.allow_unpaired_game_discovery =
                configuration.selected_client == client;
            if (configuration.selected_client == client) {
                for (const auto& process : game_processes) {
                    if (!process.executable_path.empty()) {
                        options.running_game_executables.push_back(process.executable_path);
                    }
                }
            }
            for (const auto& process : launcher_processes) {
                if (!process.executable_path.empty()) {
                    options.running_launcher_executables.push_back(process.executable_path);
                }
            }
            return cabbird::launcher::DiscoverLauncherConfiguration(
                client == cabbird::launcher::AzurPromiliaClient::Global
                    ? configuration.global : configuration.mainland_china,
                options);
        };
        configuration.mainland_china = discover(
            cabbird::launcher::AzurPromiliaClient::MainlandChina, launcher_processes);
        configuration.global = discover(
            cabbird::launcher::AzurPromiliaClient::Global, launcher_processes);
        {
            std::scoped_lock lock(state_mutex_);
            configuration_ = std::move(configuration);
            const auto& selected = configuration_.Selected();
            state_.selected_client = configuration_.selected_client;
            state_.theme = configuration_.theme;
            state_.language = configuration_.language;
            state_.artwork = configuration_.artwork;
            state_.custom_colors = configuration_.custom_colors;
            state_.frame_rate_limit = configuration_.frame_rate_limit;
            state_.update.auto_check = configuration_.update.auto_check;
            state_.update.skipped_version = configuration_.update.skipped_version;
            state_.game_directory = selected.game_directory;
            state_.launcher_executable = selected.launcher_executable;
        }
        const auto saved = PersistConfigurationImpl();
        RefreshHotkeyImpl();
        auto theme = LoadLauncherTheme(runtime_directory_, configuration_, ++theme_revision_);
        {
            std::scoped_lock lock(state_mutex_);
            state_.theme_layout = std::move(theme);
        }
        RefreshRecoveryImpl();
        RefreshProcessesImpl();
        StartTelemetryImpl();
        if (!saved.Ok()) {
            PublishMessage(cabbird::MessageId::LauncherStatusUnexpectedFailure,
                MessageKind::Error, saved.message);
        }
    }

    void ReconcileRelatedPathsImpl() {
        cabbird::launcher::LauncherClientConfiguration preferred;
        cabbird::launcher::AzurPromiliaClient client{};
        {
            std::scoped_lock lock(state_mutex_);
            preferred.game_directory = state_.game_directory;
            preferred.launcher_executable = state_.launcher_executable;
            client = configuration_.selected_client;
        }
        cabbird::launcher::LauncherDiscoveryOptions options;
        options.client = client;
        options.payload_root = payload_root_;
        options.allow_unpaired_game_discovery = true;
        const auto discovered = cabbird::launcher::DiscoverLauncherConfiguration(
            preferred, options);
        std::scoped_lock lock(state_mutex_);
        configuration_.Selected() = discovered;
        state_.game_directory = discovered.game_directory;
        state_.launcher_executable = discovered.launcher_executable;
    }

    [[nodiscard]] cabbird::launcher::LauncherConfigurationSaveResult
    PersistConfigurationImpl() const {
        cabbird::launcher::LauncherConfiguration configuration;
        {
            std::scoped_lock lock(state_mutex_);
            configuration = configuration_;
        }
        return cabbird::launcher::SaveLauncherConfiguration(
            configuration_path_, configuration);
    }

    void StartTelemetryImpl() noexcept {
        try {
            const auto day = cabbird::TelemetryDay(std::chrono::system_clock::now());
            if (telemetry_attempt_day_ == day) return;
            cabbird::TelemetryOptions options;
            const auto config = unitymem::AnalyzerConfig::Load(runtime_directory_ / L"cabbird.ini");
            options.enabled = config.telemetry_enabled;
            options.endpoint = config.telemetry_endpoint;
            if (!options.enabled || options.endpoint.empty()) return;
            const auto snapshot = Snapshot();
            if (!snapshot.theme_layout) return;
            options.language = cabbird::LocaleName(cabbird::ResolveUserLocale(
                cabbird::ParseLanguagePreference(snapshot.language).preference).locale);
            cabbird::LauncherTelemetryUsage usage;
            usage.theme = snapshot.theme;
            usage.custom_background = snapshot.theme_layout->HasBackground() && !snapshot.artwork.background.empty();
            telemetry_attempt_day_ = day;
            telemetry_worker_ = std::jthread([options = std::move(options), usage = std::move(usage)](
                std::stop_token stop) { cabbird::ReportLauncherUsage(options, usage, stop); });
        } catch (...) {}
    }

    void RefreshProcessesImpl(ProcessAnnounce announce = ProcessAnnounce::Always) {
        auto processes = cabbird::launcher::EnumerateAttachableProcesses();
        const auto game_directory = GameDirectory();
        if (!game_directory.empty()) {
            std::erase_if(processes, [&game_directory](const auto& process) {
                return process.executable_path.empty() ||
                    !PathsEqual(process.executable_path.parent_path(), game_directory);
            });
        }
        const auto runtime = ResolveLaunchRuntime();
        const bool core_available = runtime.Ok();
        std::scoped_lock lock(state_mutex_);
        const bool had_processes = !state_.processes.empty();
        state_.processes = std::move(processes);
        state_.core_available = core_available;
        state_.runtime_version = runtime.version;
        state_.runtime_message = runtime.message;
        if (std::none_of(
                state_.processes.begin(), state_.processes.end(),
                [this](const auto& process) {
                    return process.process_id == state_.attached_process;
                })) {
            state_.attached_process = 0;
        }
        if (announce == ProcessAnnounce::Never) return;
        if (announce == ProcessAnnounce::OnChange && had_processes == !state_.processes.empty()) return;
        if (!core_available) {
            state_.message = runtime.message.empty()
                ? MakeLauncherMessage(cabbird::MessageId::LauncherStatusCoreUnavailable)
                : MakeLauncherMessage(
                    cabbird::MessageId::LauncherStatusCoreUnavailable, {},
                    runtime.message);
            state_.message_kind = MessageKind::Error;
        } else if (state_.processes.empty()) {
            state_.message = MakeLauncherMessage(
                cabbird::MessageId::LauncherStatusNoProcesses);
            state_.message_kind = MessageKind::Neutral;
        } else {
            const std::string pid = std::to_string(state_.attached_process != 0
                ? state_.attached_process : state_.processes.front().process_id);
            state_.message = MakeLauncherMessage(state_.attached_process != 0
                ? cabbird::MessageId::LauncherStatusLaunchAttached
                : cabbird::MessageId::LauncherStatusProcessFound, {pid});
            state_.message_kind = MessageKind::Neutral;
        }
    }

    std::filesystem::path payload_root_;
    std::filesystem::path runtime_directory_;
    std::filesystem::path configuration_path_;
    mutable std::mutex state_mutex_;
    cabbird::launcher::LauncherConfiguration configuration_;
    LauncherSnapshot state_;
    bool settings_dirty_{};
    std::uint64_t theme_revision_{};
    std::mutex queue_mutex_;
    std::condition_variable_any queue_changed_;
    std::deque<Work> queue_;
    std::chrono::steady_clock::time_point last_process_poll_{};
    bool auto_check_done_{};
    std::jthread update_check_worker_;
    std::string telemetry_attempt_day_;
    std::jthread telemetry_worker_;
    std::jthread worker_;
};

struct Graphics final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap_chain;
    cabbird::launcher::DesktopSurface desktop_surface;
    ComPtr<ID3D11RenderTargetView> render_target;
    ComPtr<ID3D11ShaderResourceView> logo;
    UINT logo_source_extent{};
    UINT logo_mip{UINT_MAX};
    std::uint64_t theme_revision{};
    ComPtr<ID3D11ShaderResourceView> theme_background;
    std::vector<ComPtr<ID3D11ShaderResourceView>> theme_stickers;
    bool redraw_required{true};
};

struct LauncherDrawing final {
    std::vector<std::vector<std::byte>> lists;
    std::vector<std::byte> scratch;
    int saved_lists{};
    ImRect previous_overlay{};

    int Compare(const ImDrawData& data, const bool force) {
        const int count = data.CmdListsCount;
        const int previous = static_cast<int>(lists.size());
        int first_change = force ? 0 : (std::min)(previous, count);
        lists.resize(count);
        for (int i = 0; i < count; ++i) {
            scratch.clear();
            const bool reusable = cabbird::glass::AppendDrawListSnapshot(*data.CmdLists[i], scratch);
            if (!reusable || scratch != lists[i]) {
                first_change = (std::min)(first_change, i);
                if (!reusable) scratch.clear();
                lists[i].swap(scratch);
            }
        }
        if (first_change < saved_lists) saved_lists = 0;
        if (!force && previous == count && first_change == count) return -1;
        return first_change;
    }

    static void BuildRange(ImDrawData& batch, const ImDrawData& data, const int first, const int end) {
        batch.Valid = true;
        batch.DisplayPos = data.DisplayPos;
        batch.DisplaySize = data.DisplaySize;
        batch.FramebufferScale = data.FramebufferScale;
        batch.OwnerViewport = data.OwnerViewport;
        for (int i = first; i < end; ++i) batch.AddDrawList(data.CmdLists[i]);
    }

    std::optional<RECT> Render(Graphics& graphics, const ImDrawData& data, const int first_change) {
        const bool restore = saved_lists > 0 &&
            (saved_lists == first_change || first_change == data.CmdListsCount);
        if (!restore) {
            graphics.context->ClearRenderTargetView(graphics.render_target.Get(),
                cabbird::launcher::kDesktopClearColor);
            if (first_change > 0) {
                ImDrawData underlay;
                BuildRange(underlay, data, 0, first_change);
                cabbird::glass::RenderDrawData(graphics.context.Get(), &underlay, graphics.render_target.Get());
                saved_lists = cabbird::glass::SaveDesktopLayer(graphics.render_target.Get()) ? first_change : 0;
            }
        }
        ImDrawData overlay;
        BuildRange(overlay, data, restore ? saved_lists : first_change, data.CmdListsCount);
        const ImRect bounds = cabbird::glass::BoundDesktopCaptures(overlay);
        std::optional<RECT> damage;
        if (restore) {
            ImRect changed = previous_overlay;
            changed.Add(bounds);
            if (changed.IsInverted()) changed = ImRect(0, 0, data.DisplaySize.x, data.DisplaySize.y);
            damage = RECT{static_cast<LONG>(std::floor(changed.Min.x)), static_cast<LONG>(std::floor(changed.Min.y)),
                static_cast<LONG>(std::ceil(changed.Max.x)), static_cast<LONG>(std::ceil(changed.Max.y))};
            cabbird::glass::RestoreDesktopLayer(graphics.render_target.Get(), *damage);
        }
        previous_overlay = bounds;
        if (overlay.CmdListsCount > 0) cabbird::glass::RenderDrawData(graphics.context.Get(), &overlay, graphics.render_target.Get());
        return damage;
    }
};

Graphics* g_graphics{};
float g_launcher_dpi_scale{1.0f};
bool g_launcher_dpi_changed{};
bool g_launcher_hotkey_capture{};
std::optional<std::uint32_t> g_launcher_captured_hotkey;

bool IsLauncherHotkeyModifier(const std::uint32_t key) noexcept {
    return key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU ||
        key == VK_LCONTROL ||
        key == VK_RCONTROL || key == VK_LMENU || key == VK_RMENU;
}

void BeginLauncherHotkeyCapture() {
    g_launcher_captured_hotkey.reset();
    g_launcher_hotkey_capture = true;
}

// Capture the focused window's key-down event rather than polling global state
// during a later frame: a short press may already be released by that point.
bool HandleLauncherHotkeyMessage(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (!g_launcher_hotkey_capture) return false;
    if (message == WM_KILLFOCUS) {
        g_launcher_hotkey_capture = false;
        return false;
    }
    if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN) return false;
    if ((static_cast<std::uintptr_t>(lparam) & (std::uintptr_t{1} << 30U)) != 0) return true;
    auto key = static_cast<std::uint32_t>(wparam);
    if (key == VK_SHIFT) {
        const UINT scan = (static_cast<std::uintptr_t>(lparam) >> 16U) & 0xffU;
        key = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
    }
    if (key == VK_ESCAPE) {
        g_launcher_hotkey_capture = false;
        return true;
    }
    if (key < 8 || key > 0xff) return false;
    if (IsLauncherHotkeyModifier(key)) return true;
    g_launcher_captured_hotkey = key;
    g_launcher_hotkey_capture = false;
    return true;
}

std::optional<std::uint32_t> CaptureLauncherHotkey() {
    return std::exchange(g_launcher_captured_hotkey, std::nullopt);
}

float DpiScale(UINT dpi) noexcept {
    return dpi == 0 ? 1.0f : static_cast<float>(dpi) / kDefaultDpi;
}

float Scale(float value) noexcept {
    return std::round(value * g_launcher_dpi_scale);
}

ImVec2 Scale(float x, float y) noexcept {
    return ImVec2(Scale(x), Scale(y));
}

float ButtonHeight(float logical_height) noexcept {
    return (std::max)(Scale(logical_height), ImGui::GetFrameHeight());
}

void ApplyLauncherDpiScale() noexcept {
    ImGuiIO& io = ImGui::GetIO();
    // The launcher retains its single uploaded bake, including after its source
    // records are released. Normalize that bake directly when DPI changes.
    if (io.FontDefault != nullptr && io.FontDefault->FontSize > 0.0f)
        io.FontDefault->Scale = kLauncherBaseFontPixels / io.FontDefault->FontSize;
    io.FontGlobalScale = g_launcher_dpi_scale * kLauncherFontScale;
    unitymem::ApplyCabbirdUiStyle();
    const auto& background = unitymem::CabbirdUiTheme().window_background;
    if (0.2126f * background.red + 0.7152f * background.green + 0.0722f * background.blue >= 0.5f) {
        ImGui::GetStyle().Colors[ImGuiCol_Text] = {0.0f, 0.0f, 0.0f, 1.0f};
        ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] = {0.29f, 0.30f, 0.29f, 1.0f};
    }
    g_launcher_dpi_changed = false;
    if (g_graphics != nullptr) g_graphics->redraw_required = true;
}

bool CreateRenderTarget(Graphics& graphics) {
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(graphics.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return false;
    graphics.redraw_required = true;
    return SUCCEEDED(graphics.device->CreateRenderTargetView(
        back_buffer.Get(), nullptr, &graphics.render_target));
}

bool CreateGraphics(HWND window, Graphics& graphics) {
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION, &graphics.device, &selected, &graphics.context))) return false;
    RECT client{};
    if (!GetClientRect(window, &client)) return false;
    return SUCCEEDED(graphics.desktop_surface.Create(window, graphics.device.Get(),
        static_cast<UINT>(client.right - client.left), static_cast<UINT>(client.bottom - client.top),
        graphics.swap_chain)) && CreateRenderTarget(graphics);
}

void SelectLogoMip(Graphics& graphics) {
    if (graphics.logo == nullptr || graphics.logo_source_extent == 0) return;
    // ImGui's sampler clamps LOD to zero. Point that zero at the DPI-sized mip
    // instead of asking the sampler to reduce the full-resolution artwork.
    const float ratio = static_cast<float>(graphics.logo_source_extent) / Scale(48.0f);
    const UINT mip = ratio > 1.0f ? static_cast<UINT>(std::floor(std::log2(ratio))) : 0;
    if (mip == graphics.logo_mip) return;
    ComPtr<ID3D11Resource> texture;
    graphics.logo->GetResource(&texture);
    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
    graphics.logo->GetDesc(&description);
    description.Texture2D.MostDetailedMip = mip;
    description.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> selected;
    if (SUCCEEDED(graphics.device->CreateShaderResourceView(texture.Get(), &description, &selected))) {
        graphics.logo = std::move(selected);
        graphics.logo_mip = mip;
        graphics.redraw_required = true;
    }
}

void LoadLogo(Graphics& graphics) {
    const HRSRC resource = FindResourceW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kLogoResourceId), RT_RCDATA);
    if (resource == nullptr) return;
    const HGLOBAL loaded = LoadResource(GetModuleHandleW(nullptr), resource);
    const DWORD size = SizeofResource(GetModuleHandleW(nullptr), resource);
    const void* data = loaded == nullptr ? nullptr : LockResource(loaded);
    if (data == nullptr || size == 0) return;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const auto decoded = cabbird::DecodeUiImageRgba8(std::span(bytes, size));
    if (!decoded) return;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = decoded.image.width;
    description.Height = decoded.image.height;
    description.MipLevels = 0;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    description.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(graphics.device->CreateTexture2D(&description, nullptr, &texture))) {
        graphics.context->UpdateSubresource(texture.Get(), 0, nullptr,
            decoded.image.pixels.data(), decoded.image.width * 4U, 0);
        if (SUCCEEDED(graphics.device->CreateShaderResourceView(
                texture.Get(), nullptr, &graphics.logo))) {
            graphics.context->GenerateMips(graphics.logo.Get());
            graphics.logo_source_extent = std::max(decoded.image.width, decoded.image.height);
            graphics.logo_mip = UINT_MAX;
            SelectLogoMip(graphics);
        }
    }
}

ComPtr<ID3D11ShaderResourceView> UploadThemeImage(Graphics& graphics, cabbird::UiRgba8Image& image) {
    ComPtr<ID3D11ShaderResourceView> view;
    if (image.pixels.empty()) return view;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = image.width; description.Height = image.height;
    description.MipLevels = 1; description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(graphics.device->CreateTexture2D(&description, nullptr, &texture))) {
        graphics.context->UpdateSubresource(texture.Get(), 0, nullptr, image.pixels.data(), image.width * 4U, 0);
        static_cast<void>(graphics.device->CreateShaderResourceView(texture.Get(), nullptr, &view));
    }
    std::vector<std::uint8_t>{}.swap(image.pixels);
    return view;
}

void UploadLauncherTheme(Graphics& graphics, const std::shared_ptr<const LauncherTheme>& theme) {
    if (theme == nullptr || graphics.theme_revision == theme->revision) return;
    graphics.theme_revision = theme->revision;
    graphics.theme_background = UploadThemeImage(graphics, theme->background);
    graphics.theme_stickers.clear();
    graphics.theme_stickers.reserve(theme->stickers.size());
    for (auto& image : theme->stickers) graphics.theme_stickers.push_back(UploadThemeImage(graphics, image));
    graphics.redraw_required = true;
}

void DrawLauncherThemeImages(Graphics& graphics, const std::shared_ptr<const LauncherTheme>& theme,
    const ImVec2 origin, const ImVec2 size, const bool front) {
    if (theme == nullptr) return;
    auto* list = front ? ImGui::GetForegroundDrawList() : ImGui::GetWindowDrawList();
    const auto tint = [](const cabbird::CabbirdUiColor color, const float alpha) {
        return ImGui::GetColorU32(ImVec4(color.red, color.green, color.blue, color.alpha * alpha));
    };
    list->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
    const auto& layout = theme->layout;
    if (!front && theme->HasBackground() && graphics.theme_background != nullptr) {
        const auto placement = cabbird::ComputePlatformUiBackgroundPlacement(layout.background.mode,
            static_cast<float>(theme->background.width), static_cast<float>(theme->background.height),
            origin.x, origin.y, size.x, size.y);
        if (placement.Visible()) {
            const bool covers_body = placement.x <= origin.x && placement.y <= origin.y &&
                placement.x + placement.width >= origin.x + size.x &&
                placement.y + placement.height >= origin.y + size.y;
            list->AddImageRounded(reinterpret_cast<ImTextureID>(graphics.theme_background.Get()),
                {placement.x, placement.y}, {placement.x + placement.width, placement.y + placement.height},
                {placement.uv0_x, placement.uv0_y}, {placement.uv1_x, placement.uv1_y},
                tint(layout.background.tint, layout.background.opacity), covers_body ? Scale(20.0f) : 0.0f);
        }
        list->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y},
            tint(cabbird::CabbirdUiSurfaceColor(), layout.surface_opacity), Scale(20.0f));
    }
    for (std::size_t index = 0; index < layout.stickers.size() && index < graphics.theme_stickers.size(); ++index) {
        const auto& sticker = layout.stickers[index];
        if ((sticker.layer == cabbird::PlatformUiStickerLayer::Front) != front ||
            graphics.theme_stickers[index] == nullptr) continue;
        auto fitted = sticker;
        if (fitted.width > 0.0f && fitted.height > 0.0f) fitted.height = 0.0f;
        const auto& image = theme->stickers[index];
        const auto placement = cabbird::ComputePlatformUiStickerPlacement(fitted,
            static_cast<float>(image.width), static_cast<float>(image.height), size.x, size.y);
        if (!placement.Visible()) continue;
        const auto quad = cabbird::ComputePlatformUiImageQuad(placement);
        list->AddImageQuad(reinterpret_cast<ImTextureID>(graphics.theme_stickers[index].Get()),
            {origin.x + quad.x[0], origin.y + quad.y[0]}, {origin.x + quad.x[1], origin.y + quad.y[1]},
            {origin.x + quad.x[2], origin.y + quad.y[2]}, {origin.x + quad.x[3], origin.y + quad.y[3]},
            {quad.u[0], quad.v[0]}, {quad.u[1], quad.v[1]}, {quad.u[2], quad.v[2]}, {quad.u[3], quad.v[3]},
            tint(sticker.tint, sticker.opacity));
    }
    list->PopClipRect();
}

LRESULT WINAPI WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (HandleLauncherHotkeyMessage(message, wparam, lparam)) return 0;
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return TRUE;
    switch (message) {
    case WM_NCCALCSIZE:
        if (wparam != 0) return 0;
        break;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        RECT client{};
        const bool in_client = ScreenToClient(window, &point) && GetClientRect(window, &client);
        const bool caption_control = in_client && point.y >= 0 && point.y < Scale(kCaptionHeight) &&
            point.x >= client.right - Scale(98.0f);
        return cabbird::launcher::FrameHitTest(window, lparam, caption_control);
    }
    case WM_SIZE:
        if (g_graphics != nullptr && g_graphics->swap_chain != nullptr &&
            wparam != SIZE_MINIMIZED) {
            g_graphics->render_target.Reset();
            if (SUCCEEDED(g_graphics->swap_chain->ResizeBuffers(
                    0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0))) {
                static_cast<void>(CreateRenderTarget(*g_graphics));
            }
        }
        return 0;
    case WM_DPICHANGED: {
        g_launcher_dpi_scale = DpiScale(HIWORD(wparam));
        g_launcher_dpi_changed = true;
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested != nullptr) {
            static_cast<void>(SetWindowPos(
                window, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER));
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        cabbird::launcher::ConstrainMaximizedToWorkArea(window, *limits);
        limits->ptMinTrackSize = {
            static_cast<LONG>(Scale(720.0f)), static_cast<LONG>(Scale(560.0f))};
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0U) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

std::optional<std::filesystem::path> ChooseGameDirectory(
    HWND owner, const std::filesystem::path& current,
    const cabbird::Translator& translator) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    DWORD options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    const std::wstring title = Utf8Wide(
        translator.Text(cabbird::MessageId::LauncherDialogGameDirectory));
    dialog->SetTitle(title.c_str());
    if (!current.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                current.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
    }
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(&selected))) return std::nullopt;
    PWSTR raw{};
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) {
        return std::nullopt;
    }
    const std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

std::optional<std::filesystem::path> ChooseLauncherExecutable(
    HWND owner, const std::filesystem::path& current,
    const cabbird::launcher::AzurPromiliaClient client, const cabbird::Translator& translator) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    DWORD options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
            FOS_PATHMUSTEXIST);
    }
    const std::wstring executable_filter = Utf8Wide(
        translator.Text(cabbird::MessageId::LauncherDialogExecutableFilter));
    const wchar_t* launcher_name = client == cabbird::launcher::AzurPromiliaClient::Global
        ? L"launcher.exe" : L"launcher.exe";
    const wchar_t* launcher_label = client == cabbird::launcher::AzurPromiliaClient::Global
        ? L"UNITY Global Launcher" : L"UNITY Launcher";
    const COMDLG_FILTERSPEC filters[]{
        {launcher_label, launcher_name},
        {executable_filter.c_str(), L"*.exe"},
    };
    static_cast<void>(dialog->SetFileTypes(
        static_cast<UINT>(std::size(filters)), filters));
    const std::wstring title = Utf8Wide(
        translator.Text(cabbird::MessageId::LauncherDialogUnityLauncher));
    dialog->SetTitle(title.c_str());
    if (!current.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                current.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
    }
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(&selected))) return std::nullopt;
    PWSTR raw{};
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) {
        return std::nullopt;
    }
    const std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

bool LauncherSurfaceIsLight() noexcept {
    const auto& surface = unitymem::CabbirdUiTheme().window_background;
    return 0.2126f * surface.red + 0.7152f * surface.green + 0.0722f * surface.blue >= 0.5f;
}

ImVec4 LauncherInk() noexcept {
    return LauncherSurfaceIsLight() ? ImVec4(0.0f, 0.0f, 0.0f, 1.0f)
        : ThemeColor(unitymem::CabbirdUiTheme().text);
}

ImVec4 LauncherMutedInk() noexcept {
    return LauncherSurfaceIsLight() ? ImVec4(0.29f, 0.30f, 0.29f, 1.0f)
        : ThemeColor(unitymem::CabbirdUiTheme().text_muted);
}

const char* Text(const cabbird::Translator& translator, cabbird::MessageId id) noexcept {
    return translator.Text(id).data();
}

std::string StableLabel(const cabbird::Translator& translator, cabbird::MessageId id,
    std::string_view stable_id) {
    return cabbird::StableDisplayLabel(translator.Text(id), stable_id);
}

std::string Format(const cabbird::Translator& translator, cabbird::MessageId id,
    std::initializer_list<std::string_view> arguments) {
    return translator.Format(id, std::span<const std::string_view>(arguments.begin(), arguments.size()));
}

std::string RenderMessage(const cabbird::Translator& translator, const LauncherMessage& message) {
    std::vector<std::string_view> arguments;
    arguments.reserve(message.arguments.size());
    for (const auto& argument : message.arguments) arguments.push_back(argument);
    std::string result = arguments.empty() ? std::string(translator.Text(message.id))
        : translator.Format(message.id, arguments);
    if (!message.detail.empty()) result += " — " + message.detail;
    return result;
}

void Tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
}

struct LauncherChipStyle final {
    ImVec4 tint{};
    ImVec4 ink{};
    float tint_strength{-1.0f};
};

bool LauncherChip(const char* label, ImVec2 size, bool enabled = true,
    const LauncherChipStyle& style = {}) {
    const std::string_view raw(label);
    const auto hidden = raw.find("##");
    const auto visible = hidden == raw.npos ? raw : raw.substr(0, hidden);
    if (size.x <= 0.0f) size.x = ImGui::CalcTextSize(visible.data(), visible.data() + visible.size()).x + Scale(32.0f);
    if (size.y <= 0.0f) size.y = ButtonHeight(38.0f);
    const std::string caption = Ellipsize(visible, std::max(1.0f, size.x - Scale(24.0f)));
    const ImVec2 text_size = ImGui::CalcTextSize(caption.c_str());
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    cabbird::glass::SurfaceOptions surface;
    surface.rounding = size.y * 0.5f;
    surface.scale = g_launcher_dpi_scale;
    surface.shadow = false;
    surface.minimum_alpha = enabled ? 0.82f : 0.56f;
    float strength = style.tint_strength >= 0.0f ? style.tint_strength
        : (LauncherSurfaceIsLight() ? 0.24f : 0.20f);
    if (ImGui::IsItemActive()) strength += 0.16f;
    else if (hovered && enabled) strength += 0.10f;
    if (!enabled) strength *= 0.55f;
    surface.tint_strength = strength;
    if (style.tint.w > 0.0f) surface.tint = style.tint;
    auto* draw = ImGui::GetWindowDrawList();
    cabbird::glass::DrawSurface(draw, min, max, surface);
    const ImVec4 ink = style.ink.w > 0.0f ? style.ink : LauncherInk();
    const ImVec4 colour = enabled ? ink : ImVec4(ink.x, ink.y, ink.z, ink.w * 0.45f);
    draw->AddText(ImVec2(min.x + (size.x - text_size.x) * 0.5f,
        min.y + (size.y - text_size.y) * 0.5f), ImGui::GetColorU32(colour), caption.c_str());
    return pressed && enabled;
}

bool IconButton(const char* id, char32_t glyph, const char* tooltip, bool enabled = true) {
    ImGui::PushID(id);
    const float extent = ButtonHeight(38.0f);
    const bool pressed = LauncherChip("##icon", {extent, extent}, enabled);
    const auto origin = ImGui::GetItemRectMin();
    const auto ink_center = cabbird::glass::GlyphInkCenter(ImGui::GetFont(), ImGui::GetFontSize(), Glyph(glyph));
    auto ink = LauncherInk();
    if (!enabled) ink.w *= 0.45f;
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(std::round(origin.x + extent * 0.5f - ink_center.x),
               std::round(origin.y + extent * 0.5f - ink_center.y)),
        ImGui::GetColorU32(ink), Glyph(glyph));
    Tooltip(tooltip);
    ImGui::PopID();
    return pressed;
}

float CommandButtonWidth(char32_t glyph, const char* label) {
    const std::string text = std::string(Glyph(glyph)) + "  " + label;
    return std::ceil(ImGui::CalcTextSize(text.c_str()).x / g_launcher_dpi_scale) + 44.0f;
}

bool CommandButton(const char* id, char32_t glyph, const char* label,
    bool primary, bool enabled, ImVec2 size = {}) {
    const std::string text = std::string(Glyph(glyph)) + "  " + label + "##command";
    LauncherChipStyle style;
    if (primary) {
        style.tint = ThemeColor(unitymem::CabbirdUiTheme().accent);
        style.tint.w = 1.0f;
        style.tint_strength = 0.86f;
        style.ink = ThemeColor(unitymem::CabbirdUiTheme().inverse_text);
    }
    ImGui::PushID(id);
    const bool pressed = LauncherChip(text.c_str(),
        {size.x > 0.0f ? Scale(size.x) : 0.0f, ButtonHeight(size.y > 0.0f ? size.y : 38.0f)},
        enabled, style);
    ImGui::PopID();
    return pressed;
}

struct LauncherActions final {
    std::function<void(std::filesystem::path)> select_game_directory;
    std::function<void(std::filesystem::path)> select_launcher_executable;
    std::function<void()> refresh_processes;
    std::function<void(bool)> set_auto_check_updates;
    std::function<void(std::string)> skip_update_version;
    std::function<void()> check_for_updates;
    std::function<void()> install_update;
    std::function<void()> launch_and_attach;
    std::function<void(std::uint32_t)> set_toggle_key;
    std::function<void(std::string)> set_theme;
    std::function<void(const unitymem::CabbirdUiCustomColors&)> set_custom_colors;
    std::function<void(cabbird::LanguagePreference)> set_language;
    std::function<void(int)> set_frame_rate_limit;
    std::function<void(const std::string&, int)> set_background;
    std::function<void(std::filesystem::path)> import_background;
    std::function<void(float)> set_surface_opacity;
    std::function<void(cabbird::RuntimeRecoveryAxis)> restore_recovery;
};

LauncherActions ActionsFor(LauncherController& controller) {
    return {
        [&controller](std::filesystem::path path) { controller.SelectGameDirectory(std::move(path)); },
        [&controller](std::filesystem::path path) { controller.SelectLauncherExecutable(std::move(path)); },
        [&controller] { controller.RefreshProcesses(); },
        [&controller](bool enabled) { controller.SetAutoCheckUpdates(enabled); },
        [&controller](std::string version) { controller.SkipUpdateVersion(std::move(version)); },
        [&controller] { controller.CheckForUpdates(); },
        [&controller] { controller.InstallUpdate(); },
        [&controller] { controller.LaunchAndAttach(); },
        [&controller](std::uint32_t key) { controller.SetToggleKey(key); },
        [&controller](std::string theme) { controller.SetTheme(std::move(theme)); },
        [&controller](const unitymem::CabbirdUiCustomColors& colors) { controller.SetCustomColors(colors); },
        [&controller](cabbird::LanguagePreference language) { controller.SetLanguage(language); },
        [&controller](int limit) { controller.SetFrameRateLimit(limit); },
        [&controller](const std::string& path, int mode) { controller.SetBackground(path, mode); },
        [&controller](std::filesystem::path path) { controller.ImportBackground(std::move(path)); },
        [&controller](float opacity) { controller.SetSurfaceOpacity(opacity); },
        [&controller](cabbird::RuntimeRecoveryAxis axis) { controller.RestoreRecovery(axis); }};
}

void DrawLauncherTitleBar(HWND window, Graphics& graphics) {
    SelectLogoMip(graphics);
    const float height = Scale(kCaptionHeight);
    const float button_width = Scale(44.0f);
    const float width = ImGui::GetContentRegionAvail().x;
    const auto origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    float title_x = origin.x + Scale(12.0f);
    if (graphics.logo != nullptr) {
        const float mark = Scale(48.0f);
        const float y = origin.y + (height - mark) * 0.5f + Scale(3.0f);
        draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(graphics.logo.Get())),
            {title_x, y}, {title_x + mark, y + mark});
        title_x += mark + Scale(12.0f);
    }
    draw->AddText(ImGui::GetFont(), Scale(28.0f),
        {title_x, origin.y + (height - Scale(28.0f)) * 0.5f + Scale(3.0f)},
        ImGui::GetColorU32(LauncherInk()), "CabbirdLauncher");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 0.0f});
    ImGui::InvisibleButton("##launcher-drag", {std::max(1.0f, width - button_width * 2.0f - Scale(10.0f)), height});
    static bool drag_active{};
    static POINT drag_cursor{}, drag_window{};
    if (window != nullptr && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
        drag_active = false;
    } else if (window != nullptr && ImGui::IsItemActivated() && !IsZoomed(window)) {
        RECT rectangle{};
        drag_active = GetCursorPos(&drag_cursor) && GetWindowRect(window, &rectangle);
        if (drag_active) drag_window = {rectangle.left, rectangle.top};
    }
    if (drag_active && ImGui::IsItemActive()) {
        POINT cursor{};
        if (GetCursorPos(&cursor)) SetWindowPos(window, nullptr,
            drag_window.x + cursor.x - drag_cursor.x, drag_window.y + cursor.y - drag_cursor.y,
            0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!ImGui::IsItemActive()) drag_active = false;
    ImGui::SameLine();
    const bool minimize = ImGui::InvisibleButton("##launcher-minimize", {button_width, height});
    auto bounds_min = ImGui::GetItemRectMin(), bounds_max = ImGui::GetItemRectMax();
    auto ink = ImGui::IsItemHovered() ? ThemeColor(unitymem::CabbirdUiTheme().accent) : LauncherInk();
    const ImVec2 minimize_center((bounds_min.x + bounds_max.x) * 0.5f,
        (bounds_min.y + bounds_max.y) * 0.5f + Scale(3.0f));
    draw->AddLine({minimize_center.x - Scale(9), minimize_center.y},
        {minimize_center.x + Scale(9), minimize_center.y}, ImGui::GetColorU32(ink), Scale(2));
    if (minimize && window != nullptr) ShowWindow(window, SW_MINIMIZE);
    ImGui::SameLine();
    const bool close = ImGui::InvisibleButton("##launcher-close", {button_width, height});
    bounds_min = ImGui::GetItemRectMin(); bounds_max = ImGui::GetItemRectMax();
    ink = ImGui::IsItemHovered() ? ThemeColor(unitymem::CabbirdUiTheme().danger) : LauncherInk();
    const ImVec2 center((bounds_min.x + bounds_max.x) * 0.5f,
        (bounds_min.y + bounds_max.y) * 0.5f + Scale(3.0f));
    const float radius = Scale(9.0f);
    draw->AddLine({center.x - radius, center.y - radius}, {center.x + radius, center.y + radius},
        ImGui::GetColorU32(ink), Scale(2));
    draw->AddLine({center.x + radius, center.y - radius}, {center.x - radius, center.y + radius},
        ImGui::GetColorU32(ink), Scale(2));
    if (close && window != nullptr) PostMessageW(window, WM_CLOSE, 0, 0);
    ImGui::PopStyleVar();
}

void DrawLauncherModes(LauncherMode& mode,
    const cabbird::Translator& translator) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(16.0f, 9.0f));
    ImGui::BeginChild("LauncherModes", {0.0f, Scale(kModeHeight)},
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    const std::array labels{Text(translator, cabbird::MessageId::LauncherModeLaunch),
        Text(translator, cabbird::MessageId::LauncherModeSettings)};
    cabbird::glass::SegmentedOptions options;
    options.height = 54.0f; options.padding_x = 32.0f; options.scale = g_launcher_dpi_scale;
    int selected = static_cast<int>(mode);
    if (cabbird::glass::SegmentedControl("launcher-modes", labels, selected, options)) {
        mode = static_cast<LauncherMode>(selected);
        g_launcher_hotkey_capture = false;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void DrawReadOnlyPath(const char* id, const std::filesystem::path& path, float trailing_width,
    const cabbird::Translator& translator) {
    std::string value = path.empty() ? std::string(translator.Text(cabbird::MessageId::LauncherNoDirectorySelected))
        : PathUtf8(path);
    ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - Scale(trailing_width)));
    const float y_padding = std::max(0.0f, (Scale(38.0f) - ImGui::GetFontSize()) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {Scale(16.0f), y_padding});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Scale(14.0f));
    ImGui::InputText(id, value.data(), value.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleVar(2);
}

void DrawLauncherPaths(HWND window, const LauncherActions& actions,
    const LauncherSnapshot& snapshot, const cabbird::Translator& translator) {
    ImGui::TextColored(LauncherMutedInk(), "%s", Text(translator, cabbird::MessageId::LauncherSectionUnityLauncherDirectory));
    DrawReadOnlyPath("##UnityLauncherDirectory", snapshot.launcher_executable.parent_path(), 54.0f, translator);
    ImGui::SameLine(0.0f, Scale(12.0f));
    if (IconButton("choose-unity-launcher", 0xe838,
            Text(translator, cabbird::MessageId::LauncherChooseUnityLauncher), !snapshot.busy)) {
        if (const auto selected = ChooseLauncherExecutable(window, snapshot.launcher_executable,
                snapshot.selected_client, translator)) actions.select_launcher_executable(*selected);
    }
    ImGui::Dummy(Scale(0.0f, 2.0f));
    ImGui::TextColored(LauncherMutedInk(), "%s", Text(translator, cabbird::MessageId::LauncherSectionGameDirectory));
    DrawReadOnlyPath("##GameDirectory", snapshot.game_directory, 54.0f, translator);
    ImGui::SameLine(0.0f, Scale(12.0f));
    if (IconButton("choose-game-directory", 0xe838,
            Text(translator, cabbird::MessageId::LauncherChooseGameDirectory), !snapshot.busy)) {
        if (const auto selected = ChooseGameDirectory(window, snapshot.game_directory, translator))
            actions.select_game_directory(*selected);
    }
}

void DrawRecoveryState(const LauncherActions& actions, const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator) {
    const bool active = snapshot.recovery && snapshot.recovery->safe_mode.Active();
    if (!active && snapshot.recovery_message.empty()) return;
    ImGui::Dummy(Scale(0.0f, 12.0f));
    ImGui::Separator();
    ImGui::Dummy(Scale(0.0f, 8.0f));
    ImGui::TextColored(LauncherMutedInk(), "%s", Text(translator, cabbird::MessageId::LauncherSectionRecovery));
    if (!snapshot.recovery_message.empty()) {
        ImGui::TextWrapped("%s", snapshot.recovery_message.c_str());
        return;
    }
    const auto& safe = snapshot.recovery->safe_mode;
    ImGui::TextColored(ThemeColor(unitymem::CabbirdUiTheme().warning), "%s",
        Text(translator, cabbird::MessageId::LauncherRecoverySafeModeActive));
    if (!safe.reason.empty()) ImGui::TextWrapped("%s", safe.reason.c_str());
    const auto row = [&](bool visible, const char* id, cabbird::MessageId label, cabbird::RuntimeRecoveryAxis axis) {
        if (!visible) return;
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(Text(translator, label));
        ImGui::SameLine(ImGui::GetContentRegionMax().x - Scale(140.0f));
        const auto caption = StableLabel(translator, cabbird::MessageId::LauncherRecoveryRestore, id);
        if (LauncherChip(caption.c_str(), Scale(140.0f, 38.0f), !snapshot.busy)) actions.restore_recovery(axis);
    };
    row(safe.minimal_core, "restore-minimal-core", cabbird::MessageId::LauncherRecoveryMinimalCore, cabbird::RuntimeRecoveryAxis::MinimalCore);
    row(safe.third_party_plugins_suspended, "restore-third-party", cabbird::MessageId::LauncherRecoveryThirdPartyPlugins, cabbird::RuntimeRecoveryAxis::ThirdPartyPlugins);
    row(safe.profile_overrides_suspended, "restore-profiles", cabbird::MessageId::LauncherRecoveryProfileOverrides, cabbird::RuntimeRecoveryAxis::ProfileOverrides);
}

void DrawStartupSettings(const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator) {
    ImGui::TextColored(LauncherMutedInk(), "%s", Text(translator, cabbird::MessageId::LauncherSectionSettings));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, cabbird::MessageId::LauncherSettingMenuToggle));
    ImGui::SameLine(ImGui::GetContentRegionMax().x - Scale(190.0f));
    const auto label = (g_launcher_hotkey_capture ? std::string(Text(translator,
        cabbird::MessageId::LauncherSettingPressKey)) : VirtualKeyName(snapshot.toggle_key)) + "###menu-toggle-binding";
    if (LauncherChip(label.c_str(), Scale(180.0f, 38.0f), !snapshot.busy)) BeginLauncherHotkeyCapture();
    if (g_launcher_hotkey_capture) ImGui::TextColored(LauncherMutedInk(), "%s",
        Text(translator, cabbird::MessageId::LauncherSettingEscapeHint));
}

bool LauncherMessageVisible(const LauncherSnapshot& snapshot) noexcept {
    return snapshot.busy || snapshot.message_kind != MessageKind::Neutral;
}

void DrawProcessOverview(const LauncherActions& actions, const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator) {
    ImGui::Dummy(Scale(0.0f, 14.0f)); ImGui::Separator(); ImGui::Dummy(Scale(0.0f, 12.0f));
    const float status_anchor = ImGui::GetCursorPosY();
    ImGui::TextColored(LauncherMutedInk(), "%s", Text(translator, cabbird::MessageId::LauncherSectionProcesses));
    const DWORD detected_process = snapshot.processes.empty() ? 0 : snapshot.processes.front().process_id;
    const std::string process_id = std::to_string(snapshot.attached_process != 0
        ? snapshot.attached_process : detected_process);
    const std::string status = LauncherMessageVisible(snapshot) ? RenderMessage(translator, snapshot.message)
        : !snapshot.core_available ? std::string(Text(translator, cabbird::MessageId::LauncherStatusCoreUnavailable))
        : snapshot.processes.empty() ? std::string(Text(translator, cabbird::MessageId::LauncherStatusNoProcesses))
        : Format(translator, snapshot.attached_process != 0
            ? cabbird::MessageId::LauncherStatusLaunchAttached : cabbird::MessageId::LauncherStatusProcessFound, {process_id});
    const auto ink = snapshot.message_kind == MessageKind::Error ? ThemeColor(unitymem::CabbirdUiTheme().danger)
        : snapshot.message_kind == MessageKind::Success ? ThemeColor(unitymem::CabbirdUiTheme().success) : LauncherMutedInk();
    const float status_y = ImGui::GetCursorPosY();
    ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - Scale(60.0f));
    ImGui::TextColored(ink, "%s", status.c_str());
    ImGui::PopTextWrapPos();
    const float status_bottom = ImGui::GetCursorPosY();
    ImGui::SetCursorPos({ImGui::GetContentRegionMax().x - ButtonHeight(38.0f), status_y - Scale(5.0f)});
    if (IconButton("refresh-processes", 0xe72c, Text(translator, cabbird::MessageId::LauncherProcessRefresh), !snapshot.busy))
        actions.refresh_processes();
    ImGui::SetCursorPosY(std::max(status_bottom, ImGui::GetCursorPosY()));
    static std::string previous_status;
    static ImVec2 previous_extent{};
    const auto extent = ImGui::GetWindowSize();
    const bool status_changed = previous_status != status;
    const bool extent_changed = extent.x != previous_extent.x || extent.y != previous_extent.y;
    if (status_changed || extent_changed || ImGui::IsWindowAppearing()) {
        const auto* pane = ImGui::GetCurrentWindow();
        const float top = pane->Pos.y + status_anchor - pane->Scroll.y;
        const float bottom = ImGui::GetCursorScreenPos().y;
        if (top < pane->InnerClipRect.Min.y || bottom > pane->InnerClipRect.Max.y)
            ImGui::SetScrollFromPosY(status_anchor - pane->Scroll.y, 0.0f);
    }
    if (status_changed) previous_status = status;
    previous_extent = extent;
}

void DrawLaunchMode(HWND window, const LauncherActions& actions, const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator) {
    // The action row belongs to the pane, while paths, process status and long
    // diagnostics scroll independently above it on smaller windows.
    const float action_y = ImGui::GetContentRegionMax().y - Scale(kLaunchActionRowBottom);
    const float content_height = std::max(Scale(60.0f), action_y - ImGui::GetCursorPosY() - Scale(14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild("LauncherLaunchContent", {0.0f, content_height},
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    DrawLauncherPaths(window, actions, snapshot, translator);
    DrawProcessOverview(actions, snapshot, translator);
    ImGui::EndChild();
    const bool can_launch = !snapshot.busy && snapshot.core_available && !snapshot.launcher_executable.empty() && snapshot.processes.empty();
    const char* launch_label = Text(translator, cabbird::MessageId::LauncherLaunchAttach);
    const float launch_width = std::max(240.0f, CommandButtonWidth(0xe768, launch_label));
    ImGui::SetCursorPosY(action_y);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - Scale(launch_width)));
    if (CommandButton("launch-attach-core", 0xe768, launch_label, true, can_launch,
            {launch_width, kLaunchActionRowHeight})) actions.launch_and_attach();
}

void DrawAppearanceSettings(const LauncherActions& actions, const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator) {
    ImGui::TextColored(LauncherMutedInk(), "%s", Text(translator, cabbird::MessageId::LauncherAppearance));
    ImGui::TextUnformatted(Text(translator, cabbird::MessageId::SettingsPalette));
    const std::array labels{
        Text(translator, cabbird::MessageId::SettingsPaletteMoss),
        Text(translator, cabbird::MessageId::SettingsPaletteAurora),
        Text(translator, cabbird::MessageId::SettingsPaletteEmber),
        Text(translator, cabbird::MessageId::SettingsPalettePaper),
        "CabbirdHub",
        Text(translator, cabbird::MessageId::SettingsPaletteCustom)};
    constexpr std::array<std::string_view, 6> themes{"moss", "aurora", "ember", "paper", "cabbirdhub", "custom"};
    const auto found = std::ranges::find(themes, snapshot.theme);
    int selected = found == themes.end() ? 3 : static_cast<int>(found - themes.begin());
    cabbird::glass::SegmentedOptions options;
    options.height = 46.0f; options.padding_x = 20.0f; options.scale = g_launcher_dpi_scale;
    if (snapshot.busy) options.disabled_mask = 0x3f;
    if (cabbird::glass::SegmentedControl("launcher-theme", labels, selected, options))
        actions.set_theme(std::string(themes[static_cast<std::size_t>(selected)]));
    if (snapshot.theme == "custom") {
        auto colors = snapshot.custom_colors;
        bool changed{};
        const auto color_row = [&](const char* id, const cabbird::MessageId label,
                                   unitymem::CabbirdUiColor& color) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Text(translator, label));
            ImGui::SameLine(ImGui::GetContentRegionMax().x - Scale(230.0f));
            ImGui::SetNextItemWidth(Scale(220.0f));
            std::array channels{color.red, color.green, color.blue};
            if (ImGui::ColorEdit3(id, channels.data())) {
                color = {channels[0], channels[1], channels[2], 1.0f};
                changed = true;
            }
        };
        ImGui::BeginDisabled(snapshot.busy);
        color_row("##custom-accent", cabbird::MessageId::SettingsCustomAccent, colors.accent);
        color_row("##custom-text", cabbird::MessageId::SettingsCustomText, colors.text);
        color_row("##custom-surface", cabbird::MessageId::SettingsCustomWindowBackground, colors.window_background);
        color_row("##custom-child", cabbird::MessageId::SettingsCustomChildBackground, colors.child_background);
        color_row("##custom-border", cabbird::MessageId::SettingsCustomBorder, colors.border);
        ImGui::EndDisabled();
        if (changed) actions.set_custom_colors(colors);
    }
}

void DrawSettingsSection(const cabbird::Translator& translator, const cabbird::MessageId title) {
    ImGui::Dummy(Scale(0.0f, 12.0f)); ImGui::Separator(); ImGui::Dummy(Scale(0.0f, 8.0f));
    ImGui::TextColored(LauncherMutedInk(), "%s", Text(translator, title));
}

void DrawLanguageSettings(const LauncherActions& actions, const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator) {
    DrawSettingsSection(translator, cabbird::MessageId::LauncherSectionLanguage);
    ImGui::TextUnformatted(Text(translator, cabbird::MessageId::SettingsLanguage));
    constexpr std::array languages{cabbird::LanguagePreference::Auto,
        cabbird::LanguagePreference::ZhCn, cabbird::LanguagePreference::EnUs};
    const std::array labels{Text(translator, cabbird::MessageId::SettingsAutomatic),
        Text(translator, cabbird::MessageId::SettingsSimplifiedChinese),
        Text(translator, cabbird::MessageId::SettingsEnglish)};
    const auto preference = cabbird::ParseLanguagePreference(snapshot.language).preference;
    int selected = static_cast<int>(std::ranges::find(languages, preference) - languages.begin());
    cabbird::glass::SegmentedOptions options;
    options.height = 46.0f; options.padding_x = 26.0f; options.scale = g_launcher_dpi_scale;
    if (snapshot.busy) options.disabled_mask = 0x7;
    if (cabbird::glass::SegmentedControl("launcher-language", labels, selected, options))
        actions.set_language(languages[static_cast<std::size_t>(selected)]);
}

void DrawPerformanceSettings(const LauncherActions& actions, const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator) {
    DrawSettingsSection(translator, cabbird::MessageId::LauncherSectionPerformance);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, cabbird::MessageId::LauncherSettingFrameRateLimit));
    ImGui::SameLine(ImGui::GetContentRegionMax().x - Scale(210.0f));
    ImGui::SetNextItemWidth(Scale(200.0f));
    const auto preview = std::to_string(snapshot.frame_rate_limit) + " FPS";
    ImGui::BeginDisabled(snapshot.busy);
    if (cabbird::glass::BeginCombo("##launcher-frame-rate", preview.c_str())) {
        for (const int rate : cabbird::launcher::kLauncherFrameRates) {
            const auto label = std::to_string(rate) + " FPS";
            const bool selected = rate == snapshot.frame_rate_limit;
            if (ImGui::Selectable(label.c_str(), selected)) actions.set_frame_rate_limit(rate);
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
}

std::optional<std::filesystem::path> ChooseLauncherThemeImage(HWND window,
    const cabbird::Translator& translator) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return std::nullopt;
    DWORD options{};
    if (SUCCEEDED(dialog->GetOptions(&options)))
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    const COMDLG_FILTERSPEC filters[]{{L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.webp"}};
    dialog->SetFileTypes(1, filters);
    const auto title = Utf8Wide(translator.Text(cabbird::MessageId::SettingsBackgroundOpen));
    dialog->SetTitle(title.c_str());
    if (FAILED(dialog->Show(window))) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return std::nullopt;
    PWSTR raw{};
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) return std::nullopt;
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

void DrawBackgroundSettings(HWND window, const LauncherActions& actions, const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator) {
    const auto layout = snapshot.theme_layout;
    const int mode = layout != nullptr ? static_cast<int>(layout->layout.background.mode) :
        std::max(0, snapshot.artwork.background_mode);
    const std::string& path = !snapshot.artwork.background.empty() ? snapshot.artwork.background :
        layout != nullptr ? layout->layout.background.path : snapshot.artwork.background;
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, cabbird::MessageId::SettingsBackgroundImage));
    const auto label_right = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    const auto caption = StableLabel(translator, cabbird::MessageId::SettingsBackgroundOpen, "launcher-background-open");
    const float picker_width = ImGui::CalcTextSize(Text(translator, cabbird::MessageId::SettingsBackgroundOpen)).x + Scale(32.0f);
    const float mode_width = Scale(160.0f);
    const float controls_width = cabbird::glass::SwitchSize().x + picker_width + mode_width +
        ImGui::GetStyle().ItemSpacing.x * 2;
    const float left = ImGui::GetContentRegionMax().x - controls_width - Scale(10.0f);
    if (left > label_right + Scale(20.0f)) ImGui::SameLine(left);
    else ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), left));
    ImGui::BeginDisabled(snapshot.busy);
    bool enabled = mode != 0;
    if (cabbird::glass::Switch("##launcher-background-enabled", &enabled))
        actions.set_background(path, enabled ? std::max(mode, 2) : 0);
    ImGui::SameLine();
    if (LauncherChip(caption.c_str(), {picker_width, ButtonHeight(38.0f)})) {
        if (const auto selected = ChooseLauncherThemeImage(window, translator)) actions.import_background(*selected);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(mode_width);
    const std::array modes{Text(translator, cabbird::MessageId::SettingsBackgroundModeCenter),
        Text(translator, cabbird::MessageId::SettingsBackgroundModeCrop),
        Text(translator, cabbird::MessageId::SettingsBackgroundModeStretch)};
    int selected = std::clamp(mode, 1, 3) - 1;
    if (cabbird::glass::BeginCombo("##launcher-background-mode", modes[static_cast<std::size_t>(selected)])) {
        for (int index = 0; index < static_cast<int>(modes.size()); ++index) {
            if (ImGui::Selectable(modes[static_cast<std::size_t>(index)], selected == index))
                actions.set_background(path, index + 1);
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    // Keep the thumb at the value being released until Worker publishes the new theme.
    static std::optional<int> held_visibility;
    static bool committed{true};
    static std::uint64_t committed_revision{};
    const auto revision = layout != nullptr ? layout->revision : 0;
    if (held_visibility && committed && (committed_revision != revision ||
            (!snapshot.busy && snapshot.message_kind == MessageKind::Error))) held_visibility.reset();
    const float applied_opacity = layout != nullptr ? layout->layout.surface_opacity :
        std::clamp(snapshot.artwork.surface_opacity, 0.0f, 1.0f);
    int visibility = held_visibility.value_or(static_cast<int>(std::lround(
        (1.0f - applied_opacity) * 100.0f)));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, cabbird::MessageId::SettingsBackgroundVisibility));
    const auto visibility_right = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    if (left > visibility_right + Scale(20.0f)) ImGui::SameLine(left);
    else ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), left));
    ImGui::BeginChild("BackgroundVisibilityControl", {controls_width, ImGui::GetFrameHeight()},
        ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImGui::SetNextItemWidth(controls_width);
    ImGui::BeginDisabled(snapshot.busy);
    if (cabbird::glass::SliderInt("##launcher-background-visible", &visibility, 0, 100, "%d%%")) {
        held_visibility = visibility;
        committed = false;
    }
    if (!committed && held_visibility && !ImGui::IsItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        actions.set_surface_opacity(1.0f - static_cast<float>(*held_visibility) / 100.0f);
        committed_revision = revision;
        committed = true;
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
}

bool g_update_prompt_requested{};

std::string Num(double value, int precision) {
    char text[64]{};
    std::snprintf(text, sizeof(text), "%.*f", precision, value);
    return text;
}

void LauncherOpenReleasePage(const LauncherUpdateStatus& update) {
    const std::wstring url = Utf8Wide(update.page_url.empty()
        ? std::string(cabbird::launcher::kReleasePageUrl) : update.page_url);
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// The failure a check or an install reported, worded by the launcher where it can be. A rate-limited
// API, an unreachable host and an answer that is not a release are different problems with
// different answers, and the installer's own diagnostics are passed through as they were produced.
std::string UpdateFailureText(
    const cabbird::Translator& translator, const LauncherUpdateStatus& update) {
    const std::string status = std::to_string(update.http_status);
    switch (update.failure) {
    case LauncherUpdateFailure::Network:
        return Text(translator, cabbird::MessageId::LauncherUpdateNetworkFailed);
    case LauncherUpdateFailure::RateLimited:
        return Format(translator, cabbird::MessageId::LauncherUpdateRateLimited, {status});
    case LauncherUpdateFailure::Http:
        return Format(translator, cabbird::MessageId::LauncherUpdateHttpFailed, {status});
    case LauncherUpdateFailure::NotRelease:
        return Format(translator, cabbird::MessageId::LauncherUpdateNotRelease, {status});
    case LauncherUpdateFailure::Check:
        return Format(translator, cabbird::MessageId::LauncherUpdateCheckFailed, {update.detail});
    case LauncherUpdateFailure::Install:
        return Format(translator, cabbird::MessageId::LauncherUpdateFailed, {update.detail});
    case LauncherUpdateFailure::None:
        break;
    }
    return Format(translator, cabbird::MessageId::LauncherUpdateCheckFailed, {update.detail});
}

// The update's own byte counters and cancellation control, independent of a mod download.
// A transfer without a reported length shows received bytes instead of a percentage.
void LauncherDrawDownloadProgress(const cabbird::Translator& translator) {
    const double done = static_cast<double>(g_update_download.bytes.load());
    const double total = static_cast<double>(g_update_download.total.load());
    // Seconds since this download started: the bar only appears while one runs, so a static clock
    // reset when the transfer begins is all the elapsed time it needs.
    static double started{};
    static long long last_seen{};
    static bool running{};
    if (!running || done < last_seen) {
        started = ImGui::GetTime();
        running = true;
    }
    last_seen = static_cast<long long>(done);
    const double seconds = ImGui::GetTime() - started;
    const double megabytes = done / (1024.0 * 1024.0);
    if (total > 0.0) {
        const std::string overlay = Format(translator,
            cabbird::MessageId::LauncherBrowseProgressPercent,
            {Num(megabytes, 1), Num(total / (1024.0 * 1024.0), 1),
                Num(done * 100.0 / total, 0), Num(seconds, 0)});
        ImGui::ProgressBar(static_cast<float>(done / total), ImVec2(Scale(260.0f), 0.0f),
            overlay.c_str());
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", Format(translator,
            cabbird::MessageId::LauncherBrowseProgressBytes,
            {Num(megabytes, 1), Num(seconds, 0)}).c_str());
    }
    ImGui::SameLine();
    // The one command that makes sense beside a running transfer.
    if (LauncherChip((std::string(Text(translator, cabbird::MessageId::LauncherModsCancel)) +
            "##cancel-update-download").c_str(), {})) {
        g_update_download.cancel.store(true);
    }
}

// The state of the running update, as both the page and the dialog report it.
std::string UpdatePhaseText(
    const cabbird::Translator& translator, const LauncherUpdateStatus& update) {
    switch (update.phase) {
    case LauncherUpdatePhase::Idle:
        return {};
    case LauncherUpdatePhase::Checking:
        return Text(translator, cabbird::MessageId::LauncherUpdateChecking);
    case LauncherUpdatePhase::UpToDate:
        return Format(translator, cabbird::MessageId::LauncherUpdateUpToDate,
            {update.remote_version});
    case LauncherUpdatePhase::Available:
        return Format(translator, cabbird::MessageId::LauncherUpdateAvailable,
            {update.remote_version});
    case LauncherUpdatePhase::Downloading:
        return Text(translator, cabbird::MessageId::LauncherUpdateDownloading);
    case LauncherUpdatePhase::Installing:
        return Text(translator, cabbird::MessageId::LauncherUpdateInstalling);
    case LauncherUpdatePhase::Installed:
        return Text(translator, cabbird::MessageId::LauncherUpdateInstalled);
    case LauncherUpdatePhase::Failed:
        return UpdateFailureText(translator, update);
    }
    return {};
}

ImVec4 UpdatePhaseInk(const LauncherUpdatePhase phase) {
    const auto& theme = unitymem::CabbirdUiTheme();
    switch (phase) {
    case LauncherUpdatePhase::Available: return ThemeColor(theme.warning);
    case LauncherUpdatePhase::Installed: return ThemeColor(theme.success);
    case LauncherUpdatePhase::Failed: return ThemeColor(theme.danger);
    default: return LauncherMutedInk();
    }
}

// The launcher's own updates: the version this build is, the newest release it found, and the two
// commands that follow from them. The update replaces the files of this folder and restarts the
// window, so the button that starts it names the version it installs.
void DrawUpdateSettings(
    const LauncherActions& actions, const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator) {
    const LauncherUpdateStatus& update = snapshot.update;
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Text(translator, cabbird::MessageId::LauncherSectionUpdates));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Text(translator, cabbird::MessageId::LauncherSettingAutoUpdate));
    ImGui::SameLine(ImGui::GetContentRegionMax().x - Scale(70.0f) - Scale(10.0f));
    bool auto_check = update.auto_check;
    if (cabbird::glass::Switch("##launcher-auto-update", &auto_check)) {
        actions.set_auto_check_updates(auto_check);
    }
    ImGui::TextDisabled("%s", Format(translator, cabbird::MessageId::LauncherUpdateCurrent,
        {CABBIRD_SDK_VERSION_STRING}).c_str());
    const bool busy = snapshot.busy || update.Busy();
    if (LauncherChip(Text(translator, update.phase == LauncherUpdatePhase::Checking
            ? cabbird::MessageId::LauncherUpdateChecking
            : cabbird::MessageId::LauncherUpdateCheck), {}, !busy)) {
        actions.check_for_updates();
    }
    // The update command exists only while there is a release to install, so the row never offers
    // something the launcher has no version for.
    if (update.CanInstall()) {
        ImGui::SameLine();
        if (LauncherChip(Format(translator, cabbird::MessageId::LauncherUpdateInstall,
                {update.remote_version}).c_str(), {}, !busy)) {
            actions.install_update();
        }
    }
    ImGui::SameLine();
    // The release page is the only place that says what a new version changes, and it is where a
    // user goes when they would rather read about it before installing.
    if (LauncherChip(Text(translator, cabbird::MessageId::LauncherUpdateOpenPage), {})) {
        LauncherOpenReleasePage(update);
    }
    const std::string status = UpdatePhaseText(translator, update);
    if (update.phase == LauncherUpdatePhase::Downloading) {
        LauncherDrawDownloadProgress(translator);
    } else if (!status.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(UpdatePhaseInk(update.phase), "%s", status.c_str());
        ImGui::PopTextWrapPos();
    }
}

// The release notes are markdown, and the shell already owns the parser for the markdown it draws
// (the first-run notice is authored in it). The launcher reuses that parser and draws the blocks
// itself, in its own ink: the same document, rendered the way the rest of this window is.
//
// Emphasis is faux bold -- the run drawn twice -- because the launcher bakes one font weight, and a
// run that wrapped is not doubled: the second draw would land on the line above it.
void LauncherDrawEmphasis(const std::string& text) {
    ImGui::TextUnformatted(text.c_str());
    if (ImGui::GetItemRectSize().y > ImGui::GetTextLineHeight() * 1.6f) return;
    const ImVec2 origin = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
        ImVec2(origin.x + (std::max)(0.5f, Scale(0.5f)), origin.y),
        ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
}

// One line of a block: the runs stay in a single sentence, so emphasis and links keep their place.
void LauncherDrawNoticeRuns(const std::vector<cabbird::NoticeRun>& runs) {
    const auto& theme = unitymem::CabbirdUiTheme();
    ImGui::PushTextWrapPos();
    for (std::size_t index = 0; index < runs.size(); ++index) {
        if (index > 0) ImGui::SameLine(0.0f, 0.0f);
        const cabbird::NoticeRun& run = runs[index];
        switch (run.style) {
        case cabbird::NoticeRunStyle::Text:
            ImGui::TextUnformatted(run.text.c_str());
            break;
        case cabbird::NoticeRunStyle::Emphasis:
            LauncherDrawEmphasis(run.text);
            break;
        case cabbird::NoticeRunStyle::Code:
            ImGui::TextColored(LauncherMutedInk(), "%s", run.text.c_str());
            break;
        case cabbird::NoticeRunStyle::Link:
            ImGui::TextColored(ThemeColor(theme.accent), "%s", run.text.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s", run.url.c_str());
            }
            // A link in a release note is the one thing here that leaves the machine: it goes to the
            // shell like every other address this window opens.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !run.url.empty()) {
                const std::wstring url = Utf8Wide(run.url);
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        }
    }
    ImGui::PopTextWrapPos();
}

// A heading: the same text a little larger and painted twice a hair apart, which is the whole of the
// emphasis this window has -- it bakes one font weight, so "bold" is the glyphs drawn again and
// "larger" is the atlas drawn at a bigger size. A heading that would not fit the pane keeps the body
// size and wraps like any other line, because a clipped heading is worse than a plain one.
void LauncherDrawHeading(const std::vector<cabbird::NoticeRun>& runs, const bool muted) {
    std::string text;
    for (const cabbird::NoticeRun& run : runs) text += run.text;
    if (text.empty()) return;
    ImFont* const font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * 1.18f;
    const ImVec2 measured = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    const ImVec4 ink = muted ? LauncherMutedInk() : LauncherInk();
    if (measured.x > ImGui::GetContentRegionAvail().x) {
        ImGui::TextWrapped("%s", text.c_str());
        return;
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* const draw = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ink);
    const float bold = (std::max)(0.6f, Scale(0.6f));
    draw->AddText(font, size, origin, color, text.c_str());
    draw->AddText(font, size, ImVec2(origin.x + bold, origin.y), color, text.c_str());
    ImGui::Dummy(measured);
}

void LauncherDrawReleaseNotes(const cabbird::NoticeDocument& document) {
    for (std::size_t index = 0; index < document.blocks.size(); ++index) {
        const cabbird::NoticeBlock& block = document.blocks[index];
        switch (block.kind) {
        case cabbird::NoticeBlockKind::Heading:
        case cabbird::NoticeBlockKind::Subheading: {
            // The release opens with its own name and date, which is context rather than a section;
            // every heading is the same emphasis, only the first is quieter.
            LauncherDrawHeading(block.runs, index == 0);
            ImGui::Spacing();
            break;
        }
        case cabbird::NoticeBlockKind::Paragraph:
            LauncherDrawNoticeRuns(block.runs);
            ImGui::Spacing();
            break;
        case cabbird::NoticeBlockKind::Bullet:
            ImGui::Bullet();
            ImGui::SameLine();
            LauncherDrawNoticeRuns(block.runs);
            ImGui::Spacing();
            break;
        case cabbird::NoticeBlockKind::Numbered:
            ImGui::TextUnformatted(block.marker.c_str());
            ImGui::SameLine();
            LauncherDrawNoticeRuns(block.runs);
            ImGui::Spacing();
            break;
        case cabbird::NoticeBlockKind::Separator:
            ImGui::Separator();
            break;
        }
    }
}

// The parsed notes, kept across frames: the document only changes when the release does, and parsing
// a few kilobytes of markdown every frame for a dialog that is usually closed is wasted work.
// git-cliff marks a change's scope with single asterisks -- "*(autotaxi)* Add ..." -- which is not
// emphasis in the shell's markdown subset, so the markers would reach the screen. Keeping "**" (real
// bold) and the asterisk a bullet starts with, and dropping every other one, leaves the scope text
// readable and the bold intact.
std::string LauncherNotesMarkdown(const std::string& notes) {
    std::string cleaned;
    cleaned.reserve(notes.size());
    for (std::size_t index = 0; index < notes.size(); ++index) {
        const char character = notes[index];
        if (character != '*') {
            cleaned.push_back(character);
            continue;
        }
        const bool line_start = index == 0 || notes[index - 1] == '\n' ||
            (notes[index - 1] == ' ' && index > 1 && notes[index - 2] == '\n');
        if (line_start) {
            cleaned.push_back(character);
            continue;
        }
        if (index + 1 < notes.size() && notes[index + 1] == '*') {
            cleaned.append("**");
            ++index;
        }
    }
    return cleaned;
}

const cabbird::NoticeDocument* LauncherReleaseNotes(const std::string& version,
    const std::string& notes) {
    static std::string parsed_for;
    static cabbird::NoticeDocument document;
    const std::string key = version + ':' + std::to_string(notes.size());
    if (key != parsed_for) {
        document = cabbird::ParseNoticeMarkdown(LauncherNotesMarkdown(notes), 0);
        parsed_for = key;
    }
    return &document;
}

// The prompt an update check ends in: the release's own notes in a pane, and two answers below --
// skipping that version on the left, updating on the right. Nothing is installed until the right one
// is pressed, which is the difference between an update and a silent one.
void DrawUpdatePrompt(
    const LauncherActions& actions, const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator) {
    const LauncherUpdateStatus& update = snapshot.update;
    // The dialog's name is its id, and nothing else: its title is drawn below, four pixels lower than
    // the title bar would have put it, so the line under it keeps the place it has always had. The two
    // strings still have to be the same one -- OpenPopup and BeginPopupModal pair on this id.
    const std::string title = Format(translator, cabbird::MessageId::LauncherUpdateAvailable,
        {update.remote_version});
    const std::string name = "##update-available";
    // One question per release, and none for a release the user already skipped: a version answered
    // once is not asked about again, this session or a later one. The band's chip asks for the same
    // dialog explicitly, which is why that path opens it even when this session already saw the
    // automatic one.
    static std::string prompted_version;
    const bool asked = std::exchange(g_update_prompt_requested, false);
    if (update.phase == LauncherUpdatePhase::Available && !update.remote_version.empty() &&
        (asked || (update.skipped_version != update.remote_version &&
            prompted_version != update.remote_version))) {
        prompted_version = update.remote_version;
        ImGui::OpenPopup(name.c_str());
    }
    // The popup's padding: its left edge is the inset the title bar uses (FramePadding.x), so the
    // first line of content sits exactly under the title instead of a few pixels left of it, and
    // the bottom of it gives the last row the same room the top gives the first.
    const ImGuiStyle& dialog_style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
        ImVec2(dialog_style.FramePadding.x, Scale(2.0f)));
    // Centred on the launcher's canvas -- not on the OS window, which reaches up into the transparent
    // band above it -- and held there: a question is not a window the user should have to put back.
    const ImVec2 canvas_min = ImGui::GetWindowPos();
    const ImVec2 canvas_size = ImGui::GetWindowSize();
    ImGui::SetNextWindowPos(
        ImVec2(canvas_min.x + canvas_size.x * 0.5f, canvas_min.y + canvas_size.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // Sized by its contents, like the mod settings dialog, so it grows with the language it is drawn
    // in rather than clipping a localized label.
    if (!ImGui::BeginPopupModal(name.c_str(), nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize)) {
        ImGui::PopStyleVar();
        return;
    }
    // The title bar itself draws nothing: its text is put here instead, a few pixels lower than the
    // bar would have placed it, so the line below keeps the position it has always had. The clip is
    // widened for it -- a window clips its items to the content area, and the title bar is above that
    // -- or the text would be drawn and never seen.
    {
        const ImVec2 origin = ImGui::GetWindowPos();
        const ImVec2 corner(origin.x + ImGui::GetWindowSize().x,
            origin.y + ImGui::GetFontSize() + dialog_style.FramePadding.y * 2.0f);
        ImDrawList* const draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(origin, corner, false);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
            ImVec2(origin.x + dialog_style.FramePadding.x,
                origin.y + dialog_style.FramePadding.y + Scale(4.0f)),
            ImGui::GetColorU32(LauncherInk()), title.c_str());
        draw->PopClipRect();
    }
    // The version being left behind, under the title that names the one being installed.
    ImGui::TextDisabled("%s", Format(translator, cabbird::MessageId::LauncherUpdateCurrent,
        {CABBIRD_SDK_VERSION_STRING}).c_str());
    ImGui::Spacing();

    // The notes sit in a pane of their own, on the same material as the page's cards but quieter: it
    // is a recess inside the dialog, not another object floating over it.
    const float pane_width = Scale(760.0f);
    const bool has_notes = !update.notes.empty();
    if (has_notes) {
        const float pane_height = Scale(380.0f);
        const ImVec2 pane_min = ImGui::GetCursorScreenPos();
        const ImVec2 pane_max(pane_min.x + pane_width, pane_min.y + pane_height);
        // A plain recess, not a second pane of glass: the material belongs to the surface a window is
        // cut from, and another layer of it inside the first only muddies both. A tinted fill with a
        // hairline is what a panel inside a dialog reads as, and it keeps the notes legible.
        const auto& theme = unitymem::CabbirdUiTheme();
        ImDrawList* const draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(pane_min, pane_max,
            ImGui::GetColorU32(ThemeColorWithAlpha(theme.child_background, 0.72f)),
            Scale(12.0f));
        draw->AddRect(pane_min, pane_max,
            ImGui::GetColorU32(ThemeColorWithAlpha(theme.border, 0.55f)),
            Scale(12.0f), ImDrawFlags_None, Scale(1.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        // A child without a border ignores style.WindowPadding unless it is asked to use it, which is
        // how the notes ended up written against the pane's edges. AlwaysUseWindowPadding gives the
        // text the inset it needs, and the wrap position follows that inset instead of the window edge.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(18.0f, 14.0f));
        ImGui::BeginChild("##update-notes", ImVec2(pane_width, pane_height),
            ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
        // The release publishes markdown; the shell's own parser turns it into blocks, and the blocks
        // are drawn here in this window's ink rather than dumped as a wall of hashes and asterisks.
        LauncherDrawReleaseNotes(*LauncherReleaseNotes(update.remote_version, update.notes));
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    const bool busy = snapshot.busy || update.Busy();
    const std::string status = UpdatePhaseText(translator, update);
    if (update.phase == LauncherUpdatePhase::Downloading) {
        LauncherDrawDownloadProgress(translator);
    } else if (!status.empty() && update.phase != LauncherUpdatePhase::Available) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(UpdatePhaseInk(update.phase), "%s", status.c_str());
        ImGui::PopTextWrapPos();
    }
    // The two answers are centred as one pair with a gap between them: a dialog asks a question, and
    // its answers read as the pair that answers it rather than as two corners of the window. The update
    // is the launcher's primary command -- accent inside the glass, with the download mark -- so the
    // pair also carries the hierarchy.
    const float row_height = ButtonHeight(44.0f);
    const char* const skip_label = Text(translator, cabbird::MessageId::LauncherUpdateSkip);
    const std::string update_label = Format(translator, cabbird::MessageId::LauncherUpdateInstall,
        {update.remote_version});
    // Both widths are measured the way the two controls size themselves -- the chip from its caption,
    // the command from its mark and label -- so the pair stays centred in every language.
    const float skip_width = ImGui::CalcTextSize(skip_label).x + Scale(30.0f);
    const float update_width = ImGui::CalcTextSize(Glyph(0xe8b7)).x +
        ImGui::CalcTextSize(update_label.c_str()).x + Scale(46.0f);
    float pair_width = skip_width + update_width + Scale(16.0f);
    if (!has_notes) {
        // A release without notes has nothing to read here, so the page that does have something to
        // read joins the pair instead of leaving an empty pane on screen.
        pair_width += ImGui::CalcTextSize(
            Text(translator, cabbird::MessageId::LauncherUpdateOpenPage)).x + Scale(46.0f);
    }
    const float pair_left = ImGui::GetCursorPosX() +
        (std::max)(0.0f, (ImGui::GetContentRegionAvail().x - pair_width) * 0.5f);
    ImGui::SetCursorPosX(pair_left);
    ImGui::BeginDisabled(busy);
    if (LauncherChip(skip_label, ImVec2(skip_width, row_height))) {
        actions.skip_update_version(update.remote_version);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    if (!has_notes) {
        ImGui::SameLine(0.0f, Scale(8.0f));
        if (LauncherChip(Text(translator, cabbird::MessageId::LauncherUpdateOpenPage),
                ImVec2(0.0f, row_height))) {
            LauncherOpenReleasePage(update);
        }
    }
    ImGui::SameLine(0.0f, Scale(16.0f));
    ImGui::BeginDisabled(busy);
    if (CommandButton("update-install", 0xe8b7, update_label.c_str(), true, true,
            ImVec2(update_width / g_launcher_dpi_scale, 44.0f))) {
        actions.install_update();
    }
    ImGui::EndDisabled();
    // The package is downloaded from github.com, which a network behind the wrong wall may not reach
    // at all. These are the same release published where it can be fetched, offered next to the
    // launcher's own download instead of in place of it. No extra spacing above them: the line after
    // the answers already carries the row spacing, and a Spacing here doubled it.
    {
        const float quark_width = ImGui::CalcTextSize(
            Text(translator, cabbird::MessageId::LauncherUpdateQuark)).x + Scale(30.0f);
        const float baidu_width = ImGui::CalcTextSize(
            Text(translator, cabbird::MessageId::LauncherUpdateBaidu)).x + Scale(30.0f);
        const float links_width = quark_width + baidu_width + Scale(8.0f);
        const float links_left = ImGui::GetCursorPosX() +
            (std::max)(0.0f, (ImGui::GetContentRegionAvail().x - links_width) * 0.5f);
        ImGui::SetCursorPosX(links_left);
        if (LauncherChip(Text(translator, cabbird::MessageId::LauncherUpdateQuark),
                ImVec2(quark_width, row_height), !cabbird::launcher::kReleaseQuarkUrl.empty())) {
            const std::wstring url(cabbird::launcher::kReleaseQuarkUrl);
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine(0.0f, Scale(8.0f));
        if (LauncherChip(Text(translator, cabbird::MessageId::LauncherUpdateBaidu),
                ImVec2(baidu_width, row_height), !cabbird::launcher::kReleaseBaiduUrl.empty())) {
            const std::wstring url(cabbird::launcher::kReleaseBaiduUrl);
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    // The padding no longer gives the bottom the room the top does not need, so the last row keeps a
    // reserve of its own instead of sitting on the window's edge.
    ImGui::Dummy(ImVec2(0.0f, Scale(10.0f)));
    ImGui::EndPopup();
    ImGui::PopStyleVar();
}


void DrawAboutSettings(const cabbird::Translator& translator) {
    DrawSettingsSection(translator, cabbird::MessageId::LauncherSectionAbout);
    struct Address final {
        cabbird::MessageId label;
        const char* id;
        std::string_view url;
    };
    constexpr std::array<Address, 2> addresses{{
        {cabbird::MessageId::LauncherAboutGithub, "##about-github", kRepositoryUrl},
        {cabbird::MessageId::LauncherAboutDiscord, "##about-discord", kDiscordUrl},
    }};
    const float row_height = ButtonHeight(40.0f);
    for (std::size_t index = 0; index < addresses.size(); ++index) {
        const Address& address = addresses[index];
        const char* const label = Text(translator, address.label);
        const float width = ImGui::CalcTextSize(label).x + Scale(40.0f);
        if (index > 0) ImGui::SameLine(0.0f, Scale(12.0f));
        const std::string chip = std::string(label) + address.id;
        if (LauncherChip(chip.c_str(), ImVec2(width, row_height))) {
            const std::wstring url = Utf8Wide(address.url);
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%.*s", static_cast<int>(address.url.size()), address.url.data());
        }
    }
}

void DrawSettingsMode(HWND window, const LauncherActions& actions, const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator) {
    DrawAppearanceSettings(actions, snapshot, translator);
    DrawBackgroundSettings(window, actions, snapshot, translator);
    if (!snapshot.settings_error.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ThemeColor(unitymem::CabbirdUiTheme().danger), "%s: %s",
            Text(translator, cabbird::MessageId::SettingsSaveFailed), snapshot.settings_error.c_str());
        ImGui::PopTextWrapPos();
    }
    DrawLanguageSettings(actions, snapshot, translator);
    DrawPerformanceSettings(actions, snapshot, translator);
    ImGui::Dummy(Scale(0.0f, 12.0f)); ImGui::Separator(); ImGui::Dummy(Scale(0.0f, 8.0f));
    DrawStartupSettings(snapshot, translator);
    DrawUpdateSettings(actions, snapshot, translator);
    DrawAboutSettings(translator);
    DrawRecoveryState(actions, snapshot, translator);
}

void DrawLauncher(HWND window, Graphics& graphics, const LauncherSnapshot& snapshot,
    const LauncherActions& actions, LauncherMode& mode, const cabbird::Translator& translator) {
    if (!snapshot.busy) {
        if (const auto captured = CaptureLauncherHotkey()) actions.set_toggle_key(*captured);
    }
    const auto wanted_palette = unitymem::ParseCabbirdUiPalette(snapshot.theme);
    UploadLauncherTheme(graphics, snapshot.theme_layout);
    const float surface_alpha = snapshot.theme_layout != nullptr && snapshot.theme_layout->HasBackground() &&
            graphics.theme_background != nullptr ? snapshot.theme_layout->layout.surface_opacity : 1.0f;
    const bool surface_changed = cabbird::GetCabbirdUiSurfaceAlpha() != surface_alpha;
    if (surface_changed) cabbird::SetCabbirdUiSurfaceAlpha(surface_alpha);
    const bool custom_changed = wanted_palette == unitymem::CabbirdUiPalette::Custom &&
        unitymem::GetCabbirdUiCustomColors() != snapshot.custom_colors;
    if (custom_changed) unitymem::SetCabbirdUiCustomColors(snapshot.custom_colors);
    if (unitymem::GetCabbirdUiPalette() != wanted_palette || custom_changed || surface_changed) {
        unitymem::SetCabbirdUiPalette(wanted_palette);
        ApplyLauncherDpiScale();
        graphics.redraw_required = true;
    }
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0.0f, 0.0f}); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::Begin("CabbirdLauncherRoot", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
    const ImVec2 origin = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y},
        ImGui::GetColorU32(ThemeColor(cabbird::CabbirdUiSurfaceColor())), Scale(20.0f));
    DrawLauncherThemeImages(graphics, snapshot.theme_layout, origin, size, false);
    cabbird::glass::CaptureBackdrop(ImGui::GetWindowDrawList());
    DrawLauncherTitleBar(window, graphics);
    DrawLauncherModes(mode, translator);
    const ImVec2 card_min(origin.x + Scale(14.0f), ImGui::GetCursorScreenPos().y);
    const ImVec2 card_max(origin.x + size.x - Scale(14.0f), origin.y + size.y - Scale(14.0f));
    cabbird::glass::SurfaceOptions card;
    card.rounding = Scale(20.0f); card.scale = g_launcher_dpi_scale; card.shadow = false;
    cabbird::glass::DrawSurface(ImGui::GetWindowDrawList(), card_min, card_max, card);
    ImGui::SetCursorScreenPos(card_min);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Scale(20.0f, 18.0f));
    ImGui::BeginChild("LauncherBody", {size.x - Scale(28.0f), -Scale(14.0f)},
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    if (mode == LauncherMode::Launch) {
        DrawLaunchMode(window, actions, snapshot, translator);
    } else DrawSettingsMode(window, actions, snapshot, translator);
    ImGui::EndChild(); ImGui::PopStyleVar();
    DrawLauncherThemeImages(graphics, snapshot.theme_layout, origin, size, true);
    DrawUpdatePrompt(actions, snapshot, translator);
    ImGui::End(); ImGui::PopStyleVar();
}

int DrawLauncher(HWND window, Graphics& graphics, LauncherController& controller,
    LauncherMode& mode, const cabbird::Translator& translator) {
    controller.PollProcesses();
    controller.AutoCheckUpdates();
    const auto snapshot = controller.Snapshot();
    DrawLauncher(window, graphics, snapshot, ActionsFor(controller), mode,
        snapshot.translator != nullptr ? *snapshot.translator : translator);
    if (snapshot.restart_pending) PostMessageW(window, WM_CLOSE, 0, 0);
    if (!ImGui::IsAnyMouseDown()) controller.FlushSettings();
    return snapshot.frame_rate_limit;
}

bool IsLauncherInteraction(const UINT message) noexcept {
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
        (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
        message == WM_MOUSELEAVE || message == WM_SETFOCUS || message == WM_KILLFOCUS ||
        message == WM_SIZE || message == WM_DPICHANGED || message == WM_PAINT;
}

// Both a frame deadline and new window messages can wake the UI thread. Process input
// immediately even at the idle cadence, then recheck the active limit before drawing.
void WaitForLauncherFrame(HANDLE timer, const std::chrono::steady_clock::duration remaining) {
    using namespace std::chrono;
    if (timer != nullptr) {
        LARGE_INTEGER due{};
        due.QuadPart = -ceil<duration<long long, std::ratio<1, 10000000>>>(remaining).count();
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            MsgWaitForMultipleObjectsEx(1, &timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            return;
        }
    }
    MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(ceil<milliseconds>(remaining).count()),
        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
    static_cast<void>(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    const auto administrator = EnsureAdministrator(command_line);
    if (!administrator.run_current_process) return administrator.exit_code;

    int argument_count{};
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    DWORD parent_process{};
    if (arguments != nullptr) {
        if (argument_count == 3 && std::wstring_view(arguments[1]) == L"--wait-for-update") {
            wchar_t* end{};
            parent_process = wcstoul(arguments[2], &end, 10);
            if (end == arguments[2] || *end != L'\0' || parent_process == GetCurrentProcessId()) {
                LocalFree(arguments);
                return ERROR_INVALID_PARAMETER;
            }
        }
        LocalFree(arguments);
    }
    if (parent_process != 0) {
        const HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_process);
        if (parent != nullptr) {
            const DWORD waited = WaitForSingleObject(parent, 60000);
            CloseHandle(parent);
            if (waited != WAIT_OBJECT_0) return ERROR_TIMEOUT;
        } else if (GetLastError() != ERROR_INVALID_PARAMETER) {
            return static_cast<int>(GetLastError());
        }
    }



    // Pin the system dwmapi before ImGui's delay imports are resolved.  The pin costs one
    // LoadLibrary and keeps this program independent of whatever appears next to it later.
    const HMODULE system_dwmapi = LoadSystemDwmapi();
    if (system_dwmapi == nullptr) return static_cast<int>(GetLastError());

    const std::filesystem::path runtime_root = ExecutableDirectory() / L"Cabbird";
    const unitymem::AnalyzerConfig config =
        unitymem::AnalyzerConfig::Load(runtime_root / L"cabbird.ini");
    const auto launcher_configuration = cabbird::launcher::LoadLauncherConfiguration(
        cabbird::launcher::LauncherConfigurationPath(ExecutableDirectory()));
    unitymem::SetCabbirdUiPalette(unitymem::ParseCabbirdUiPalette(launcher_configuration.configuration.theme));
    const auto language = launcher_configuration.configuration.language.empty() ? config.platform_language :
        cabbird::ParseLanguagePreference(launcher_configuration.configuration.language).preference;
    const auto locale = cabbird::ResolveUserLocale(language);
    const auto translator_result = cabbird::LoadHostCatalog(
        locale.locale, runtime_root / L"locales" / L"host");
    if (translator_result.translator == nullptr) return ERROR_RESOURCE_DATA_NOT_FOUND;
    const std::shared_ptr<const cabbird::Translator> translator =
        translator_result.translator;

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    WNDCLASSEXW window_class{
        .cbSize = sizeof(window_class),
        .style = CS_CLASSDC,
        .lpfnWndProc = WindowProc,
        .hInstance = instance,
        .hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kIconResourceId)),
        .hCursor = LoadCursorW(nullptr, IDC_ARROW),
        .lpszClassName = L"CabbirdLauncherWindow",
        .hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(kIconResourceId)),
    };
    if (RegisterClassExW(&window_class) == 0) return 1;
    const std::wstring window_title = Utf8Wide(
        translator->Text(cabbird::MessageId::LauncherWindowTitle));
    g_launcher_dpi_scale = DpiScale(GetDpiForSystem());
    MONITORINFO monitor{sizeof(monitor)};
    POINT cursor{};
    GetCursorPos(&cursor);
    RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor)) work = monitor.rcWork;
    const int work_width = std::max(1L, work.right - work.left);
    const int work_height = std::max(1L, work.bottom - work.top);
    const int initial_width = std::min(work_width, static_cast<int>(Scale(1280.0f)));
    const int initial_height = std::min(work_height, static_cast<int>(Scale(860.0f)));
    const int initial_x = work.left + (work_width - initial_width) / 2;
    const int initial_y = work.top + (work_height - initial_height) / 2;
    const HWND window = CreateWindowExW(
        cabbird::launcher::kDesktopWindowExtendedStyle, window_class.lpszClassName, window_title.c_str(),
        cabbird::launcher::kDesktopWindowStyle, initial_x, initial_y,
        initial_width, initial_height,
        nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        UnregisterClassW(window_class.lpszClassName, instance);
        return 2;
    }

    Graphics graphics;
    g_graphics = &graphics;
    if (!CreateGraphics(window, graphics)) {
        DestroyWindow(window);
        UnregisterClassW(window_class.lpszClassName, instance);
        g_graphics = nullptr;
        return 3;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    static_cast<void>(unitymem::ConfigureCabbirdUiFontAtlas(runtime_root,
        unitymem::CabbirdUiFontBake{.scales = kLauncherFontScales, .icons = kLauncherIcons,
            .tight_atlas = true, .symbol_blocks = false}));
    g_launcher_dpi_scale = DpiScale(GetDpiForWindow(window));
    ApplyLauncherDpiScale();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigNavCursorVisibleAuto = false;
    io.ConfigNavEscapeClearFocusWindow = true;
    io.IniFilename = nullptr;
    if (!ImGui_ImplWin32_Init(window) ||
        !ImGui_ImplDX11_Init(graphics.device.Get(), graphics.context.Get())) {
        ImGui::DestroyContext();
        graphics = {};
        DestroyWindow(window);
        UnregisterClassW(window_class.lpszClassName, instance);
        g_graphics = nullptr;
        return 4;
    }
    static_cast<void>(cabbird::glass::InitializeRenderer(
        graphics.device.Get(), graphics.context.Get(), false));
    if (ImGui_ImplDX11_CreateDeviceObjects())
        unitymem::ReleaseCabbirdUiFontAtlas(unitymem::CabbirdUiFontRetention::Minimal);
    LoadLogo(graphics);
    auto initial_configuration = launcher_configuration.configuration;
    initial_configuration.language = cabbird::LanguagePreferenceName(language);
    LauncherController controller(ExecutableDirectory(), initial_configuration, translator);
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);

    bool running = true;
    using Clock = std::chrono::steady_clock;
    const HANDLE frame_timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
    constexpr auto interaction_tail = std::chrono::milliseconds(500);
    auto previous_frame = Clock::now();
    auto active_until = previous_frame + interaction_tail;
    LauncherDrawing drawing;
    LauncherMode mode = LauncherMode::Launch;
    int frame_rate_limit = launcher_configuration.configuration.frame_rate_limit;
    while (running) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (IsLauncherInteraction(message.message)) active_until = Clock::now() + interaction_tail;
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (g_launcher_dpi_changed) ApplyLauncherDpiScale();
        if (IsIconic(window)) {
            MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }
        const auto now = Clock::now();
        const auto period = now < active_until
            ? std::chrono::nanoseconds(1'000'000'000 / frame_rate_limit)
            : std::chrono::milliseconds(125);
        const auto due = previous_frame + period;
        if (now < due) {
            WaitForLauncherFrame(frame_timer, due - now);
            continue;
        }
        previous_frame = now - due < period ? due : now;
        if (graphics.render_target == nullptr && !CreateRenderTarget(graphics)) continue;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        frame_rate_limit = DrawLauncher(window, graphics, controller, mode, *translator);
        if (ImGui::IsAnyMouseDown()) active_until = now + interaction_tail;
        ImGui::Render();
        ImDrawData* const data = ImGui::GetDrawData();
        cabbird::glass::PrepareDrawData(data, /*retain_desktop_layer=*/true);
        const int first_change = drawing.Compare(*data, graphics.redraw_required);
        if (first_change >= 0) {
            auto damage = drawing.Render(graphics, *data, first_change);
            DXGI_PRESENT_PARAMETERS update{};
            if (damage) {
                update.DirtyRectsCount = 1;
                update.pDirtyRects = &*damage;
            }
            const HRESULT presented = graphics.swap_chain->Present1(0, 0, &update);
            graphics.redraw_required = presented != S_OK;
        }
    }
    if (frame_timer != nullptr) CloseHandle(frame_timer);

    cabbird::glass::ShutdownRenderer();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_graphics = nullptr;
    graphics = {};
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, instance);
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}

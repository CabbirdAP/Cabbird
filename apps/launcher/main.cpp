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
constexpr std::array<char32_t, 8> kLauncherIcons{
    0xe838, 0xe72c, 0xe768, 0xe73e, 0xe7ba, 0xe711, 0xe8b7, 0xe8e5};
constexpr float kDefaultDpi = 96.0f;

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
            const auto selected = ResolveAttachRuntime();
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

    void AttachToProcess(const DWORD process_id) {
        Queue(cabbird::MessageId::LauncherStatusAttachingProcess, [this, process_id] {
            const auto runtime = ResolveAttachRuntime();
            if (!runtime.Ok()) { PublishRuntimeFailure(runtime); return; }
            cabbird::launcher::ManualMapOptions options;
            options.process_id = process_id;
            options.core_path = runtime.core_path;
            options.runtime_root = runtime.runtime_root;
            options.log_directory = runtime.runtime_root / L"logs";
            const auto result = cabbird::launcher::ManualMapRuntimeCore(options);
            if (result.Ok()) RefreshProcessesImpl(ProcessAnnounce::Never);
            std::scoped_lock lock(state_mutex_);
            if (result.Ok()) {
                state_.attached_process = process_id;
                const std::string pid = std::to_string(process_id);
                state_.message = MakeLauncherMessage(cabbird::MessageId::LauncherStatusProcessAttached, {pid});
                state_.message_kind = MessageKind::Success;
            } else {
                const std::string error = std::to_string(result.win32_error);
                state_.message = MakeLauncherMessage(cabbird::MessageId::LauncherStatusProcessAttachFailed,
                    {error}, result.message);
                state_.message_kind = MessageKind::Error;
            }
        });
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
            configuration_ = std::move(updated);
            state_.theme = configuration_.theme;
            state_.language = configuration_.language;
            state_.artwork = configuration_.artwork;
            state_.custom_colors = configuration_.custom_colors;
            state_.frame_rate_limit = configuration_.frame_rate_limit;
            state_.settings_error.clear();
            if (translator != nullptr) state_.translator = std::move(translator);
            if (theme != nullptr) state_.theme_layout = std::move(theme);
            state_.message = MakeLauncherMessage(cabbird::MessageId::LauncherStatusSettingsSaved);
            state_.message_kind = MessageKind::Success;
    }

    [[nodiscard]] cabbird::RuntimeLaunchResult ResolveAttachRuntime() const {
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

    void RefreshProcessesImpl(ProcessAnnounce announce = ProcessAnnounce::Always) {
        auto processes = cabbird::launcher::EnumerateAttachableProcesses();
        const auto game_directory = GameDirectory();
        if (!game_directory.empty()) {
            std::erase_if(processes, [&game_directory](const auto& process) {
                return process.executable_path.empty() ||
                    !PathsEqual(process.executable_path.parent_path(), game_directory);
            });
        }
        const auto runtime = ResolveAttachRuntime();
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
                ? cabbird::MessageId::LauncherStatusProcessAttached
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
    std::function<void()> launch_and_attach;
    std::function<void(DWORD)> attach_to_process;
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
        [&controller] { controller.LaunchAndAttach(); },
        [&controller](DWORD process) { controller.AttachToProcess(process); },
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

DWORD DrawProcessOverview(const LauncherActions& actions, const LauncherSnapshot& snapshot,
    const cabbird::Translator& translator) {
    ImGui::Dummy(Scale(0.0f, 14.0f)); ImGui::Separator(); ImGui::Dummy(Scale(0.0f, 12.0f));
    const float status_anchor = ImGui::GetCursorPosY();
    ImGui::TextColored(LauncherMutedInk(), "%s", Text(translator, cabbird::MessageId::LauncherSectionProcesses));
    const auto target = std::find_if(snapshot.processes.begin(), snapshot.processes.end(),
        [](const auto& process) { return process.Compatible(); });
    const DWORD target_id = target == snapshot.processes.end() ? 0 : target->process_id;
    const std::string process_id = std::to_string(snapshot.attached_process != 0
        ? snapshot.attached_process : target_id);
    const std::string status = LauncherMessageVisible(snapshot) ? RenderMessage(translator, snapshot.message)
        : !snapshot.core_available ? std::string(Text(translator, cabbird::MessageId::LauncherStatusCoreUnavailable))
        : snapshot.processes.empty() ? std::string(Text(translator, cabbird::MessageId::LauncherStatusNoProcesses))
        : Format(translator, snapshot.attached_process != 0
            ? cabbird::MessageId::LauncherStatusProcessAttached : cabbird::MessageId::LauncherStatusProcessFound, {process_id});
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
    return target_id;
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
    const DWORD selected_process = DrawProcessOverview(actions, snapshot, translator);
    ImGui::EndChild();
    const bool can_attach = !snapshot.busy && snapshot.core_available && selected_process != 0 && selected_process != snapshot.attached_process;
    const bool can_launch = !snapshot.busy && snapshot.core_available && !snapshot.launcher_executable.empty() && snapshot.processes.empty();
    const char* attach_label = Text(translator, cabbird::MessageId::LauncherAttachExisting);
    const char* launch_label = Text(translator, cabbird::MessageId::LauncherLaunchAttach);
    const float attach_width = std::max(200.0f, CommandButtonWidth(0xe8e5, attach_label));
    const float launch_width = std::max(240.0f, CommandButtonWidth(0xe768, launch_label));
    const float total = Scale(attach_width + launch_width + 14.0f);
    ImGui::SetCursorPosY(action_y);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - total));
    if (CommandButton("attach-existing", 0xe8e5, attach_label, false, can_attach,
            {attach_width, kLaunchActionRowHeight})) actions.attach_to_process(selected_process);
    ImGui::SameLine(0.0f, Scale(14.0f));
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
    if (!snapshot.runtime_version.empty()) {
        ImGui::Dummy(Scale(0.0f, 12.0f));
        const auto runtime = Format(translator, cabbird::MessageId::LauncherRuntimeVersion, {snapshot.runtime_version});
        ImGui::TextColored(LauncherMutedInk(), "%s", runtime.c_str());
    }
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
    ImGui::End(); ImGui::PopStyleVar();
}

int DrawLauncher(HWND window, Graphics& graphics, LauncherController& controller,
    LauncherMode& mode, const cabbird::Translator& translator) {
    controller.PollProcesses();
    const auto snapshot = controller.Snapshot();
    DrawLauncher(window, graphics, snapshot, ActionsFor(controller), mode,
        snapshot.translator != nullptr ? *snapshot.translator : translator);
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

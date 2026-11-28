#pragma once

#include "cabbird/cabbird_ui_theme.hpp"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cabbird::launcher {

enum class AzurPromiliaClient : std::uint8_t {
    MainlandChina,
    Global,
};

struct LauncherClientConfiguration final {
    std::filesystem::path game_directory;
    std::filesystem::path launcher_executable;
};

struct LauncherThemeArtwork final {
    // Relative to the Cabbird runtime root, so the installation remains movable.
    std::string background;
    int background_mode{-1};  // -1 follows layout; 0 disabled, 1 center, 2 crop, 3 stretch.
    float surface_opacity{};  // -1 follows the layout's veil.

    friend bool operator==(const LauncherThemeArtwork&, const LauncherThemeArtwork&) = default;
};

inline constexpr std::array<int, 5> kLauncherFrameRates{15, 30, 60, 120, 240};
inline constexpr int kLauncherDefaultFrameRate = 120;

[[nodiscard]] inline constexpr int LauncherFrameRatePreset(const double requested) noexcept {
    for (std::size_t index = 1; index < kLauncherFrameRates.size(); ++index) {
        if (requested <= (kLauncherFrameRates[index - 1] + kLauncherFrameRates[index]) / 2.0)
            return kLauncherFrameRates[index - 1];
    }
    return kLauncherFrameRates.back();
}

struct LauncherUpdateConfiguration final {
    bool auto_check{true};
    std::string endpoint;
    std::string skipped_version;
};

struct LauncherConfiguration final {
    AzurPromiliaClient selected_client{AzurPromiliaClient::MainlandChina};
    // Launcher appearance is independent of the in-game platform settings.
    std::string theme{"paper"};
    // Empty preserves the runtime ini language when upgrading an older launcher.
    std::string language;
    LauncherThemeArtwork artwork;
    LauncherUpdateConfiguration update;
    CabbirdUiCustomColors custom_colors;
    int frame_rate_limit{kLauncherDefaultFrameRate};
    LauncherClientConfiguration mainland_china;
    LauncherClientConfiguration global;

    [[nodiscard]] LauncherClientConfiguration& Selected() noexcept {
        return selected_client == AzurPromiliaClient::Global ? global : mainland_china;
    }

    [[nodiscard]] const LauncherClientConfiguration& Selected() const noexcept {
        return selected_client == AzurPromiliaClient::Global ? global : mainland_china;
    }
};

struct LauncherConfigurationLoadResult final {
    LauncherConfiguration configuration;
    bool loaded{};
    DWORD win32_error{ERROR_SUCCESS};
    std::string message;
};

struct LauncherConfigurationSaveResult final {
    DWORD win32_error{ERROR_SUCCESS};
    std::string message;

    [[nodiscard]] bool Ok() const noexcept { return win32_error == ERROR_SUCCESS; }
};

struct LauncherDiscoveryOptions final {
    AzurPromiliaClient client{AzurPromiliaClient::MainlandChina};
    std::filesystem::path payload_root;
    std::vector<std::filesystem::path> running_game_executables;
    std::vector<std::filesystem::path> running_launcher_executables;
    std::vector<std::filesystem::path> search_roots;
    bool include_system_locations{true};
    bool allow_unpaired_game_discovery{};
};

[[nodiscard]] std::filesystem::path LauncherConfigurationPath(
    const std::filesystem::path& payload_root) noexcept;
[[nodiscard]] LauncherConfigurationLoadResult LoadLauncherConfiguration(
    const std::filesystem::path& path) noexcept;
[[nodiscard]] LauncherConfigurationSaveResult SaveLauncherConfiguration(
    const std::filesystem::path& path,
    const LauncherConfiguration& configuration) noexcept;

[[nodiscard]] bool IsAzurPromiliaGameDirectory(
    const std::filesystem::path& directory) noexcept;
[[nodiscard]] bool IsAzurPromiliaLauncherExecutable(
    const std::filesystem::path& executable, AzurPromiliaClient client) noexcept;
[[nodiscard]] LauncherClientConfiguration DiscoverLauncherConfiguration(
    const LauncherClientConfiguration& preferred,
    const LauncherDiscoveryOptions& options);

}  // namespace cabbird::launcher

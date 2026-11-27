#pragma once

#include <Windows.h>

#include <filesystem>
#include <memory>
#include <string>

namespace cabbird {
struct AnalyzerConfig;
struct PlatformDiagnostics;
struct UnityProfileRuntimeOptions;
class UnityProfileRuntime;
}

namespace cabbird::app {

[[nodiscard]] UnityProfileRuntimeOptions MakeUnityRuntimeOptions(
    const std::filesystem::path& runtime_root,
    HMODULE game_module,
    const AnalyzerConfig& config);

// Adapter evidence is translated to the UI model at the application boundary.
// Neither the Runtime kernel nor the renderer needs to understand Profile data.
void BindUnityDiagnostics(
    PlatformDiagnostics& diagnostics,
    std::weak_ptr<UnityProfileRuntime> profile,
    std::string game_id);

}  // namespace cabbird::app

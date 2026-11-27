#include "unity_runtime.hpp"

#include "cabbird/config.hpp"
#include "cabbird/platform_host.hpp"
#include "cabbird/unity_profile_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace cabbird::app {

UnityProfileRuntimeOptions MakeUnityRuntimeOptions(
    const std::filesystem::path& runtime_root,
    HMODULE game_module,
    const AnalyzerConfig& config) {
    UnityProfileRuntimeOptions options;
    options.runtime_root = runtime_root;
    options.start_frame_clock = config.platform_enabled;
    options.game_id = config.game_id;
    options.game_module = game_module;
    const auto interval = [](std::size_t value) {
        return static_cast<std::uint32_t>((std::min)(value,
            static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
    };
    options.snapshot_sampling.player_tick_interval = interval(config.player_snapshot_tick_interval);
    options.snapshot_sampling.entity_tick_interval = interval(config.entity_snapshot_tick_interval);
    options.snapshot_sampling.entity_direct_position = config.entity_direct_position;
    return options;
}

void BindUnityDiagnostics(
    PlatformDiagnostics& diagnostics,
    std::weak_ptr<UnityProfileRuntime> profile,
    std::string game_id) {
    diagnostics.profile_json = [profile] {
        const auto runtime = profile.lock();
        return runtime ? runtime->DiagnosticsJson()
                       : std::string{"{\"state\":\"unavailable\"}"};
    };
    diagnostics.unity_compatibility = [profile, game_id = std::move(game_id)] {
        UnityProfileDocumentStatus document;
        document.game = game_id;
        const auto runtime = profile.lock();
        if (runtime == nullptr) {
            document.error = "the profile runtime is not running";
            return BuildUnityCompatibilitySnapshot(document);
        }
        const auto current = runtime->Evidence();
        document.loaded = current.unity_profile.has_value();
        document.source = current.unity_profile_path.string();
        document.hash = current.unity_profile_hash;
        document.error = current.unity_profile_error;
        if (current.unity_profile.has_value()) {
            document.build_id = current.unity_profile->BuildId();
            document.assembly = current.unity_profile->GameAssemblyName();
        }
        document.methods.reserve(current.unity_methods.size());
        for (const auto& method : current.unity_methods) {
            document.methods.push_back({method.key, method.bound, method.how});
        }
        return BuildUnityCompatibilitySnapshot(document);
    };
    diagnostics.hooks = [profile] {
        const auto runtime = profile.lock();
        return runtime ? runtime->Hooks() : std::vector<HookRecordView>{};
    };
}

}  // namespace cabbird::app

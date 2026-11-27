#include "cabbird/sdk/cpp.hpp"
#include "cabbird/sdk/localization.hpp"
#include "cabbird/sdk/profile.hpp"
#include "cabbird/sdk/services/game_lifecycle.h"
#include "cabbird/sdk/config_save_queue.hpp"

#include <Windows.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace cabbird::sdk;

constexpr auto kId = "cabbird.story-tools";
constexpr auto kVersion = "1.0.0";
constexpr auto kConfigId = "settings";
constexpr std::string_view kSchema = R"({"type":"object","required":["enabled","unlockSkip","autoPlay","speedEnabled","playbackSpeed"],"additionalProperties":false,"properties":{"enabled":{"type":"boolean"},"unlockSkip":{"type":"boolean"},"autoPlay":{"type":"boolean"},"autoSkip":{"type":"boolean"},"speedEnabled":{"type":"boolean"},"playbackSpeed":{"type":"number","minimum":1,"maximum":9}}})";
constexpr auto kStoryManager = "Lens.Gameplay.Story.StoryManager";
constexpr auto kStoryPage = "Azur.Gameplay.UI.PageStory";
constexpr auto kStorySection = "Lens.Gameplay.Story.StorySection";
constexpr float kMinimumSpeed = 1;
constexpr float kMaximumSpeed = 9;

struct Settings final {
    bool enabled{};
    bool unlock_skip{true};
    bool auto_play{};
    bool auto_skip{};
    bool speed_enabled{};
    float playback_speed{3};
};

enum class FeatureState { Disabled, Waiting, Live, ProfileUnavailable, MetadataUnavailable, HookRefused, ReleaseFailed };
enum class Feature : std::size_t { Skip, Auto, Speed, Count };
constexpr auto kFeatureCount = static_cast<std::size_t>(Feature::Count);

struct HookSlot final {
    std::uintptr_t original{};
    CabbirdGenerationHandleV1 handle{};
};

struct Binding final {
    std::uintptr_t target{};
    std::uintptr_t caller{};
    bool attempted{}; // Game owned; unresolved startup metadata retries.
    ULONGLONG retry_at{};
    HookSlot hook;
    std::atomic<FeatureState> state{FeatureState::Waiting};
    FeatureState reported_state{FeatureState::Waiting}; // Game only.
};

enum class SpeedState { Idle, Waiting, Playing, Paused, Unavailable, RestoreFailed };
enum class SpeedMethod : std::size_t { Manager, Section, CurrentSpeed, SetSpeed, SetAudioSpeed, Count };
struct SpeedRuntime final {
    std::array<std::uintptr_t, static_cast<std::size_t>(SpeedMethod::Count)> methods{};
    std::uint32_t paused_offset{}, audio_paused_offset{}, destroyed_offset{}, initialized_offset{};
    float native_maximum{};
    bool validated{}, applied{}; // Game only. No managed object survives a call.
    std::atomic<SpeedState> state{SpeedState::Idle};
    SpeedState reported_state{SpeedState::Idle}; // Game only.
};

enum class SkipMethod : std::size_t { Manager, Page, PageCanSkip, Redirect, Skip, Count };
struct SkipRuntime final {
    std::array<std::uintptr_t, static_cast<std::size_t>(SkipMethod::Count)> methods{};
    std::uint32_t branch_offset{}, paused_offset{};
    bool attempted{}, validated{}; // Game only; objects are borrowed synchronously.
    ULONGLONG retry_at{}, next_action_at{};
    FeatureState state{FeatureState::Waiting}, reported_state{FeatureState::Waiting};
};

struct Context final {
    explicit Context(const CabbirdHostApiV1* raw) : host(raw), localizer(raw) {}

    Host host;
    Localizer localizer;
    const CabbirdCoreServiceV1* core{};
    const CabbirdHookServiceV1* hooks{};
    const CabbirdUnityMetadataServiceV1* metadata{};
    const CabbirdWindowServiceV1* windows{};
    const CabbirdConfigServiceV1* config{};
    const CabbirdJsonServiceV1* json{};
    const CabbirdGameLifecycleServiceV1* game_lifecycle{};
    CabbirdGenerationHandleV1 window{};
    CabbirdGenerationHandleV1 cleanup{};
    Profile profile;
    std::array<Binding, kFeatureCount> bindings;
    SpeedRuntime speed;
    SkipRuntime skip;
    cabbird::sdk::ConfigSaveQueue saves;
    std::mutex settings_mutex;
    Settings settings;
    std::atomic<bool> running{};
    std::atomic<bool> unlock_requested{};
    std::atomic<bool> auto_requested{};
    std::atomic<bool> skip_requested{};
    std::atomic<bool> speed_requested{};
    std::atomic<float> playback_speed{3};
    std::atomic<DWORD> game_thread{};
    std::atomic<bool> save_failed{};
};

void LogFailure(Context& ctx, std::string_view message) {
    const auto line = std::string("Story Tools: ") + std::string(message);
    ctx.core->log(ctx.core->user, CABBIRD_CORE_LOG_LEVEL_V1_ERROR, StringView(line));
}

void ReportFailures(Context& ctx) {
    constexpr const char* features[]{"skip", "auto-play", "speed"};
    for (std::size_t index = 0; index < kFeatureCount; ++index) {
        auto& binding = ctx.bindings[index];
        const auto state = binding.state.load(std::memory_order_relaxed);
        if (state == binding.reported_state) continue;
        binding.reported_state = state;
        const char* reason{};
        switch (state) {
            case FeatureState::ProfileUnavailable: reason = "profile entry or caller did not match"; break;
            case FeatureState::MetadataUnavailable: reason = "runtime method shape did not match"; break;
            case FeatureState::HookRefused: reason = "host refused the hook"; break;
            case FeatureState::ReleaseFailed: reason = "hook removal did not drain"; break;
            default: break;
        }
        if (reason) LogFailure(ctx, std::string(features[index]) + " binding failed: " + reason + '.');
    }
    if (ctx.skip.state != ctx.skip.reported_state) {
        ctx.skip.reported_state = ctx.skip.state;
        if (ctx.skip.state == FeatureState::ProfileUnavailable)
            LogFailure(ctx, "Automatic skip binding failed: profile entries did not match.");
        else if (ctx.skip.state == FeatureState::MetadataUnavailable)
            LogFailure(ctx, "Automatic skip binding failed: runtime method or field shape did not match.");
    }
    const auto state = ctx.speed_requested.load(std::memory_order_relaxed) || ctx.speed.applied
        ? ctx.speed.state.load(std::memory_order_relaxed) : SpeedState::Idle;
    if (state == ctx.speed.reported_state) return;
    ctx.speed.reported_state = state;
    if (state == SpeedState::Unavailable && ctx.bindings[static_cast<std::size_t>(Feature::Speed)].state.load() == FeatureState::Live)
        LogFailure(ctx, "Native speed state could not be validated or confirmed.");
    else if (state == SpeedState::RestoreFailed) LogFailure(ctx, "The game's configured speed could not be restored.");
}

// A detour has no callback_user parameter. This pointer names only this DLL's
// generation; the host revokes/drains its hooks before Unload destroys Context.
std::atomic<Context*> g_active{};

constexpr CabbirdStatusV1 Error(std::uint32_t code) noexcept { return {code, 0, {}}; }
CabbirdByteSpanV1 Bytes(std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

class JsonValue final {
public:
    explicit JsonValue(const CabbirdJsonServiceV1* service) noexcept : service_(service) {}
    ~JsonValue() { if (handle.id) static_cast<void>(service_->release(service_->user, handle)); }
    JsonValue(const JsonValue&) = delete;
    JsonValue& operator=(const JsonValue&) = delete;
    CabbirdGenerationHandleV1 handle{};
private:
    const CabbirdJsonServiceV1* service_;
};

bool ReadBoolean(Context& ctx, CabbirdGenerationHandleV1 document, const char* key, bool& output) {
    JsonValue value(ctx.json);
    std::int32_t parsed{};
    if (!Succeeded(ctx.json->object_find(ctx.json->user, document, StringView(key), &value.handle)) ||
        !Succeeded(ctx.json->boolean_value(ctx.json->user, value.handle, &parsed))) return false;
    output = parsed != 0;
    return true;
}

bool RestoreSettings(Context& ctx) {
    std::uint32_t version{};
    std::size_t size{};
    auto status = ctx.config->read(ctx.config->user, StringView(kConfigId), &version, {}, &size);
    if (status.code == CABBIRD_STATUS_V1_NOT_FOUND) return true;
    if ((status.code != CABBIRD_STATUS_V1_OK && status.code != CABBIRD_STATUS_V1_BUFFER_TOO_SMALL) ||
        version != 1 || size == 0 || size > 4096) return false;
    std::vector<std::uint8_t> bytes(size);
    status = ctx.config->read(ctx.config->user, StringView(kConfigId), &version, {bytes.data(), bytes.size()}, &size);
    if (!Succeeded(status) || version != 1 || size > bytes.size()) return false;
    JsonValue document(ctx.json);
    if (!Succeeded(ctx.json->parse(ctx.json->user, {reinterpret_cast<const char*>(bytes.data()), size},
            &document.handle))) return false;
    Settings settings;
    if (!ReadBoolean(ctx, document.handle, "enabled", settings.enabled) ||
        !ReadBoolean(ctx, document.handle, "unlockSkip", settings.unlock_skip) ||
        !ReadBoolean(ctx, document.handle, "autoPlay", settings.auto_play) ||
        !ReadBoolean(ctx, document.handle, "speedEnabled", settings.speed_enabled)) return false;
    // Version 1 documents written before automatic skipping remain valid.
    JsonValue auto_skip(ctx.json);
    const auto auto_skip_status = ctx.json->object_find(ctx.json->user, document.handle, StringView("autoSkip"), &auto_skip.handle);
    if (auto_skip_status.code != CABBIRD_STATUS_V1_NOT_FOUND &&
        (!Succeeded(auto_skip_status) || !ReadBoolean(ctx, document.handle, "autoSkip", settings.auto_skip))) return false;
    JsonValue speed(ctx.json);
    double parsed_speed{};
    if (!Succeeded(ctx.json->object_find(ctx.json->user, document.handle, StringView("playbackSpeed"), &speed.handle)) ||
        !Succeeded(ctx.json->number_value(ctx.json->user, speed.handle, &parsed_speed)) ||
        !std::isfinite(parsed_speed) || parsed_speed < kMinimumSpeed || parsed_speed > kMaximumSpeed) return false;
    settings.playback_speed = static_cast<float>(parsed_speed);
    ctx.settings = settings;
    return true;
}

void QueueSettings(Context& ctx, const Settings& settings) {
    std::string document = "{\"enabled\":";
    document += settings.enabled ? "true" : "false";
    document += ",\"unlockSkip\":";
    document += settings.unlock_skip ? "true" : "false";
    document += ",\"autoPlay\":";
    document += settings.auto_play ? "true" : "false";
    document += ",\"autoSkip\":";
    document += settings.auto_skip ? "true" : "false";
    document += ",\"speedEnabled\":";
    document += settings.speed_enabled ? "true" : "false";
    document += ",\"playbackSpeed\":";
    char speed[48]{};
    const auto converted = std::to_chars(speed, speed + sizeof(speed), settings.playback_speed,
        std::chars_format::general, std::numeric_limits<float>::max_digits10);
    if (converted.ec != std::errc{}) {
        if (!ctx.save_failed.exchange(true)) LogFailure(ctx, "Settings could not be serialized.");
        return;
    }
    document.append(speed, converted.ptr);
    document += '}';
    const bool failed = !ctx.saves.Queue(document);
    if (failed && !ctx.save_failed.exchange(true)) LogFailure(ctx, "Settings could not be queued for saving.");
    if (!failed) ctx.save_failed = false;
}

void PublishIntent(Context& ctx, const Settings& settings) noexcept {
    ctx.unlock_requested.store(settings.enabled && settings.unlock_skip, std::memory_order_release);
    ctx.auto_requested.store(settings.enabled && settings.auto_play, std::memory_order_release);
    ctx.skip_requested.store(settings.enabled && settings.auto_skip, std::memory_order_release);
    ctx.playback_speed.store(settings.playback_speed, std::memory_order_relaxed);
    ctx.speed_requested.store(settings.enabled && settings.speed_enabled, std::memory_order_release);
}

struct MethodShape final {
    const char* symbol;
    const char* type;
    const char* method;
    const char* result;
    std::uint32_t kind;
    std::span<const CabbirdStringViewV1> parameters{};
};
constexpr MethodShape kTargets[] = {
    {"skipPermission", kStoryManager, "get_IsDebugForceCanSkip", "System.Boolean", CABBIRD_UNITY_METHOD_V1_STATIC},
    {"autoPlay", kStoryManager, "IsAutoPlay", "System.Boolean", CABBIRD_UNITY_METHOD_V1_INSTANCE},
    {"storySpeed", kStoryManager, "GetCurrentStoryPlaySpeed", "System.Single", CABBIRD_UNITY_METHOD_V1_INSTANCE}
};
constexpr MethodShape kCallers[] = {
    {"pageCanSkip", kStoryPage, "get_canSkip", "System.Boolean", CABBIRD_UNITY_METHOD_V1_INSTANCE},
    {"pageCheckAuto", kStoryPage, "CheckAuto", "System.Void", CABBIRD_UNITY_METHOD_V1_INSTANCE},
    {"refreshPlaySpeed", kStorySection, "RefreshPlaySpeed", "System.Void", CABBIRD_UNITY_METHOD_V1_INSTANCE}
};
constexpr const char* kEdges[] = {"pageCanSkipToPermission", "pageCheckAutoToAutoPlay", "refreshToStorySpeed"};
constexpr CabbirdStringViewV1 kSpeedParameters[] = {StringView("System.Single"), StringView("System.Boolean")};
constexpr CabbirdStringViewV1 kAudioSpeedParameters[] = {StringView("System.Single")};
constexpr MethodShape kSpeedMethods[] = {
    {"storyManager", "Lens.Gameplay.Modules.Azur.AzurWorld", "get_StoryMgr", "Lens.Gameplay.Story.StoryManager", CABBIRD_UNITY_METHOD_V1_STATIC},
    {"storySection", kStoryManager, "get_Section", "Lens.Gameplay.Story.StorySection", CABBIRD_UNITY_METHOD_V1_INSTANCE},
    {"sectionSpeed", kStorySection, "get_playSpeed", "System.Single", CABBIRD_UNITY_METHOD_V1_INSTANCE},
    {"setPlaySpeed", kStorySection, "SetPlaySpeed", "System.Void", CABBIRD_UNITY_METHOD_V1_INSTANCE, kSpeedParameters},
    {"setAudioSpeed", kStorySection, "SetAudioSpeed", "System.Void", CABBIRD_UNITY_METHOD_V1_INSTANCE, kAudioSpeedParameters}
};
constexpr MethodShape kSkipMethods[] = {
    {"storyManager", "Lens.Gameplay.Modules.Azur.AzurWorld", "get_StoryMgr", "Lens.Gameplay.Story.StoryManager", CABBIRD_UNITY_METHOD_V1_STATIC},
    {"storyPage", kStoryManager, "GetPageStory", "Azur.Gameplay.UI.PageStory", CABBIRD_UNITY_METHOD_V1_STATIC},
    {"pageCanSkip", kStoryPage, "get_canSkip", "System.Boolean", CABBIRD_UNITY_METHOD_V1_INSTANCE},
    {"redirectSkip", kStoryManager, "TryRedirectSkipAction", "System.Boolean", CABBIRD_UNITY_METHOD_V1_INSTANCE},
    {"skipCurrentStory", kStoryPage, "SkipCurrentStory", "System.Void", CABBIRD_UNITY_METHOD_V1_INSTANCE}
};

void PrepareProfile(Context& ctx) {
    auto loaded = LoadProfile(ctx.host, "compatibility.profile.json",
        {"ap", "GameAssembly.dll", "il2cpp", "story-tools-v2"});
    if (!loaded) {
        for (auto& binding : ctx.bindings) binding.state = FeatureState::ProfileUnavailable;
        return;
    }
    ctx.profile = std::move(loaded).value();
    try {
        for (std::size_t index = 0; index < std::size(kSkipMethods); ++index)
            ctx.skip.methods[index] = ResolveProfileSymbolRva(ctx.core, ctx.profile, kSkipMethods[index].symbol).address;
        ctx.skip.branch_offset = ctx.profile.Value("pageBranchOpen", 4096);
        ctx.skip.paused_offset = ctx.profile.Value("pagePaused", 4096);
    } catch (...) { ctx.skip.methods = {}; }
    for (std::size_t index = 0; index < kFeatureCount; ++index) {
        auto& binding = ctx.bindings[index];
        // The SDK decodes the edge candidate's E8 callOffset and checks the
        // target bytes too. A name/RVA that is never called is insufficient.
        const auto target = ResolveProfileSymbolRva(ctx.core, ctx.profile, kTargets[index].symbol);
        const auto caller = ResolveProfileSymbolRva(ctx.core, ctx.profile, kCallers[index].symbol);
        const auto edge = ResolveProfileSymbolRva(ctx.core, ctx.profile, kEdges[index]);
        if (!target || !caller || !edge || target.address != edge.address) {
            binding.state = FeatureState::ProfileUnavailable;
            continue;
        }
        binding.target = target.address;
        binding.caller = caller.address;
    }
    try {
        for (std::size_t index = 0; index < std::size(kSpeedMethods); ++index) {
            const auto method = ResolveProfileSymbolRva(ctx.core, ctx.profile, kSpeedMethods[index].symbol);
            ctx.speed.methods[index] = method.address;
        }
        const auto setter_edge = ResolveProfileSymbolRva(ctx.core, ctx.profile, "refreshToSetPlaySpeed");
        const auto audio_edge = ResolveProfileSymbolRva(ctx.core, ctx.profile, "setPlaySpeedToAudio");
        if (setter_edge.address != ctx.speed.methods[static_cast<std::size_t>(SpeedMethod::SetSpeed)] ||
            audio_edge.address != ctx.speed.methods[static_cast<std::size_t>(SpeedMethod::SetAudioSpeed)]) {
            ctx.speed.methods = {};
        }
        ctx.speed.paused_offset = ctx.profile.Value("sectionPaused", 4096);
        ctx.speed.audio_paused_offset = ctx.profile.Value("sectionAudioPaused", 4096);
        ctx.speed.destroyed_offset = ctx.profile.Value("sectionDestroyed", 4096);
        ctx.speed.initialized_offset = ctx.profile.Value("sectionInitialized", 4096);
        ctx.speed.native_maximum = static_cast<float>(ctx.profile.Value("nativeSpeedMaximumExclusive", 100));
        const auto maximum_rva = ctx.profile.Value("nativeSpeedMaximumRva", (std::numeric_limits<std::uint32_t>::max)());
        float native_maximum{};
        const auto module = ctx.core->module_base(ctx.core->user, StringView(ctx.profile.module));
        if (!module || !Succeeded(ctx.core->read_memory(ctx.core->user, module + maximum_rva,
                {reinterpret_cast<std::uint8_t*>(&native_maximum), sizeof(native_maximum)})) ||
            native_maximum != ctx.speed.native_maximum) ctx.speed.native_maximum = 0;
        if (ctx.speed.native_maximum <= kMaximumSpeed ||
            std::find(ctx.speed.methods.begin(), ctx.speed.methods.end(), std::uintptr_t{}) != ctx.speed.methods.end())
            ctx.bindings[static_cast<std::size_t>(Feature::Speed)].target = 0;
        } catch (...) {
        ctx.bindings[static_cast<std::size_t>(Feature::Speed)].target = 0;
        ctx.speed.state = SpeedState::Unavailable;
    }
}

bool MetadataPending(CabbirdStatusV1 status) noexcept {
    // Types and methods can appear after Game callbacks start. NOT_FOUND does
    // not establish a mismatched resolved entry; the metadata ABI returns it
    // for both absent types and methods with no complete shape yet.
    return status.code == CABBIRD_STATUS_V1_UNAVAILABLE || status.code == CABBIRD_STATUS_V1_NOT_FOUND;
}

bool ValidateMethod(Context& ctx, const MethodShape& shape, std::uintptr_t expected, bool* unavailable = nullptr) {
    CabbirdUnityMethodRequestV1 request{};
    request.struct_size = sizeof(request);
    request.method_kind = shape.kind;
    request.module_name = StringView(ctx.profile.module);
    request.type_name = StringView(shape.type);
    request.method_name = StringView(shape.method);
    request.return_type = StringView(shape.result);
    request.parameter_types = shape.parameters.data();
    request.parameter_count = shape.parameters.size();
    CabbirdUnityMethodResultV1 method{sizeof(method)};
    const auto status = ctx.metadata->resolve_method(ctx.metadata->user, &request, &method);
    if (unavailable && MetadataPending(status)) *unavailable = true;
    return Succeeded(status) &&
        method.address == expected && method.address != 0;
}

bool ValidateSpeedRuntime(Context& ctx, bool& unavailable) {
    for (std::size_t index = 0; index < std::size(kSpeedMethods); ++index) {
        const auto address = ctx.speed.methods[index];
        if (!ValidateMethod(ctx, kSpeedMethods[index], address, &unavailable) ||
            !MatchesProfileEntry(ctx.core, ctx.profile, kSpeedMethods[index].symbol, address)) return false;
    }
    const auto field_matches = [&](const char* name, std::uint32_t expected) {
        CabbirdUnityFieldRequestV1 request{sizeof(request), sizeof(std::uint8_t),
            StringView(kStorySection), StringView(name), StringView("System.Boolean")};
        CabbirdUnityFieldResultV1 field{sizeof(field)};
        const auto status = ctx.metadata->resolve_instance_field(ctx.metadata->user, &request, &field);
        if (MetadataPending(status)) unavailable = true;
        return Succeeded(status) &&
            field.offset == expected && field.storage_size == sizeof(std::uint8_t);
    };
    ctx.speed.validated = ctx.speed.native_maximum > kMaximumSpeed &&
        field_matches("m_IsPaused", ctx.speed.paused_offset) &&
        field_matches("m_IsPauseAudio", ctx.speed.audio_paused_offset) &&
        field_matches("m_IsDestroy", ctx.speed.destroyed_offset) &&
        field_matches("m_IsInitFinished", ctx.speed.initialized_offset);
    return ctx.speed.validated;
}

using ManagerFn = std::uintptr_t(CABBIRD_CALL*)(std::uintptr_t);
using SectionFn = std::uintptr_t(CABBIRD_CALL*)(std::uintptr_t, std::uintptr_t);
using ReadSpeedFn = float(CABBIRD_CALL*)(std::uintptr_t, std::uintptr_t);
using SetSpeedFn = void(CABBIRD_CALL*)(std::uintptr_t, float, bool, std::uintptr_t);
using SetAudioSpeedFn = void(CABBIRD_CALL*)(std::uintptr_t, float, std::uintptr_t);

template <typename Function> Function SpeedFunction(Context& ctx, SpeedMethod method) noexcept {
    return reinterpret_cast<Function>(ctx.speed.methods[static_cast<std::size_t>(method)]);
}

bool ReadFlag(Context& ctx, std::uintptr_t section, std::uint32_t offset, bool& value) {
    std::uint8_t byte{};
    if (!Succeeded(ctx.core->read_memory(ctx.core->user, section + offset, {&byte, sizeof(byte)})) || byte > 1) return false;
    value = byte != 0;
    return true;
}

void SyncAutomaticSkip(Context& ctx, bool requested) {
    auto& skip = ctx.skip;
    if (!requested) {
        skip.state = FeatureState::Disabled;
        skip.attempted = false;
        skip.retry_at = skip.next_action_at = 0;
        return;
    }
    const auto now = GetTickCount64();
    if (!skip.validated) {
        if (skip.attempted || now < skip.retry_at) return;
        skip.attempted = true;
        if (std::find(skip.methods.begin(), skip.methods.end(), std::uintptr_t{}) != skip.methods.end()) {
            skip.state = FeatureState::ProfileUnavailable;
            return;
        }
        bool unavailable{};
        bool matches = true;
        for (std::size_t index = 0; matches && index < std::size(kSkipMethods); ++index)
            matches = ValidateMethod(ctx, kSkipMethods[index], skip.methods[index], &unavailable) &&
                MatchesProfileEntry(ctx.core, ctx.profile, kSkipMethods[index].symbol, skip.methods[index]);
        const auto field_matches = [&](const char* name, std::uint32_t expected) {
            CabbirdUnityFieldRequestV1 request{sizeof(request), sizeof(std::uint8_t),
                StringView(kStoryPage), StringView(name), StringView("System.Boolean")};
            CabbirdUnityFieldResultV1 field{sizeof(field)};
            const auto status = ctx.metadata->resolve_instance_field(ctx.metadata->user, &request, &field);
            if (MetadataPending(status)) unavailable = true;
            return Succeeded(status) && field.offset == expected && field.storage_size == sizeof(std::uint8_t);
        };
        matches = matches && field_matches("m_IsBranchOpen", skip.branch_offset) &&
            field_matches("m_IsUIPause", skip.paused_offset);
        if (!matches) {
            skip.state = unavailable ? FeatureState::Waiting : FeatureState::MetadataUnavailable;
            if (unavailable) { skip.attempted = false; skip.retry_at = now + 1000; }
            return;
        }
        skip.validated = true;
    }
    skip.state = FeatureState::Live;
    if (now < skip.next_action_at) return;
    const auto manager = reinterpret_cast<ManagerFn>(skip.methods[static_cast<std::size_t>(SkipMethod::Manager)])(0);
    const auto page = manager ? reinterpret_cast<ManagerFn>(skip.methods[static_cast<std::size_t>(SkipMethod::Page)])(0) : 0;
    if (!page) return;
    bool branch{}, paused{};
    if (!ReadFlag(ctx, page, skip.branch_offset, branch) || !ReadFlag(ctx, page, skip.paused_offset, paused)) return;
    // Keep branch choices and review pauses under game control. Native canSkip
    // owns permission, including the explicit unlock override; checking its raw
    // external-forbid flag again would cancel that override.
    if (branch || paused) return;
    using Query = bool(CABBIRD_CALL*)(std::uintptr_t, std::uintptr_t);
    using Action = void(CABBIRD_CALL*)(std::uintptr_t, std::uintptr_t);
    if (!reinterpret_cast<Query>(skip.methods[static_cast<std::size_t>(SkipMethod::PageCanSkip)])(page, 0)) return;
    skip.next_action_at = now + 250;
    if (reinterpret_cast<Query>(skip.methods[static_cast<std::size_t>(SkipMethod::Redirect)])(manager, 0)) return;
    // Match the final confirmation's permission -> virtual DoSkipStory path.
    // m_isSkipStory is set when confirmation OPENS, before skipping executes;
    // it must not block this action. CanSkipStory is a base-page click guard,
    // absent from final confirmation and NPC/freedom click paths.
    // The wrapper preserves NPC overrides and native skip-break progression.
    reinterpret_cast<Action>(skip.methods[static_cast<std::size_t>(SkipMethod::Skip)])(page, 0);
}

// Game only. Each native reference is borrowed for this synchronous call, from
// the live world and manager getters. Nothing is retained across frames/teardown.
CabbirdStatusV1 ApplySpeed(Context& ctx, bool restore) {
    if (restore && !ctx.speed.applied) return Ok();
    if (!ctx.speed.validated) return Error(CABBIRD_STATUS_V1_UNAVAILABLE);
    const auto manager = SpeedFunction<ManagerFn>(ctx, SpeedMethod::Manager)(0);
    const auto section = manager ? SpeedFunction<SectionFn>(ctx, SpeedMethod::Section)(manager, 0) : 0;
    if (!section) {
        ctx.speed.state = SpeedState::Waiting;
        if (restore) ctx.speed.applied = false;
        return Ok();
    }
    bool destroyed{}, paused{}, audio_paused{}, initialized{};
    if (!ReadFlag(ctx, section, ctx.speed.destroyed_offset, destroyed) ||
        !ReadFlag(ctx, section, ctx.speed.paused_offset, paused) ||
        !ReadFlag(ctx, section, ctx.speed.audio_paused_offset, audio_paused) ||
        !ReadFlag(ctx, section, ctx.speed.initialized_offset, initialized)) return Error(CABBIRD_STATUS_V1_UNAVAILABLE);
    if (destroyed || !initialized) {
        if (restore) ctx.speed.applied = false;
        ctx.speed.state = SpeedState::Waiting;
        return Ok();
    }
    // During stop the host has already disabled every hook. During ordinary
    // disable the speed intent is false and SyncHook has released this hook.
    const auto read_base = reinterpret_cast<ReadSpeedFn>(ctx.bindings[static_cast<std::size_t>(Feature::Speed)].target);
    const float desired = restore ? read_base(manager, 0) : ctx.playback_speed.load(std::memory_order_relaxed);
    if (!std::isfinite(desired) || desired <= 0 || desired >= ctx.speed.native_maximum)
        return Error(CABBIRD_STATUS_V1_UNAVAILABLE);
    if (paused) {
        // PauseOnReview(false, ...) recomputes the native speed when resuming.
        // Its pauseAudio=false path keeps voice playback running, so restore
        // only audio here; resuming the paused timeline would be incorrect.
        if (restore) {
            if (!audio_paused) SpeedFunction<SetAudioSpeedFn>(ctx, SpeedMethod::SetAudioSpeed)(section, desired, 0);
            ctx.speed.applied = false;
        }
        ctx.speed.state = SpeedState::Paused;
        return Ok();
    }
    const auto current_speed = SpeedFunction<ReadSpeedFn>(ctx, SpeedMethod::CurrentSpeed);
    const float current = current_speed(section, 0);
    if (!std::isfinite(current)) return Error(CABBIRD_STATUS_V1_UNAVAILABLE);
    if (std::fabs(current - desired) > 0.0001f) {
        ctx.speed.applied = true;
        // PE callers pass a null hidden MethodInfo (R9), float in XMM1,
        // bool in R8B. The full native setter updates graph, tracks, audio/UI.
        SpeedFunction<SetSpeedFn>(ctx, SpeedMethod::SetSpeed)(section, desired, true, 0);
        const float applied = current_speed(section, 0);
        if (!std::isfinite(applied) || std::fabs(applied - desired) > 0.0001f)
            return Error(CABBIRD_STATUS_V1_UNAVAILABLE);
    } else if (restore) {
        // SetPlaySpeed returns early when the section value already matches.
        // Audio can still carry the old rate after pauseAudio=false transitions.
        SpeedFunction<SetAudioSpeedFn>(ctx, SpeedMethod::SetAudioSpeed)(section, desired, 0);
    }
    if (restore) ctx.speed.applied = false;
    ctx.speed.state = restore ? SpeedState::Idle : SpeedState::Playing;
    return Ok();
}

CabbirdStatusV1 CABBIRD_CALL RestoreOnGameStop(void* raw) noexcept {
    try {
        auto& ctx = *static_cast<Context*>(raw);
        ctx.running.store(false, std::memory_order_release);
        ctx.speed_requested.store(false, std::memory_order_release);
        const auto status = ApplySpeed(ctx, true);
        if (!Succeeded(status)) {
            ctx.speed.state = SpeedState::RestoreFailed;
            LogFailure(ctx, "The game's configured speed could not be restored during stop.");
        }
        return status;
    } catch (...) { return Error(CABBIRD_STATUS_V1_FAILED); }
}

class CallbackLease final {
public:
    CallbackLease(Context& ctx, const HookSlot& slot) noexcept : hooks_(ctx.hooks) {
        held_ = Succeeded(hooks_->begin_callback(hooks_->user, slot.handle, &lease_));
    }
    ~CallbackLease() { if (held_) static_cast<void>(hooks_->end_callback(hooks_->user, lease_)); }
    explicit operator bool() const noexcept { return held_; }
    CallbackLease(const CallbackLease&) = delete;
    CallbackLease& operator=(const CallbackLease&) = delete;
private:
    const CabbirdHookServiceV1* hooks_;
    CabbirdGenerationHandleV1 lease_{};
    bool held_{};
};

// The static getter receives just hidden MethodInfo in RCX. IsAutoPlay is an
// instance getter: self in RCX, hidden MethodInfo in RDX, bool result in AL.
// Forward the exact hidden argument from the game's call, never synthesize one.
using SkipPermissionFn = bool(CABBIRD_CALL*)(std::uintptr_t);
using AutoPlayFn = bool(CABBIRD_CALL*)(std::uintptr_t, std::uintptr_t);

bool CABBIRD_CALL SkipPermissionDetour(std::uintptr_t method_info) noexcept {
    try {
        auto* ctx = g_active.load(std::memory_order_acquire);
        if (!ctx) return false;
        auto& binding = ctx->bindings[static_cast<std::size_t>(Feature::Skip)];
        const CallbackLease lease(*ctx, binding.hook);
        if (!lease) return false;
        const auto original = reinterpret_cast<SkipPermissionFn>(binding.hook.original);
        const bool base = original(method_info);
        const bool requested = ctx->running.load(std::memory_order_acquire) &&
            ctx->unlock_requested.load(std::memory_order_acquire) &&
            ctx->game_thread.load(std::memory_order_relaxed) == GetCurrentThreadId();
        return base || requested;
    } catch (...) { return false; }
}

bool CABBIRD_CALL AutoPlayDetour(std::uintptr_t self, std::uintptr_t method_info) noexcept {
    try {
        auto* ctx = g_active.load(std::memory_order_acquire);
        if (!ctx) return false;
        auto& binding = ctx->bindings[static_cast<std::size_t>(Feature::Auto)];
        const CallbackLease lease(*ctx, binding.hook);
        if (!lease) return false;
        const auto original = reinterpret_cast<AutoPlayFn>(binding.hook.original);
        const bool base = original(self, method_info);
        const bool requested = ctx->running.load(std::memory_order_acquire) &&
            ctx->auto_requested.load(std::memory_order_acquire) &&
            ctx->game_thread.load(std::memory_order_relaxed) == GetCurrentThreadId();
        return base || requested;
    } catch (...) { return false; }
}

float CABBIRD_CALL StorySpeedDetour(std::uintptr_t self, std::uintptr_t method_info) noexcept {
    try {
        auto* ctx = g_active.load(std::memory_order_acquire);
        if (!ctx) return 1;
        auto& binding = ctx->bindings[static_cast<std::size_t>(Feature::Speed)];
        const CallbackLease lease(*ctx, binding.hook);
        if (!lease) return 1;
        const float base = reinterpret_cast<ReadSpeedFn>(binding.hook.original)(self, method_info);
        const bool requested = ctx->running.load(std::memory_order_acquire) &&
            ctx->speed_requested.load(std::memory_order_acquire) &&
            ctx->game_thread.load(std::memory_order_relaxed) == GetCurrentThreadId();
        if (requested) ctx->speed.applied = true;
        return requested ? ctx->playback_speed.load(std::memory_order_relaxed) : base;
    } catch (...) { return 1; }
}

CabbirdStatusV1 ReleaseHook(Context& ctx, Binding& binding) {
    if (!binding.hook.handle.id) return Ok();
    const auto status = ctx.hooks->release(ctx.hooks->user, binding.hook.handle);
    if (!Succeeded(status) && status.code != CABBIRD_STATUS_V1_NOT_FOUND) {
        binding.state = FeatureState::ReleaseFailed;
        return status; // Preserve original/handle until the host drains it.
    }
    binding.hook = {};
    return Ok();
}

void SyncHook(Context& ctx, std::size_t index, bool requested) {
    auto& binding = ctx.bindings[index];
    if (!requested) {
        if (Succeeded(ReleaseHook(ctx, binding))) binding.state = FeatureState::Disabled;
        binding.attempted = false;
        binding.retry_at = 0;
        return;
    }
    if (binding.hook.handle.id || binding.attempted || GetTickCount64() < binding.retry_at) return;
    binding.attempted = true;
    if (!binding.target || !binding.caller) { binding.state = FeatureState::ProfileUnavailable; return; }
    bool unavailable{};
    if (!ValidateMethod(ctx, kTargets[index], binding.target, &unavailable) ||
        !ValidateMethod(ctx, kCallers[index], binding.caller, &unavailable)) {
        binding.state = unavailable ? FeatureState::Waiting : FeatureState::MetadataUnavailable;
        if (unavailable) { binding.attempted = false; binding.retry_at = GetTickCount64() + 1000; }
        return;
    }
    if (index == static_cast<std::size_t>(Feature::Speed) && !ValidateSpeedRuntime(ctx, unavailable)) {
        binding.state = unavailable ? FeatureState::Waiting : FeatureState::MetadataUnavailable;
        ctx.speed.state = unavailable ? SpeedState::Waiting : SpeedState::Unavailable;
        if (unavailable) { binding.attempted = false; binding.retry_at = GetTickCount64() + 1000; }
        return;
    }
    // Recheck immediately before the host patches code, after metadata validation.
    if (!MatchesProfileEntry(ctx.core, ctx.profile, kTargets[index].symbol, binding.target)) {
        binding.state = FeatureState::ProfileUnavailable;
        return;
    }
    CabbirdHookRequestV1 request{};
    request.struct_size = sizeof(request);
    request.kind = CABBIRD_HOOK_V1_FUNCTION;
    request.target = binding.target;
    void* detours[]{reinterpret_cast<void*>(&SkipPermissionDetour), reinterpret_cast<void*>(&AutoPlayDetour),
        reinterpret_cast<void*>(&StorySpeedDetour)};
    constexpr const char* labels[]{"cabbird.story-tools.skip-permission", "cabbird.story-tools.auto-play", "cabbird.story-tools.speed"};
    request.detour = detours[index];
    request.label = StringView(labels[index]);
    const auto status = ctx.hooks->create(ctx.hooks->user, &request, &binding.hook.original, &binding.hook.handle);
    if (!Succeeded(status) || !binding.hook.original || !binding.hook.handle.id) {
        if (Succeeded(ReleaseHook(ctx, binding))) binding.state = FeatureState::HookRefused;
        return;
    }
    binding.state = FeatureState::Live;
}

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* raw, void** output) noexcept {
    if (!output) return Error(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    *output = nullptr;
    try {
        auto ctx = std::make_unique<Context>(raw);
        ctx->core = ctx->host.Query<CabbirdCoreServiceV1>(CABBIRD_CORE_SERVICE_V1_ID).get();
        ctx->hooks = ctx->host.Query<CabbirdHookServiceV1>(CABBIRD_HOOK_SERVICE_V1_ID).get();
        ctx->metadata = ctx->host.Query<CabbirdUnityMetadataServiceV1>(CABBIRD_UNITY_METADATA_SERVICE_V1_ID).get();
        ctx->windows = ctx->host.Query<CabbirdWindowServiceV1>(CABBIRD_WINDOW_SERVICE_V1_ID).get();
        ctx->config = ctx->host.Query<CabbirdConfigServiceV1>(CABBIRD_CONFIG_SERVICE_V1_ID).get();
        ctx->json = ctx->host.Query<CabbirdJsonServiceV1>(CABBIRD_JSON_SERVICE_V1_ID).get();
        ctx->game_lifecycle = ctx->host.Query<CabbirdGameLifecycleServiceV1>(CABBIRD_GAME_LIFECYCLE_SERVICE_V1_ID).get();
        const auto scheduler = ctx->host.Query<CabbirdSchedulerServiceV1>(CABBIRD_SCHEDULER_SERVICE_V1_ID);
        if (!ctx->core || ctx->core->struct_size < offsetof(CabbirdCoreServiceV1, module_base) + sizeof(ctx->core->module_base) ||
            !ctx->core->log || !ctx->core->read_memory || !ctx->core->module_base ||
            !ctx->hooks || ctx->hooks->struct_size < sizeof(*ctx->hooks) || !ctx->hooks->create ||
            !ctx->hooks->release || !ctx->hooks->begin_callback || !ctx->hooks->end_callback ||
            !ctx->metadata || ctx->metadata->struct_size < sizeof(*ctx->metadata) || !ctx->metadata->resolve_method ||
            !ctx->metadata->resolve_instance_field ||
            !ctx->windows || ctx->windows->struct_size < sizeof(*ctx->windows) || !ctx->windows->register_window ||
            !ctx->windows->release_window || !ctx->windows->begin || !ctx->windows->end ||
            !ctx->config || ctx->config->struct_size < sizeof(*ctx->config) || !ctx->config->register_schema ||
            !ctx->config->read || !ctx->config->write_atomic ||
            !ctx->json || ctx->json->struct_size < sizeof(*ctx->json) || !ctx->json->parse || !ctx->json->release ||
            !ctx->json->object_find || !ctx->json->boolean_value || !ctx->json->number_value ||
            !ctx->game_lifecycle || ctx->game_lifecycle->struct_size < sizeof(*ctx->game_lifecycle) ||
            !ctx->game_lifecycle->register_stop_cleanup || !ctx->game_lifecycle->unregister_stop_cleanup ||
            !scheduler || scheduler->struct_size < sizeof(*scheduler.get()) || !scheduler->schedule)
            return Error(CABBIRD_STATUS_V1_UNAVAILABLE);
        CabbirdGenerationHandleV1 schema{};
        const auto registered = ctx->config->register_schema(ctx->config->user, StringView(kConfigId), 1, Bytes(kSchema), &schema);
        if (!Succeeded(registered)) return registered;
        if (!RestoreSettings(*ctx)) LogFailure(*ctx, "Saved settings could not be restored; defaults retained.");
        ctx->saves.Bind(raw, ctx->config, kConfigId, 1);
        PrepareProfile(*ctx);
        PublishIntent(*ctx, ctx->settings);
        const auto cleanup = ctx->game_lifecycle->register_stop_cleanup(ctx->game_lifecycle->user,
            RestoreOnGameStop, ctx.get(), &ctx->cleanup);
        if (!Succeeded(cleanup)) return cleanup;
        *output = ctx.release();
        return Ok();
    } catch (...) { return Error(CABBIRD_STATUS_V1_FAILED); }
}

CabbirdStatusV1 CABBIRD_CALL Start(void* raw) noexcept {
    try {
        auto& ctx = *static_cast<Context*>(raw);
        CabbirdWindowSpecV1 spec{};
        spec.struct_size = sizeof(spec);
        spec.id = StringView("story-tools-main");
        const auto title = ctx.localizer.Text("window.title", "Story Tools");
        spec.title = StringView(title);
        spec.initial_width = 460;
        spec.initial_height = 320;
        spec.minimum_width = 340;
        spec.default_open = 1;
        const auto status = ctx.windows->register_window(ctx.windows->user, &spec, &ctx.window);
        if (!Succeeded(status)) return status;
        g_active.store(&ctx, std::memory_order_release);
        ctx.running.store(true, std::memory_order_release);
        return Ok();
    } catch (...) { return Error(CABBIRD_STATUS_V1_FAILED); }
}

CabbirdStatusV1 CABBIRD_CALL Stop(void* raw, std::uint32_t) noexcept {
    try {
        auto& ctx = *static_cast<Context*>(raw);
        ctx.running.store(false, std::memory_order_release);
        CabbirdStatusV1 result = Ok();
        for (auto& binding : ctx.bindings) {
            const auto status = ReleaseHook(ctx, binding);
            if (!Succeeded(status)) result = status;
        }
        ReportFailures(ctx);
        if (!ctx.saves.Flush() && Succeeded(result)) result = Error(CABBIRD_STATUS_V1_FAILED);
        if (ctx.cleanup.id) {
            const auto status = ctx.game_lifecycle->unregister_stop_cleanup(ctx.game_lifecycle->user, ctx.cleanup);
            if (Succeeded(status) || status.code == CABBIRD_STATUS_V1_NOT_FOUND) ctx.cleanup = {};
            else result = status;
        }
        if (ctx.window.id) {
            const auto status = ctx.windows->release_window(ctx.windows->user, ctx.window);
            if (Succeeded(status) || status.code == CABBIRD_STATUS_V1_NOT_FOUND) ctx.window = {};
            else result = status;
        }
        return result;
    } catch (...) { return Error(CABBIRD_STATUS_V1_FAILED); }
}

void CABBIRD_CALL Unload(void* raw) noexcept {
    try {
        g_active.store(nullptr, std::memory_order_release);
        delete static_cast<Context*>(raw);
    } catch (...) {}
}

void CABBIRD_CALL Update(void* raw, double) noexcept {
    try {
        auto& ctx = *static_cast<Context*>(raw);
        if (!ctx.running.load(std::memory_order_acquire)) return;
        ctx.game_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        SyncHook(ctx, static_cast<std::size_t>(Feature::Skip), ctx.unlock_requested.load(std::memory_order_acquire));
        SyncHook(ctx, static_cast<std::size_t>(Feature::Auto), ctx.auto_requested.load(std::memory_order_acquire));
        const bool speed_requested = ctx.speed_requested.load(std::memory_order_acquire);
        auto& speed_binding = ctx.bindings[static_cast<std::size_t>(Feature::Speed)];
        SyncHook(ctx, static_cast<std::size_t>(Feature::Speed), speed_requested);
        if ((speed_requested && speed_binding.state.load() == FeatureState::Live) ||
            (!speed_requested && !speed_binding.hook.handle.id && ctx.speed.applied)) {
            const auto status = ApplySpeed(ctx, !speed_requested);
            if (!Succeeded(status)) ctx.speed.state = speed_requested ? SpeedState::Unavailable : SpeedState::RestoreFailed;
        }
        SyncAutomaticSkip(ctx, ctx.skip_requested.load(std::memory_order_acquire));
        ReportFailures(ctx);
    } catch (...) {}
}

void DrawContents(Context& ctx, const CabbirdUiServiceV1* ui) {
    Settings settings;
    { std::scoped_lock lock(ctx.settings_mutex); settings = ctx.settings; }
    bool changed{};
    int enabled = settings.enabled;
    if (ui->checkbox(ui->user, StringView(ctx.localizer.Text("settings.enabled", "Enable story tools")), &enabled)) {
        settings.enabled = enabled != 0;
        changed = true;
    }
    int unlock = settings.unlock_skip;
    if (ui->checkbox(ui->user, StringView(ctx.localizer.Text("settings.unlock", "Unlock the native skip button")), &unlock)) {
        settings.unlock_skip = unlock != 0;
        changed = true;
    }
    int auto_play = settings.auto_play;
    if (ui->checkbox(ui->user, StringView(ctx.localizer.Text("settings.auto", "Use the game's automatic dialogue playback")), &auto_play)) {
        settings.auto_play = auto_play != 0;
        changed = true;
    }
    int speed_enabled = settings.speed_enabled;
    int auto_skip = settings.auto_skip;
    if (ui->checkbox(ui->user, StringView(ctx.localizer.Text("settings.auto-skip", "Automatically skip stories")), &auto_skip)) {
        settings.auto_skip = auto_skip != 0;
        changed = true;
    }
    if (ui->checkbox(ui->user, StringView(ctx.localizer.Text("settings.speed-enabled", "Override story playback speed")), &speed_enabled)) {
        settings.speed_enabled = speed_enabled != 0;
        changed = true;
    }
    if (ui->slider_float(ui->user, StringView(ctx.localizer.Text("settings.speed", "Story playback speed (x)")),
            &settings.playback_speed, kMinimumSpeed, kMaximumSpeed)) {
        settings.playback_speed = std::clamp(settings.playback_speed, kMinimumSpeed, kMaximumSpeed);
        changed = true;
    }
    if (changed) {
        { std::scoped_lock lock(ctx.settings_mutex); ctx.settings = settings; }
        PublishIntent(ctx, settings);
        QueueSettings(ctx, settings);
    }

}

void CABBIRD_CALL Draw(void* raw, const CabbirdUiServiceV1* ui) noexcept {
    try {
        auto& ctx = *static_cast<Context*>(raw);
        if (!ui || ui->struct_size < offsetof(CabbirdUiServiceV1, separator) + sizeof(ui->separator) ||
            ui->service_version < CABBIRD_UI_SERVICE_V1_VERSION ||
            !ui->checkbox || !ui->slider_float || !ctx.window.id) return;
        std::int32_t visible{};
        if (!Succeeded(ctx.windows->begin(ctx.windows->user, ctx.window, 0, &visible))) return;
        struct EndWindow final {
            Context& ctx;
            ~EndWindow() { static_cast<void>(ctx.windows->end(ctx.windows->user, ctx.window)); }
        } end{ctx};
        if (visible) DrawContents(ctx, ui);
    } catch (...) {}
}
} // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(CabbirdPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) return Error(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    *descriptor = {sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        StringView(kId), StringView("Story Tools"), StringView("Cabbird"), StringView(kVersion),
        Load, Start, Stop, Unload, Update, Draw};
    return Ok();
}

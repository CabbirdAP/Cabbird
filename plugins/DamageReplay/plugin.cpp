#include "cabbird/sdk/il2cpp.hpp"
#include "cabbird/sdk/localization.hpp"
#include "cabbird/sdk/profile.hpp"
#include "cabbird/sdk/services/unity_metadata.h"
#include "cabbird/sdk/config_save_queue.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {
using cabbird::sdk::Host;
using cabbird::sdk::Ok;
using cabbird::sdk::StringView;
constexpr std::uint32_t kDefaultCount = 10, kMaximumCount = 1024;
constexpr const char* kSettingsId = "damage-replay-settings";
constexpr const char* kSettingsSchema = R"({"type":"object","additionalProperties":false,"required":["enabled","replayCount"],"properties":{"enabled":{"type":"boolean"},"replayCount":{"type":"integer","minimum":1,"maximum":1024}}})";
// These are metadata identities, not version-specific addresses or layouts.
constexpr const char* kAssembly = "Assembly-CSharp.dll";
constexpr const char* kNamespace = "Lens.Gameplay.Modules.BigWorld";
constexpr const char* kOwnerType = "Lens.Gameplay.Modules.BigWorld.AliveElementSystem";
constexpr const char* kElementType = "Lens.Gameplay.Modules.BigWorld.BaseElement";
constexpr const char* kParameterType = "Lens.Gameplay.Modules.BigWorld.DamageElementBase";
constexpr const char* kMethodName = "OnExecuteDamageElement";
constexpr const char* kDamageType = "Lens.Gameplay.Modules.BigWorld.DamageElement";

const CabbirdCoreServiceV1* g_core{};
const CabbirdUiServiceV1* g_ui{};
const CabbirdHookServiceV1* g_hook{};
const CabbirdConfigServiceV1* g_config{};
const CabbirdJsonServiceV1* g_json{};
const CabbirdIl2CppServiceV1* g_il2cpp{};
const CabbirdUnityMetadataServiceV1* g_metadata{};
const CabbirdUnityPlayerServiceV1* g_player{};
cabbird::sdk::Localizer g_text;
cabbird::sdk::ConfigSaveQueue g_save;

constexpr std::uint64_t Settings(bool enabled, std::uint32_t count) noexcept {
    return count | (static_cast<std::uint64_t>(enabled) << 32U);
}
bool Enabled(std::uint64_t value) noexcept { return (value >> 32U) != 0; }
std::uint32_t Count(std::uint64_t value) noexcept { return static_cast<std::uint32_t>(value); }
std::atomic<std::uint64_t> g_settings{Settings(false, kDefaultCount)};
std::atomic_bool g_running{};
std::atomic<int> g_intent{};
std::mutex g_status_mutex;
std::string g_status;
std::uint64_t g_reported_refusals{}; // Game only, once per category and phase/enable cycle.
std::chrono::steady_clock::time_point g_next_bind{};

struct Layout {
    std::uint32_t source_player{}, source_entity{}, attacker_entity{}, free{}, unique_id{}, element_version{};
    std::uint32_t receiver_entity{}, receiver_version{};
    std::uint32_t controlled_entity{}, controlled_player{}, controlled_index{}, controlled_version{};
    std::uint32_t object_class{}, pool_index{}, collision{}, critical{}, cache_enabled{}, cache_position{}, cache_forward{}, performance{};
};
struct Vector3 { float x{}, y{}, z{}; };
struct CollisionInfo { alignas(8) std::array<std::uint8_t, 48> bytes{}; };
static_assert(sizeof(Vector3) == 12 && sizeof(CollisionInfo) == 48);
enum Native : std::size_t { Rent, Free, Copy, Collision, Critical, Cache, Performance, NativeCount };
struct NativeMethod {
    const char* assembly; const char* namespaze; const char* type; const char* name; const char* result;
    std::uint32_t flags; std::array<const char*, 2> parameters; std::size_t count;
};
const std::array<NativeMethod, NativeCount> kNativeMethods{{
    {"AzurFramework.dll", "Lens.Framework", "ClassManager", "Get", "Lens.Framework.IClass", CABBIRD_UNITY_METHOD_V1_STATIC, {"System.Type"}, 1},
    {"AzurFramework.dll", "Lens.Framework", "ClassManager", "Free", "System.Void", CABBIRD_UNITY_METHOD_V1_STATIC, {"Lens.Framework.IClass"}, 1},
    {kAssembly, kNamespace, "BaseElement", "CopyAllDataTo", "System.Void", CABBIRD_UNITY_METHOD_V1_INSTANCE, {"Lens.Gameplay.Modules.BigWorld.IElement&"}, 1},
    {kAssembly, kNamespace, "DamageElementBase", "SetCurrentCollisionInfo", "System.Void", CABBIRD_UNITY_METHOD_V1_INSTANCE, {"Lens.Gameplay.Modules.BigWorld.CollisionInfo"}, 1},
    {kAssembly, kNamespace, "DamageElementBase", "set_criticalRandom", "System.Void", CABBIRD_UNITY_METHOD_V1_INSTANCE, {"System.Int32"}, 1},
    {kAssembly, kNamespace, "DamageElement", "SetAttackerCacheInfo", "System.Void", CABBIRD_UNITY_METHOD_V1_INSTANCE, {"UnityEngine.Vector3", "UnityEngine.Vector3"}, 2},
    {kAssembly, kNamespace, "DamageElement", "SetPerformanceCollisionInfo", "System.Void", CABBIRD_UNITY_METHOD_V1_INSTANCE, {"System.Collections.Generic.List<Lens.Gameplay.Modules.BigWorld.CollisionInfo>"}, 1}
}};
struct Compatibility {
    cabbird::sdk::Profile profile;
    Layout layout;
    std::uintptr_t address{};
    std::array<std::uintptr_t, NativeCount> native{};
    bool ready{};
} g_compatibility;
// Game-owned after with_runtime resolves it. Metadata identity is runtime-owned;
// it is compared with the hidden argument, never dereferenced or invoked here.
const CabbirdMethodInfoV1* g_method_identity{};
std::array<const CabbirdMethodInfoV1*, NativeCount> g_native_identity{};
CabbirdIl2CppClassV1* g_damage_class{};
CabbirdIl2CppObjectV1* g_damage_runtime_type{};
CabbirdIl2CppGCHandleV1 g_type_handle{};
std::atomic<DWORD> g_game_thread{};
bool g_applying{}; // Only the admitted Game thread reads/writes this flag.
struct HookSlot {
    std::uintptr_t original{};
    CabbirdGenerationHandleV1 handle{};
} g_hook_slot;

void Publish(const char* key, const char* fallback, std::string_view detail = {}) {
    const std::string_view values[]{detail};
    auto text = g_text.Format(key, fallback, values);
    std::scoped_lock lock(g_status_mutex);
    if (g_status == text) return;
    g_status = std::move(text);
    if (g_core && g_core->log) g_core->log(g_core->user, CABBIRD_CORE_LOG_LEVEL_V1_WARNING, StringView(g_status));
}
std::string StatusDetail(CabbirdStatusV1 status) {
    const auto code = std::to_string(status.code);
    const std::string message = status.message.data
        ? std::string(status.message.data, status.message.size) : std::string{};
    const std::string_view values[]{code, message};
    return g_text.Format("status.detail", "code {0}: {1}", values);
}
CabbirdByteSpanV1 Bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}
struct JsonNode {
    CabbirdGenerationHandleV1 handle{};
    ~JsonNode() { if (handle.id) static_cast<void>(g_json->release(g_json->user, handle)); }
};

// Lifecycle only. The registered host schema validates the persisted document;
// parse checks also keep this boundary valid for an external host consumer.
void RestoreSettings() {
    std::size_t size{};
    std::uint32_t version{};
    auto status = g_config->read(g_config->user, StringView(kSettingsId), &version, {}, &size);
    if (status.code == CABBIRD_STATUS_V1_NOT_FOUND) return;
    if ((status.code != CABBIRD_STATUS_V1_OK && status.code != CABBIRD_STATUS_V1_BUFFER_TOO_SMALL) ||
        version != 1 || size == 0 || size > 4096)
        throw std::runtime_error(StatusDetail(status));
    std::vector<std::uint8_t> bytes(size);
    status = g_config->read(g_config->user, StringView(kSettingsId), &version,
        {bytes.data(), bytes.size()}, &size);
    if (status.code != CABBIRD_STATUS_V1_OK || version != 1 || size > bytes.size())
        throw std::runtime_error(StatusDetail(status));
    JsonNode document, enabled, count;
    status = g_json->parse(g_json->user, {reinterpret_cast<const char*>(bytes.data()), size}, &document.handle);
    if (status.code != CABBIRD_STATUS_V1_OK) throw std::runtime_error(StatusDetail(status));
    std::int32_t enabled_value{};
    double count_value{};
    if (g_json->object_find(g_json->user, document.handle, StringView("enabled"), &enabled.handle).code != CABBIRD_STATUS_V1_OK ||
        g_json->object_find(g_json->user, document.handle, StringView("replayCount"), &count.handle).code != CABBIRD_STATUS_V1_OK ||
        g_json->boolean_value(g_json->user, enabled.handle, &enabled_value).code != CABBIRD_STATUS_V1_OK ||
        g_json->number_value(g_json->user, count.handle, &count_value).code != CABBIRD_STATUS_V1_OK ||
        !std::isfinite(count_value) || std::floor(count_value) != count_value || count_value < 1 || count_value > kMaximumCount)
        throw std::runtime_error("enabled/replayCount failed the registered settings contract");
    g_settings.store(Settings(enabled_value != 0, static_cast<std::uint32_t>(count_value)));
}
void Persist(std::uint64_t settings) {
    const auto document = std::string("{\"enabled\":") + (Enabled(settings) ? "true" : "false") +
        ",\"replayCount\":" + std::to_string(Count(settings)) + "}";
    if (!g_save.Queue(document)) Publish("settings.queue-failed", "Settings could not be queued for saving.");
}

// Lifecycle/Worker only: host-owned package parsing and RVA/entry validation.
void PrepareCompatibility(const Host& services) {
    auto loaded = cabbird::sdk::LoadProfile(services, "compatibility.profile.json",
        {"ap", "GameAssembly.dll", ".text", "damage-replay-v4"});
    if (!loaded) throw std::runtime_error(loaded.error());
    auto profile = std::move(loaded).value();
    // The native call ABI is fixed by CollisionInfo; compatibility data must
    // explicitly agree before this representation is read or passed to the game.
    if (profile.Value("collisionInfoSize", sizeof(CollisionInfo), alignof(CollisionInfo)) != sizeof(CollisionInfo))
        throw std::runtime_error("collisionInfoSize does not match the supported native ABI");
    const auto resolved = cabbird::sdk::ResolveProfileSymbolRva(g_core, profile, "damageEntry");
    if (!resolved) throw std::runtime_error(resolved.error);
    Layout layout;
    layout.source_player = profile.Value("sourcePlayerEntityIdOffset", 4096, alignof(std::int32_t));
    layout.source_entity = profile.Value("sourceEntityIdOffset", 4096, alignof(std::int32_t));
    layout.attacker_entity = profile.Value("attackerEntityIdOffset", 4096, alignof(std::int32_t));
    layout.free = profile.Value("elementFreeOffset", 4096);
    layout.unique_id = profile.Value("elementUniqueIdOffset", 4096, alignof(std::uint64_t));
    layout.element_version = profile.Value("elementVersionOffset", 4096, alignof(std::int32_t));
    layout.receiver_entity = profile.Value("receiverEntityIdOffset", 4096, alignof(std::int32_t));
    layout.receiver_version = profile.Value("receiverVersionOffset", 4096, alignof(std::int32_t));
    layout.controlled_entity = profile.Value("controlledEntityIdOffset", 4096, alignof(std::int32_t));
    layout.controlled_player = profile.Value("controlledPlayerIdOffset", 4096, alignof(std::int32_t));
    layout.controlled_index = profile.Value("controlledObjectIndexOffset", 4096, alignof(std::uint32_t));
    layout.controlled_version = profile.Value("controlledVersionOffset", 4096, alignof(std::int32_t));
    layout.object_class = profile.Value("objectClassOffset", 4096, alignof(std::uintptr_t));
    layout.pool_index = profile.Value("elementPoolIndexOffset", 4096, alignof(std::uint32_t));
    layout.collision = profile.Value("collisionInfoOffset", 4096, alignof(std::uintptr_t));
    layout.critical = profile.Value("criticalRandomOffset", 4096, alignof(std::int32_t));
    layout.cache_enabled = profile.Value("attackerCacheEnabledOffset", 4096);
    layout.cache_position = profile.Value("attackerPositionOffset", 4096, alignof(float));
    layout.cache_forward = profile.Value("attackerForwardOffset", 4096, alignof(float));
    layout.performance = profile.Value("performanceCollisionsOffset", 4096, alignof(std::uintptr_t));
    std::array<std::uintptr_t, NativeCount> native{};
    const char* symbols[]{"rent", "free", "copy", "collision", "critical", "cache", "performance"};
    for (std::size_t i{}; i < NativeCount; ++i) {
        const auto entry = cabbird::sdk::ResolveProfileSymbolRva(g_core, profile, symbols[i]);
        if (!entry) throw std::runtime_error(std::string(symbols[i]) + ": " + entry.error);
        native[i] = entry.address;
    }
    g_compatibility = {std::move(profile), layout, resolved.address, native, true};
}

struct IdentityResult {
    const CabbirdMethodInfoV1* method{};
    std::array<const CabbirdMethodInfoV1*, NativeCount> native{};
    CabbirdIl2CppClassV1* damage_class{};
    CabbirdIl2CppObjectV1* runtime_type{};
    CabbirdIl2CppGCHandleV1 handle{};
};
CabbirdStatusV1 Error(std::uint32_t code, std::string_view message) noexcept {
    return {code, 0, {message.data(), message.size()}};
}
CabbirdStatusV1 CABBIRD_CALL ResolveIdentity(void* user, const CabbirdIl2CppApiV1* api) {
    try {
        if (!api || api->struct_size < sizeof(*api) || api->api_version != 1 || !api->il2cpp_domain_get ||
            !api->il2cpp_domain_get_assemblies || !api->il2cpp_assembly_get_image ||
            !api->il2cpp_image_get_name || !api->il2cpp_class_from_name || !api->il2cpp_class_get_methods ||
            !api->il2cpp_method_get_name || !api->il2cpp_method_get_param_count ||
            !api->il2cpp_method_get_param || !api->il2cpp_type_get_name || !api->il2cpp_free ||
            !api->il2cpp_class_get_type || !api->il2cpp_type_get_object || !api->il2cpp_gchandle_new)
            return Error(CABBIRD_STATUS_V1_UNAVAILABLE, "IL2CPP identity query API is incomplete");
        auto* domain = api->il2cpp_domain_get();
        if (!domain) return Error(CABBIRD_STATUS_V1_UNAVAILABLE, "IL2CPP domain is not ready");
        std::size_t count{};
        const auto* assemblies = api->il2cpp_domain_get_assemblies(domain, &count);
        if (!assemblies) return Error(CABBIRD_STATUS_V1_UNAVAILABLE, "IL2CPP assembly enumeration is unavailable");
        CabbirdIl2CppImageV1* image{};
        for (std::size_t index{}; index < count; ++index) {
            if (!assemblies[index]) continue;
            auto* candidate = api->il2cpp_assembly_get_image(assemblies[index]);
            const auto* name = candidate ? api->il2cpp_image_get_name(candidate) : nullptr;
            if (!name || std::strcmp(name, kAssembly) != 0) continue;
            if (image && image != candidate)
                return Error(CABBIRD_STATUS_V1_CONFLICT, "Assembly-CSharp.dll image identity is ambiguous");
            image = candidate;
        }
        if (!image) return Error(CABBIRD_STATUS_V1_NOT_FOUND, "Assembly-CSharp.dll image was not found");
        auto* klass = api->il2cpp_class_from_name(image, kNamespace, "AliveElementSystem");
        if (!klass) return Error(CABBIRD_STATUS_V1_NOT_FOUND,
            "Assembly-CSharp.dll::Lens.Gameplay.Modules.BigWorld.AliveElementSystem was not found");
        const cabbird::sdk::il2cpp::Context runtime(*api);
        const CabbirdMethodInfoV1* selected{};
        void* iterator{};
        while (const auto* method = api->il2cpp_class_get_methods(klass, &iterator)) {
            const auto* name = api->il2cpp_method_get_name(method);
            if (!name || std::strcmp(name, kMethodName) != 0 ||
                api->il2cpp_method_get_param_count(method) != 1 ||
                runtime.TypeName(api->il2cpp_method_get_param(method, 0)) != kParameterType) continue;
            if (selected && selected != method)
                return Error(CABBIRD_STATUS_V1_CONFLICT, "AliveElementSystem.OnExecuteDamageElement method identity is ambiguous");
            selected = method;
        }
        if (!selected) return Error(CABBIRD_STATUS_V1_NOT_FOUND,
            "AliveElementSystem.OnExecuteDamageElement(DamageElementBase) identity was not found");
        auto& result = *static_cast<IdentityResult*>(user);
        result.method = selected;
        for (std::size_t i{}; i < NativeCount; ++i) {
            const auto& spec = kNativeMethods[i];
            auto* owner = runtime.FindClass(spec.assembly, spec.namespaze, spec.type);
            if (!owner) return Error(CABBIRD_STATUS_V1_NOT_FOUND, "fresh replay declaring type was not found in its assembly");
            const CabbirdMethodInfoV1* found{};
            void* it{};
            while (const auto* method = api->il2cpp_class_get_methods(owner, &it)) {
                const auto* name = api->il2cpp_method_get_name(method);
                if (!name || std::strcmp(name, spec.name) || api->il2cpp_method_get_param_count(method) != spec.count) continue;
                bool matches = true;
                for (std::size_t p{}; p < spec.count; ++p)
                    matches = matches && runtime.TypeName(api->il2cpp_method_get_param(method, static_cast<std::uint32_t>(p))) == spec.parameters[p];
                if (!matches) continue;
                if (found && found != method) return Error(CABBIRD_STATUS_V1_CONFLICT, "fresh replay method identity is ambiguous");
                found = method;
            }
            if (!found) return Error(CABBIRD_STATUS_V1_NOT_FOUND, "fresh replay method identity was not found");
            result.native[i] = found;
        }
        result.damage_class = api->il2cpp_class_from_name(image, kNamespace, "DamageElement");
        if (!result.damage_class) return Error(CABBIRD_STATUS_V1_NOT_FOUND, "DamageElement type was not found");
        if (g_type_handle) {
            result.runtime_type = g_damage_runtime_type;
            result.handle = g_type_handle;
            return Ok();
        }
        const auto* type = api->il2cpp_class_get_type(result.damage_class);
        result.runtime_type = type ? api->il2cpp_type_get_object(type) : nullptr;
        if (!result.runtime_type) return Error(CABBIRD_STATUS_V1_UNAVAILABLE, "DamageElement runtime type is not ready");
        result.handle = api->il2cpp_gchandle_new(result.runtime_type, true);
        if (!result.handle) return Error(CABBIRD_STATUS_V1_UNAVAILABLE, "DamageElement runtime type could not be retained");
        return Ok();
    } catch (...) { return Error(CABBIRD_STATUS_V1_FAILED, "IL2CPP method identity query failed"); }
}

bool ValidateLiveMetadata() {
    const CabbirdStringViewV1 parameters[]{StringView(kParameterType)};
    const CabbirdUnityMethodRequestV1 request{sizeof(request), CABBIRD_UNITY_METHOD_V1_INSTANCE,
        StringView(g_compatibility.profile.module), StringView(kOwnerType), StringView(kMethodName),
        StringView("System.Void"), parameters, 1};
    CabbirdUnityMethodResultV1 method{sizeof(method)};
    auto status = g_metadata->resolve_method(g_metadata->user, &request, &method);
    if (status.code != CABBIRD_STATUS_V1_OK) {
        Publish("status.method", "Damage method shape unavailable: {0}", StatusDetail(status));
        return false;
    }
    for (std::size_t i{}; i < NativeCount; ++i) {
        const auto& spec = kNativeMethods[i];
        const auto owner = std::string(spec.namespaze) + "." + spec.type;
        const CabbirdStringViewV1 args[]{StringView(spec.parameters[0]), spec.count == 2 ? StringView(spec.parameters[1]) : CabbirdStringViewV1{}};
        const CabbirdUnityMethodRequestV1 native_request{sizeof(native_request), spec.flags,
            StringView(g_compatibility.profile.module), StringView(owner), StringView(spec.name),
            StringView(spec.result), args, spec.count};
        CabbirdUnityMethodResultV1 native_result{sizeof(native_result)};
        status = g_metadata->resolve_method(g_metadata->user, &native_request, &native_result);
        if (status.code != CABBIRD_STATUS_V1_OK || native_result.address != g_compatibility.native[i]) {
            Publish("status.native", "Fresh damage replay method unavailable: {0}", owner + "." + spec.name + "; " + StatusDetail(status));
            return false;
        }
    }
    if (method.address != g_compatibility.address) {
        Publish("status.entry", "Damage method entry disagrees with the validated profile.");
        return false;
    }
    struct Field { const char* owner; const char* name; const char* type; std::uint32_t size, offset; };
    const auto& layout = g_compatibility.layout;
    const Field fields[]{
        {kElementType, "p_sourcePlayerEntityID", "System.Int32", sizeof(std::int32_t), layout.source_player},
        {kElementType, "p_sourceEntityID", "System.Int32", sizeof(std::int32_t), layout.source_entity},
        {kElementType, "p_attackerEntityID", "System.Int32", sizeof(std::int32_t), layout.attacker_entity},
        {kElementType, "m_free", "System.Boolean", sizeof(std::uint8_t), layout.free},
        {kElementType, "m_uniqueId", "System.UInt64", sizeof(std::uint64_t), layout.unique_id},
        {kElementType, "<version>k__BackingField", "System.Int32", sizeof(std::int32_t), layout.element_version},
        {kOwnerType, "m_entityId", "System.Int32", sizeof(std::int32_t), layout.receiver_entity},
        {kOwnerType, "<version>k__BackingField", "System.Int32", sizeof(std::int32_t), layout.receiver_version},
        {"Lens.Gameplay.Modules.BigWorld.BaseData", "<entityId>k__BackingField", "System.Int32", sizeof(std::int32_t), layout.controlled_entity},
        {"Lens.Gameplay.Modules.BigWorld.BaseData", "<playerId>k__BackingField", "System.Int32", sizeof(std::int32_t), layout.controlled_player},
        {"Lens.Gameplay.Modules.BigWorld.BaseData", "<objIndex>k__BackingField", "System.UInt32", sizeof(std::uint32_t), layout.controlled_index},
        {"Lens.Gameplay.Modules.BigWorld.BaseData", "<version>k__BackingField", "System.Int32", sizeof(std::int32_t), layout.controlled_version},
        {kElementType, "<objIndex>k__BackingField", "System.UInt32", sizeof(std::uint32_t), layout.pool_index},
        {kParameterType, "<collisionInfo>k__BackingField", "Lens.Gameplay.Modules.BigWorld.CollisionInfo", sizeof(CollisionInfo), layout.collision},
        {kParameterType, "<criticalRandom>k__BackingField", "System.Int32", sizeof(std::int32_t), layout.critical},
        {kParameterType, "useAttackerCacheInfo", "System.Boolean", sizeof(std::uint8_t), layout.cache_enabled},
        {kParameterType, "m_attackerPosition", "UnityEngine.Vector3", sizeof(Vector3), layout.cache_position},
        {kParameterType, "m_attackerForward", "UnityEngine.Vector3", sizeof(Vector3), layout.cache_forward},
        {kParameterType, "performanceCollisionInfos", "System.Collections.Generic.List<Lens.Gameplay.Modules.BigWorld.CollisionInfo>", sizeof(std::uintptr_t), layout.performance}};
    for (const auto& field : fields) {
        const CabbirdUnityFieldRequestV1 field_request{sizeof(field_request), field.size,
            StringView(field.owner), StringView(field.name), StringView(field.type)};
        CabbirdUnityFieldResultV1 result{sizeof(result)};
        status = g_metadata->resolve_instance_field(g_metadata->user, &field_request, &result);
        if (status.code != CABBIRD_STATUS_V1_OK || result.offset != field.offset) {
            const auto detail = std::string(field.owner) + "." + field.name + "; " + StatusDetail(status);
            Publish("status.field", "Damage identity field disagrees with the profile: {0}", detail);
            return false;
        }
    }
    IdentityResult identity;
    status = g_il2cpp->with_runtime(g_il2cpp->user, ResolveIdentity, &identity);
    if (status.code != CABBIRD_STATUS_V1_OK) {
        Publish("status.identity", "Damage method identity unavailable: {0}", StatusDetail(status));
        return false;
    }
    g_method_identity = identity.method;
    g_native_identity = identity.native;
    g_damage_class = identity.damage_class;
    g_damage_runtime_type = identity.runtime_type;
    g_type_handle = identity.handle;
    g_game_thread.store(GetCurrentThreadId());
    return true;
}

class CallbackLease {
public:
    CallbackLease() noexcept {
        held_ = g_hook->begin_callback(g_hook->user, g_hook_slot.handle, &lease_).code == CABBIRD_STATUS_V1_OK;
    }
    ~CallbackLease() { if (held_) static_cast<void>(g_hook->end_callback(g_hook->user, lease_)); }
    explicit operator bool() const noexcept { return held_; }
private:
    CabbirdGenerationHandleV1 lease_{};
    bool held_{};
};
template<class T> bool Read(std::uintptr_t object, std::uint32_t offset, T& value) noexcept {
    return object && g_core->read_memory(g_core->user, object + offset,
        {reinterpret_cast<std::uint8_t*>(&value), sizeof(value)}).code == CABBIRD_STATUS_V1_OK;
}
struct Controlled {
    std::uint64_t generation{}, data{};
    std::uint32_t index{};
    std::int32_t version{}, entity{}, player{};
    bool operator==(const Controlled&) const = default;
};
bool ControlledNow(Controlled& controlled) noexcept {
    CabbirdUnityPlayerSnapshotV1 snapshot{sizeof(snapshot)};
    if (g_player->snapshot(g_player->user, &snapshot).code != CABBIRD_STATUS_V1_OK ||
        snapshot.struct_size < sizeof(snapshot) || !(snapshot.flags & CABBIRD_UNITY_PLAYER_V1_VALID) ||
        (snapshot.flags & CABBIRD_UNITY_PLAYER_V1_IDENTITY_PROXIMITY) || !snapshot.entity_id) return false;
    const auto& layout = g_compatibility.layout;
    controlled.generation = snapshot.generation;
    controlled.data = snapshot.entity_id;
    return Read(controlled.data, layout.controlled_entity, controlled.entity) && controlled.entity != 0 &&
        Read(controlled.data, layout.controlled_player, controlled.player) &&
        Read(controlled.data, layout.controlled_index, controlled.index) &&
        Read(controlled.data, layout.controlled_version, controlled.version);
}
struct Lifetime {
    std::uint64_t unique_id{};
    std::uint32_t pool_index{};
    std::int32_t element_version{}, receiver_entity{}, receiver_version{};
    bool operator==(const Lifetime&) const = default;
};
struct Source { std::int32_t entity{}, player{}, attacker{}; };
enum class Refusal : unsigned { SourceRead, ForeignSource, ElementRead, ElementFree, ReceiverRead, SelfTarget, Changed, PlayerChanged, CallbackFailed,
    Unsupported, PayloadRead, RentFailed, CopyFailed };
void Refuse(Refusal reason, const Source& source, const Lifetime& lifetime, const Controlled& controlled,
            const char* phase, std::uint32_t repeat) {
    const auto stage_index = std::strcmp(phase, "before_original") == 0 ? 0U :
        std::strcmp(phase, "before_repeat") == 0 ? 1U : std::strcmp(phase, "original") == 0 ? 2U : 3U;
    const auto bit = std::uint64_t{1} << (4U * static_cast<unsigned>(reason) + stage_index);
    if (g_reported_refusals & bit) return;
    g_reported_refusals |= bit;
    const char* key{}; const char* fallback{};
    switch (reason) {
    case Refusal::SourceRead: key = "refusal.source-read"; fallback = "source fields could not be read"; break;
    case Refusal::ForeignSource: key = "refusal.foreign"; fallback = "source does not belong to the controlled player"; break;
    case Refusal::ElementRead: key = "refusal.element-read"; fallback = "element lifecycle fields could not be read"; break;
    case Refusal::ElementFree: key = "refusal.element-free"; fallback = "the damage element has finished execution"; break;
    case Refusal::ReceiverRead: key = "refusal.receiver"; fallback = "receiver identity could not be read"; break;
    case Refusal::SelfTarget: key = "refusal.self"; fallback = "the receiver is the controlled player"; break;
    case Refusal::Changed: key = "refusal.changed"; fallback = "the element or receiver lifecycle changed"; break;
    case Refusal::PlayerChanged: key = "refusal.player-changed"; fallback = "controlled player identity changed during the call"; break;
    case Refusal::CallbackFailed: key = "refusal.callback"; fallback = "the damage callback failed"; break;
    case Refusal::Unsupported: key = "refusal.unsupported"; fallback = "this damage element type has no validated replay contract"; break;
    case Refusal::PayloadRead: key = "refusal.payload"; fallback = "the current hit payload could not be read"; break;
    case Refusal::RentFailed: key = "refusal.rent"; fallback = "the game could not rent a fresh damage element"; break;
    case Refusal::CopyFailed: key = "refusal.copy"; fallback = "the game copy did not preserve a fresh active element identity"; break;
    }
    const auto explanation = g_text.Text(key, fallback);
    const std::array<std::string, 6> numbers{std::to_string(source.entity), std::to_string(source.player),
        std::to_string(source.attacker), std::to_string(lifetime.receiver_entity),
        std::to_string(controlled.entity), std::to_string(controlled.player)};
    // Phase identifiers and the loop index are diagnostic data.
    const auto stage = std::string("phase=") + phase + "; repeatIndex=" + std::to_string(repeat);
    const std::string_view values[]{explanation, numbers[0], numbers[1], numbers[2], numbers[3], numbers[4], numbers[5], stage};
    const auto message = g_text.Format("diagnostic.refused",
        "Damage multiplier refused: {0}; source={1}, sourcePlayer={2}, attacker={3}, receiver={4}, controlled={5}, controlledPlayer={6}; {7}", values);
    const auto level = reason == Refusal::ForeignSource || reason == Refusal::SelfTarget || reason == Refusal::PlayerChanged || reason == Refusal::Unsupported ?
        CABBIRD_CORE_LOG_LEVEL_V1_TRACE : CABBIRD_CORE_LOG_LEVEL_V1_WARNING;
    g_core->log(g_core->user, level, StringView(message));
}
bool Snapshot(std::uintptr_t receiver, std::uintptr_t element, Lifetime& lifetime, Refusal& reason) noexcept {
    const auto& layout = g_compatibility.layout;
    std::uint8_t free{};
    reason = Refusal::ElementRead;
    if (!Read(element, layout.free, free)) return false;
    if (free) { reason = Refusal::ElementFree; return false; }
    if (!Read(element, layout.unique_id, lifetime.unique_id) ||
        !Read(element, layout.pool_index, lifetime.pool_index) || !lifetime.pool_index ||
        !Read(element, layout.element_version, lifetime.element_version)) return false;
    reason = Refusal::ReceiverRead;
    return Read(receiver, layout.receiver_entity, lifetime.receiver_entity) && lifetime.receiver_entity != 0 &&
        Read(receiver, layout.receiver_version, lifetime.receiver_version);
}
bool Owned(const Source& source, const Controlled& controlled) noexcept {
    if (source.player != 0)
        return source.player == controlled.entity || (controlled.player != 0 && source.player == controlled.player);
    return source.entity == controlled.entity || source.attacker == controlled.entity;
}
using DamageFunction = void(CABBIRD_CALL*)(std::uintptr_t, std::uintptr_t, std::uintptr_t);
template<class Function> Function NativeCall(Native method) noexcept {
    return reinterpret_cast<Function>(g_compatibility.native[method]);
}
std::uintptr_t MethodIdentity(Native method) noexcept { return reinterpret_cast<std::uintptr_t>(g_native_identity[method]); }
// This owns only a new ClassManager.Get result which has not been submitted to
// AliveElementSystem. CopyTo may register its fresh ID with a parent; the game's
// pool reset and parent lifetime checks handle cancellation of that new child.
// Once submitted, the receiver can retain/free it and this owner is released.
class PendingElement {
public:
    PendingElement() = default;
    explicit PendingElement(std::uintptr_t value) : value_(value) {}
    PendingElement(const PendingElement&) = delete;
    PendingElement& operator=(const PendingElement&) = delete;
    PendingElement(PendingElement&& other) noexcept { Take(other); }
    PendingElement& operator=(PendingElement&& other) noexcept {
        Reset(); Take(other); return *this;
    }
    ~PendingElement() { Reset(); }
    std::uintptr_t Get() const noexcept { return value_; }
    std::uintptr_t Release() noexcept { return std::exchange(value_, 0); }
    void PoolIdentity(std::uint32_t index, std::int32_t version) noexcept { index_ = index; version_ = version; }
    void CopyStarted() noexcept { copied_ = true; }
    void ElementIdentity(std::uint64_t unique) noexcept { unique_ = unique; }
private:
    void Take(PendingElement& other) noexcept {
        value_ = std::exchange(other.value_, 0); index_ = other.index_; version_ = other.version_;
        unique_ = other.unique_; copied_ = other.copied_;
    }
    void Reset() noexcept {
        if (const auto value = Release()) {
            // Parent cleanup can end/recycle a pending child during the original
            // hit. Cancellation must never free that parent's replacement object.
            const auto& layout = g_compatibility.layout;
            std::uint32_t index{}; std::int32_t version{};
            if (!index_ || !Read(value, layout.pool_index, index) || index != index_ ||
                !Read(value, layout.element_version, version) || version != version_) return;
            if (copied_) {
                std::uint8_t free{}; std::uint64_t unique{};
                if (!unique_ || !Read(value, layout.free, free) || free ||
                    !Read(value, layout.unique_id, unique) || unique != unique_) return;
            }
            try { NativeCall<void(CABBIRD_CALL*)(std::uintptr_t, std::uintptr_t)>(Free)(value, MethodIdentity(Free)); }
            catch (...) {}
        }
    }
    std::uintptr_t value_{};
    std::uint32_t index_{};
    std::int32_t version_{};
    std::uint64_t unique_{};
    bool copied_{};
};
struct HitPayload {
    CollisionInfo collision;
    std::int32_t critical{};
    std::uint8_t cache{};
    Vector3 position, forward;
    std::uintptr_t performance{};
};
bool ReadPayload(std::uintptr_t element, HitPayload& payload) noexcept {
    const auto& layout = g_compatibility.layout;
    return Read(element, layout.collision, payload.collision) && Read(element, layout.critical, payload.critical) &&
        Read(element, layout.cache_enabled, payload.cache) && Read(element, layout.cache_position, payload.position) &&
        Read(element, layout.cache_forward, payload.forward) && Read(element, layout.performance, payload.performance);
}
PendingElement CopyFresh(std::uintptr_t receiver, std::uintptr_t source, const HitPayload& payload,
                         const Lifetime& source_lifetime, Lifetime& lifetime, Refusal& refusal) {
    refusal = Refusal::RentFailed;
    auto rented = NativeCall<std::uintptr_t(CABBIRD_CALL*)(std::uintptr_t, std::uintptr_t)>(Rent)(
        reinterpret_cast<std::uintptr_t>(g_damage_runtime_type), MethodIdentity(Rent));
    if (!rented || rented == source) return {};
    PendingElement pending(rented);
    const auto& layout = g_compatibility.layout;
    CabbirdIl2CppClassV1* klass{};
    std::uint32_t index{}; std::int32_t version{};
    if (!Read(rented, layout.object_class, klass) || klass != g_damage_class ||
        !Read(rented, layout.pool_index, index) || !index || !Read(rented, layout.element_version, version)) return {};
    pending.PoolIdentity(index, version);
    refusal = Refusal::CopyFailed;
    auto destination = rented;
    pending.CopyStarted();
    NativeCall<void(CABBIRD_CALL*)(std::uintptr_t, std::uintptr_t*, std::uintptr_t)>(Copy)(source, &destination, MethodIdentity(Copy));
    if (destination != rented) return {};
    std::uint64_t unique{};
    if (!Read(rented, layout.unique_id, unique)) return {};
    pending.ElementIdentity(unique);
    // Use game setters for value types containing managed references and for
    // hit inputs which the game's configuration-oriented CopyTo omits.
    NativeCall<void(CABBIRD_CALL*)(std::uintptr_t, const CollisionInfo*, std::uintptr_t)>(Collision)(rented, &payload.collision, MethodIdentity(Collision));
    NativeCall<void(CABBIRD_CALL*)(std::uintptr_t, std::int32_t, std::uintptr_t)>(Critical)(rented, payload.critical, MethodIdentity(Critical));
    if (payload.cache) NativeCall<void(CABBIRD_CALL*)(std::uintptr_t, const Vector3*, const Vector3*, std::uintptr_t)>(Cache)(
        rented, &payload.position, &payload.forward, MethodIdentity(Cache));
    NativeCall<void(CABBIRD_CALL*)(std::uintptr_t, std::uintptr_t, std::uintptr_t)>(Performance)(rented, payload.performance, MethodIdentity(Performance));
    Refusal ignored{};
    if (!Snapshot(receiver, rented, lifetime, ignored) || lifetime.unique_id == 0 ||
        lifetime.unique_id == source_lifetime.unique_id || lifetime.element_version != version ||
        lifetime.pool_index != index ||
        lifetime.receiver_entity != source_lifetime.receiver_entity || lifetime.receiver_version != source_lifetime.receiver_version) return {};
    return pending;
}
void CABBIRD_CALL DamageDetour(std::uintptr_t receiver, std::uintptr_t element, std::uintptr_t method_info) {
    try {
        const CallbackLease lease;
        if (!lease) return;
        const auto original = reinterpret_cast<DamageFunction>(g_hook_slot.original);
        if (!original) return;
        const auto settings = g_settings.load();
        if (!g_running.load() || !Enabled(settings) || GetCurrentThreadId() != g_game_thread.load() ||
            method_info != reinterpret_cast<std::uintptr_t>(g_method_identity) || g_applying || Count(settings) == 1) {
            original(receiver, element, method_info); return;
        }
        struct Guard { Guard() { g_applying = true; } ~Guard() { g_applying = false; } } guard;
        bool original_called{};
        Source source; Lifetime before; Controlled controlled;
        const char* phase = "before_original"; std::uint32_t repeat_index{};
        try {
            if (!ControlledNow(controlled)) { original_called = true; original(receiver, element, method_info); return; }
            Refusal refusal = Refusal::SourceRead;
            const auto& layout = g_compatibility.layout;
            bool replayable = Read(element, layout.source_entity, source.entity) &&
                Read(element, layout.source_player, source.player) && Read(element, layout.attacker_entity, source.attacker);
            if (replayable && !Owned(source, controlled)) { refusal = Refusal::ForeignSource; replayable = false; }
            if (replayable) replayable = Snapshot(receiver, element, before, refusal);
            if (replayable && before.receiver_entity == controlled.entity) { refusal = Refusal::SelfTarget; replayable = false; }
            CabbirdIl2CppClassV1* klass{};
            if (replayable && (!Read(element, layout.object_class, klass) || klass != g_damage_class)) {
                refusal = Refusal::Unsupported; replayable = false;
            }
            HitPayload payload;
            if (replayable && !ReadPayload(element, payload)) { refusal = Refusal::PayloadRead; replayable = false; }
            Lifetime pending_lifetime;
            auto pending = replayable ? CopyFresh(receiver, element, payload, before, pending_lifetime, refusal) : PendingElement{};
            if (replayable && !pending.Get()) replayable = false;
            original_called = true; phase = "original";
            original(receiver, element, method_info);
            if (!replayable) { Refuse(refusal, source, before, controlled, "before_original", 0); return; }
            for (std::uint32_t repeat = 1; repeat < Count(settings) && g_running.load() && Enabled(g_settings.load()); ++repeat) {
                phase = "before_repeat"; repeat_index = repeat;
                Controlled current;
                if (!ControlledNow(current) || !(current == controlled)) { Refuse(Refusal::PlayerChanged, source, before, controlled, phase, repeat); break; }
                Lifetime after;
                if (!Snapshot(receiver, pending.Get(), after, refusal)) { Refuse(refusal, source, after, controlled, phase, repeat); break; }
                if (!(after == pending_lifetime)) { Refuse(Refusal::Changed, source, after, controlled, phase, repeat); break; }
                // Copy the next input while the current input is still active.
                // No damage call or copy consumes an element after ExecuteEnd.
                Lifetime next_lifetime;
                auto next = repeat + 1 < Count(settings) ? CopyFresh(receiver, pending.Get(), payload, after, next_lifetime, refusal) : PendingElement{};
                const bool copy_failed = repeat + 1 < Count(settings) && !next.Get();
                phase = "repeat"; original(receiver, pending.Release(), method_info);
                if (copy_failed) { Refuse(refusal, source, after, controlled, "before_repeat", repeat + 1); break; }
                pending = std::move(next); pending_lifetime = next_lifetime;
            }
        } catch (...) {
            Refuse(Refusal::CallbackFailed, source, before, controlled, phase, repeat_index);
            if (!original_called) original(receiver, element, method_info);
        }
    } catch (...) {}
}
bool InstallHook() {
    if (g_hook_slot.handle.id) return true;
    if (!g_compatibility.ready || !ValidateLiveMetadata()) return false;
    const CabbirdHookRequestV1 request{sizeof(request), CABBIRD_HOOK_V1_FUNCTION, g_compatibility.address,
        reinterpret_cast<void*>(&DamageDetour), StringView("cabbird.damage-replay")};
    const auto status = g_hook->create(g_hook->user, &request, &g_hook_slot.original, &g_hook_slot.handle);
    if (status.code != CABBIRD_STATUS_V1_OK) {
        Publish("status.hook", "Damage hook operation failed: {0}", StatusDetail(status));
        return false;
    }
    return true;
}
bool RemoveHook() {
    if (!g_hook_slot.handle.id) return true;
    const auto status = g_hook->release(g_hook->user, g_hook_slot.handle);
    if (status.code != CABBIRD_STATUS_V1_OK && status.code != CABBIRD_STATUS_V1_NOT_FOUND) {
        Publish("status.hook", "Damage hook operation failed: {0}", StatusDetail(status));
        return false;
    }
    // release (or host PreStop) disables the hook and drains its generation.
    g_hook_slot = {};
    g_method_identity = nullptr;
    g_game_thread.store(0);
    return true;
}

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** context) {
    if (!host || !context) return Error(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "host/context is missing");
    *context = nullptr;
    try {
        const Host services(host);
        g_core = services.Query<CabbirdCoreServiceV1>(CABBIRD_CORE_SERVICE_V1_ID).get();
        g_ui = services.Query<CabbirdUiServiceV1>(CABBIRD_UI_SERVICE_V1_ID).get();
        g_hook = services.Query<CabbirdHookServiceV1>(CABBIRD_HOOK_SERVICE_V1_ID).get();
        g_config = services.Query<CabbirdConfigServiceV1>(CABBIRD_CONFIG_SERVICE_V1_ID).get();
        g_json = services.Query<CabbirdJsonServiceV1>(CABBIRD_JSON_SERVICE_V1_ID).get();
        g_il2cpp = services.Query<CabbirdIl2CppServiceV1>(CABBIRD_IL2CPP_SERVICE_V1_ID).get();
        g_metadata = services.Query<CabbirdUnityMetadataServiceV1>(CABBIRD_UNITY_METADATA_SERVICE_V1_ID).get();
        g_player = services.Query<CabbirdUnityPlayerServiceV1>(CABBIRD_UNITY_PLAYER_SERVICE_V1_ID).get();
        if (!g_core || g_core->struct_size < sizeof(*g_core) || !g_core->log || !g_core->read_memory || !g_core->module_base ||
            !g_ui || g_ui->struct_size < sizeof(*g_ui) || !g_ui->begin_window || !g_ui->end_window ||
            !g_ui->checkbox || !g_ui->input_uint32 ||
            !g_hook || g_hook->struct_size < sizeof(*g_hook) || !g_hook->create || !g_hook->release ||
            !g_hook->begin_callback || !g_hook->end_callback ||
            !g_config || g_config->struct_size < sizeof(*g_config) || !g_config->register_schema || !g_config->read || !g_config->write_atomic ||
            !g_json || g_json->struct_size < sizeof(*g_json) || !g_json->parse || !g_json->release ||
            !g_json->object_find || !g_json->boolean_value || !g_json->number_value ||
            !g_il2cpp || g_il2cpp->struct_size < sizeof(*g_il2cpp) || !g_il2cpp->with_runtime || !g_il2cpp->release_handles ||
            !g_metadata || g_metadata->struct_size < sizeof(*g_metadata) || !g_metadata->resolve_method || !g_metadata->resolve_instance_field ||
            !g_player || g_player->struct_size < offsetof(CabbirdUnityPlayerServiceV1, snapshot) + sizeof(g_player->snapshot) || !g_player->snapshot)
            return Error(CABBIRD_STATUS_V1_UNAVAILABLE, "required damage multiplier service is unavailable");
        g_text = cabbird::sdk::Localizer(host);
        g_save.Bind(host, g_config, kSettingsId, 1);
        g_settings.store(Settings(false, kDefaultCount));
        g_running.store(false); g_intent.store(0);
        g_hook_slot = {}; g_compatibility = {}; g_method_identity = nullptr; g_native_identity = {};
        g_damage_class = nullptr; g_damage_runtime_type = nullptr; g_type_handle = nullptr;
        g_game_thread.store(0); g_applying = false; g_status.clear();
        g_reported_refusals = 0; g_next_bind = {};
        CabbirdGenerationHandleV1 registration{};
        auto status = g_config->register_schema(g_config->user, StringView(kSettingsId), 1,
            Bytes(kSettingsSchema), &registration);
        if (status.code != CABBIRD_STATUS_V1_OK) return status;
        try { RestoreSettings(); }
        catch (const std::exception& error) {
            Publish("settings.restore-failed", "Saved settings could not be restored: {0}", error.what());
        }
        try { PrepareCompatibility(services); }
        catch (const std::exception& error) { Publish("status.profile", "Compatibility profile unavailable: {0}", error.what()); }
        *context = &g_settings;
        return Ok();
    } catch (...) { return Error(CABBIRD_STATUS_V1_FAILED, "damage multiplier load failed"); }
}
CabbirdStatusV1 CABBIRD_CALL Start(void*) {
    g_running.store(true);
    if (Enabled(g_settings.load())) g_intent.store(1);
    return Ok();
}
CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) {
    try {
        g_running.store(false); g_intent.store(0);
        const bool removed = RemoveHook();
        const bool saved = g_save.Flush(); // Lifecycle, after the host drains Worker callbacks.
        if (!removed) return Error(CABBIRD_STATUS_V1_FAILED, "damage multiplier hook did not drain");
        if (g_type_handle) {
            const auto status = g_il2cpp->release_handles(g_il2cpp->user, &g_type_handle, 1);
            if (status.code != CABBIRD_STATUS_V1_OK) return status;
            g_type_handle = nullptr; g_damage_class = nullptr; g_damage_runtime_type = nullptr;
        }
        return saved ? Ok() : Error(CABBIRD_STATUS_V1_FAILED, "damage multiplier settings did not save");
    } catch (...) { return Error(CABBIRD_STATUS_V1_FAILED, "damage multiplier stop failed"); }
}
void CABBIRD_CALL Unload(void*) {
    try {
        g_save.Reset();
        g_core = nullptr; g_ui = nullptr; g_hook = nullptr; g_config = nullptr;
        g_json = nullptr; g_il2cpp = nullptr; g_metadata = nullptr; g_player = nullptr;
        g_compatibility = {}; g_text = {}; g_status.clear();
    } catch (...) {}
}
void CABBIRD_CALL Update(void*, double) {
    try {
        if (!g_running.load()) return;
        const auto intent = g_intent.exchange(0);
        if (intent == 1) { g_reported_refusals = 0; g_next_bind = {}; }
        if (!Enabled(g_settings.load())) {
            if (g_hook_slot.handle.id) static_cast<void>(RemoveHook());
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!g_hook_slot.handle.id && g_compatibility.ready && now >= g_next_bind) {
            g_next_bind = now + std::chrono::milliseconds(500);
            static_cast<void>(InstallHook());
        }
    } catch (...) {
        try { Publish("status.update-failed", "Damage multiplier could not apply the requested operation."); }
        catch (...) {}
    }
}
void CABBIRD_CALL Draw(void*, const CabbirdUiServiceV1* ui) {
    try {
        if (!ui || !g_running.load()) return;
        const auto title = g_text.Label("window.title", "Damage Multiplier", "cabbird.damage-replay");
        int open = 1;
        const cabbird::sdk::UiWindow window(ui, title, &open);
        if (!window) return;
        auto settings = g_settings.load();
        int enabled = Enabled(settings) ? 1 : 0;
        const auto enabled_label = g_text.Text("settings.enabled", "Enabled");
        const bool toggled = ui->checkbox(ui->user, StringView(enabled_label), &enabled) != 0;
        auto count = Count(settings);
        const auto count_label = g_text.Text("settings.count", "Total calls per damage event");
        const bool edited = ui->input_uint32(ui->user, StringView(count_label), &count, 1, 10) != 0;
        if (toggled || edited) {
            count = (std::max)(1U, (std::min)(count, kMaximumCount));
            settings = Settings(enabled != 0, count);
            g_settings.store(settings);
            Persist(settings);
            if (toggled) g_intent.store(enabled ? 1 : 2);
        }
    } catch (...) {}
}
} // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(CabbirdPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor))
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        StringView("cabbird.damage-replay"), StringView("Damage Multiplier"), StringView("Cabbird"),
        StringView("1.0.0"), Load, Start, Stop, Unload, Update, Draw};
    return Ok();
}

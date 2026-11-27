#include "cabbird/sdk/il2cpp.hpp"
#include "cabbird/sdk/localization.hpp"
#include "cabbird/sdk/profile.hpp"
#include "cabbird/sdk/services/game_lifecycle.h"
#include "cabbird/sdk/services/unity_metadata.h"

#include <atomic>
#include <chrono>
#include <memory>

namespace {
using namespace cabbird::sdk;
using Object = CabbirdIl2CppObjectV1;
using Method = CabbirdMethodInfoV1;
using Class = CabbirdIl2CppClassV1;
using Runtime = cabbird::sdk::il2cpp::Context;
constexpr auto kNamespace = "Lens.Gameplay.Modules.BigWorld";
constexpr auto kData = "Lens.Gameplay.Modules.BigWorld.BaseData";
constexpr auto kAlive = "Lens.Gameplay.Modules.BigWorld.AliveData";
constexpr auto kElements = "Lens.Gameplay.Modules.BigWorld.AliveElementSystem";
constexpr auto kAdd = "Lens.Gameplay.Modules.BigWorld.AddElementStruct";

CabbirdStatusV1 Status(std::uint32_t code) noexcept { return {code, 0, {}}; }
template<class T> const T* Query(const CabbirdHostApiV1* host, std::string_view id) {
    const auto service = Host(host).Query<T>(id);
    return service && service->struct_size >= sizeof(T) ? service.get() : nullptr;
}
struct Identity {
    std::uintptr_t data{};
    std::int32_t id{}, version{};
    std::uint32_t index{};
    std::uint64_t scene{};
    bool operator==(const Identity&) const = default;
};
struct OwnedElement { Identity owner; std::uint64_t unique{}; };
struct Context {
    explicit Context(const CabbirdHostApiV1* host) : text(host) {}
    Localizer text;
    const CabbirdCoreServiceV1* core{};
    const CabbirdUnityPlayerServiceV1* player{};
    const CabbirdIl2CppServiceV1* il2cpp{};
    const CabbirdUnityMetadataServiceV1* metadata{};
    const CabbirdGameLifecycleServiceV1* lifecycle{};
    CabbirdGenerationHandleV1 cleanup{};
    std::atomic_bool running{};
    std::atomic<std::uint32_t> reported{};
    Profile profile;
    std::uint32_t idOffset{}, indexOffset{}, versionOffset{}, entityDataOffset{}, addSize{}, verifySize{};
    std::int32_t elementId{};
    const Method *getEntity{}, *handleEntity{}, *entityValid{}, *getProperty{}, *getElement{},
        *hasElement{}, *inject{}, *freeElement{}, *elementConfig{}, *elementFree{};
    CabbirdFieldInfoV1* nullVerify{};
    Class *heroClass{}, *buffClass{};
    bool bound{};
    std::uint32_t bindStatus{CABBIRD_STATUS_V1_UNAVAILABLE};
    std::uint64_t mismatchedRva{};
    std::string_view mismatchedEntry;
    OwnedElement owned; // Game-owned scalar identity; no retained managed object or runtime table.
    CabbirdUnityPlayerSnapshotV1 playerFrame{sizeof(playerFrame)};
    std::chrono::steady_clock::time_point nextBind{}, nextApply{};
};

void LogOnce(Context& c, std::uint32_t bit, std::string_view key, std::string_view fallback) noexcept {
    try {
        if ((c.reported.fetch_or(bit) & bit) == 0 && c.core && c.core->log)
            c.core->log(c.core->user, CABBIRD_CORE_LOG_LEVEL_V1_WARNING, StringView(c.text.Text(key, fallback)));
    } catch (...) {}
}
template<class T> bool Read(Context& c, std::uintptr_t object, std::uint32_t offset, T& value) {
    return object && Succeeded(c.core->read_memory(c.core->user, object + offset,
        {reinterpret_cast<std::uint8_t*>(&value), sizeof(value)}));
}
bool ReadIdentity(Context& c, std::uintptr_t data, Identity& result) {
    result.data = data;
    return Read(c, data, c.idOffset, result.id) && result.id != 0 &&
        Read(c, data, c.indexOffset, result.index) && Read(c, data, c.versionOffset, result.version);
}
template<class T> bool InvokeValue(const Runtime& runtime, const Method* method, Object* self,
                                  void** args, T& result) {
    Object* boxed{};
    if (!runtime.Invoke(method, self, args, &boxed) || !boxed) return false;
    const auto* value = static_cast<const T*>(runtime.Functions().il2cpp_object_unbox(boxed));
    if (!value) return false;
    result = *value;
    return true;
}
const Method* MethodShape(Context& c, const Runtime& runtime, const char* type, const char* ns,
                          const char* cls, const char* name, const char* result,
                          std::initializer_list<const char*> parameters, bool isStatic = false,
                          std::string_view entry = {}) {
    std::vector<CabbirdStringViewV1> arguments;
    for (auto* parameter : parameters) arguments.push_back(StringView(parameter));
    CabbirdUnityMethodRequestV1 request{sizeof(request)};
    request.method_kind = isStatic ? CABBIRD_UNITY_METHOD_V1_STATIC : CABBIRD_UNITY_METHOD_V1_INSTANCE;
    request.module_name = StringView(c.profile.module);
    request.type_name = StringView(type); request.method_name = StringView(name);
    request.return_type = StringView(result); request.parameter_types = arguments.data();
    request.parameter_count = arguments.size();
    CabbirdUnityMethodResultV1 resolved{sizeof(resolved)};
    const auto status = c.metadata->resolve_method(c.metadata->user, &request, &resolved);
    if (!Succeeded(status)) { c.bindStatus = status.code; return nullptr; }
    if (!entry.empty()) {
        const auto expected = ResolveProfileSymbolRva(c.core, c.profile, entry);
        if (!expected || expected.address != resolved.address) {
            c.bindStatus = CABBIRD_STATUS_V1_CONFLICT;
            c.mismatchedRva = resolved.rva; c.mismatchedEntry = entry;
            return nullptr;
        }
    }
    const auto* method = runtime.FindMethodBySignature(runtime.FindClassAnyImage(ns, cls), name, parameters);
    if (!method) c.bindStatus = CABBIRD_STATUS_V1_NOT_FOUND;
    return method; // The read-only service validated its unique complete shape.
}
bool FieldShape(Context& c, const char* type, const char* field, const char* valueType,
                 std::uint32_t size, const char* key) {
    CabbirdUnityFieldRequestV1 request{sizeof(request), size, StringView(type), StringView(field), StringView(valueType)};
    CabbirdUnityFieldResultV1 result{sizeof(result)};
    const auto status = c.metadata->resolve_instance_field(c.metadata->user, &request, &result);
    if (!Succeeded(status)) { c.bindStatus = status.code; return false; }
    if (result.offset != c.profile.Value(key, 4096) || result.storage_size != size) {
        c.bindStatus = CABBIRD_STATUS_V1_CONFLICT;
        return false;
    }
    return true;
}
bool Bind(Context& c, const Runtime& runtime) {
    if (c.bound) return true;
    struct Field { const char *type, *name, *valueType, *key; std::uint32_t size; };
    const Field fields[]{
        {kData, "<entityId>k__BackingField", "System.Int32", "dataId", 4},
        {kData, "<objIndex>k__BackingField", "System.UInt32", "dataIndex", 4},
        {kData, "<version>k__BackingField", "System.Int32", "dataVersion", 4},
        {"Lens.Gameplay.Modules.BigWorld.Entity", "data", kData, "entityData", sizeof(void*)},
        {kAdd, "layer", "System.Int32", "addLayer", 4},
        {kAdd, "isRemote", "System.Boolean", "addRemote", 1},
        {kAdd, "removeOnSwitchClient", "System.Boolean", "addRemoveOnSwitch", 1},
        {kAdd, "uniqueID", "System.UInt64", "addUnique", 8},
        {kAdd, "parentUniqueID", "System.UInt64", "addParentUnique", 8},
        {kAdd, "collisionInfo", "Lens.Gameplay.Modules.BigWorld.CollisionInfo", "addCollision",
            c.profile.Value("collisionSize", 256, 8)},
        {kAdd, "attackPointInfo", "Lens.Gameplay.Modules.BigWorld.AttackPointInfo", "addAttackPoint",
            c.profile.Value("attackPointSize", 256, 8)},
        {kAdd, "duration", "System.Single", "addDuration", 4},
        {kAdd, "customReboundId", "System.UInt64", "addReboundId", 8},
        {kAdd, "customReboundType", "System.UInt32", "addReboundType", 4},
        {"CustomBattleVerifyInfo", "battleIndex", "System.UInt64", "verifyBattleIndex", 8},
        {"CustomBattleVerifyInfo", "relatedIndex", "System.UInt64", "verifyRelatedIndex", 8},
        {"CustomBattleVerifyInfo", "skillVerifyType", "Protocal.Cs.SkillVerifyType", "verifyKind", 4},
        {"CustomBattleVerifyInfo", "source_id", "System.Int64", "verifySource", 8},
        {"CustomBattleVerifyInfo", "opTime", "System.UInt64", "verifyTime", 8}};
    for (const auto& field : fields)
        if (!FieldShape(c, field.type, field.name, field.valueType, field.size, field.key)) return false;
    c.getEntity = MethodShape(c, runtime, "Lens.Gameplay.Modules.BigWorld.EntityManager", kNamespace,
        "EntityManager", "GetEntity", "Lens.Gameplay.Modules.BigWorld.EntityHandle", {"System.Int32"}, true);
    c.handleEntity = MethodShape(c, runtime, "Lens.Gameplay.Modules.BigWorld.EntityHandle", kNamespace,
        "EntityHandle", "get_entity", "Lens.Gameplay.Modules.BigWorld.Entity", {});
    c.entityValid = MethodShape(c, runtime, "Lens.Gameplay.Modules.BigWorld.Entity", kNamespace,
        "Entity", "IsValid", "System.Boolean", {});
    c.getProperty = MethodShape(c, runtime, kAlive, kNamespace,
        "AliveData", "get_elementProperty", "Lens.Gameplay.Modules.BigWorld.ElementProperty", {});
    c.getElement = MethodShape(c, runtime, "Lens.Gameplay.Modules.BigWorld.ElementProperty", kNamespace,
        "ElementProperty", "GetExistByUniqueId", "Lens.Gameplay.Modules.BigWorld.BaseElement", {"System.UInt64"});
    c.elementConfig = MethodShape(c, runtime, "Lens.Gameplay.Modules.BigWorld.BaseElement", kNamespace,
        "BaseElement", "get_elementConfigId", "System.Int32", {});
    c.elementFree = MethodShape(c, runtime, "Lens.Gameplay.Modules.BigWorld.BaseElement", kNamespace,
        "BaseElement", "get_free", "System.Boolean", {});
    c.hasElement = MethodShape(c, runtime, kElements, kNamespace, "AliveElementSystem", "HasElement",
        "System.Boolean", {"Lens.Gameplay.Modules.BigWorld.EntityHandle", "System.Int32", "System.UInt64&"}, true,
        "hasElement");
    c.inject = MethodShape(c, runtime, kElements, kNamespace, "AliveElementSystem", "InjectElement", "System.Void",
        {"System.Int32", "System.Int32", "System.Int32", "System.Int32", kAdd, "CustomBattleVerifyInfo",
         "System.Collections.Generic.List<Protocal.Cs.AttributeEntry>", "System.Int32",
         "System.Collections.Generic.List<Protocal.Cs.BattleLinkNode>", "System.Int32", "System.UInt64", "System.Int32"},
        true, "injectElement");
    c.freeElement = MethodShape(c, runtime, kElements, kNamespace, "AliveElementSystem", "FreeElement",
        "System.Void", {"System.UInt64", "System.Int32"}, true, "freeElement");
    c.heroClass = runtime.FindClassAnyImage(kNamespace, "HeroData");
    c.buffClass = runtime.FindClassAnyImage(kNamespace, "BuffElement");
    c.nullVerify = runtime.FindField(runtime.FindClassAnyImage("", "CustomBattleVerifyInfo"), "s_null");
    const auto& api = runtime.Functions();
    const bool verify = c.nullVerify && (api.il2cpp_field_get_flags(c.nullVerify) & 0x10U) &&
        runtime.TypeName(api.il2cpp_field_get_type(c.nullVerify)) == "CustomBattleVerifyInfo";
    if (!verify || !c.heroClass || !c.buffClass) c.bindStatus = CABBIRD_STATUS_V1_NOT_FOUND;
    c.bound = c.getEntity && c.handleEntity && c.entityValid && c.getProperty && c.getElement &&
        c.hasElement && c.inject && c.freeElement && c.elementConfig && c.elementFree && c.heroClass && c.buffClass && verify;
    return c.bound;
}

enum class OwnerState { Missing, Ready, Failed };
// Resolving through EntityManager each time rejects recycled data/scene identities.
OwnerState ResolveOwner(Context& c, const Runtime& runtime, const Identity& identity, Object*& handle, Object*& data) {
    void* args[]{const_cast<std::int32_t*>(&identity.id)};
    Object* entity{};
    if (!runtime.Invoke(c.getEntity, nullptr, args, &handle)) return OwnerState::Failed;
    if (!handle) return OwnerState::Missing;
    if (!runtime.Invoke(c.handleEntity, handle, nullptr, &entity)) return OwnerState::Failed;
    if (!entity) return OwnerState::Missing;
    bool valid{};
    std::uintptr_t current{};
    Identity observed;
    if (!InvokeValue(runtime, c.entityValid, entity, nullptr, valid)) return OwnerState::Failed;
    if (!valid) return OwnerState::Missing;
    if (!Read(c, reinterpret_cast<std::uintptr_t>(entity), c.entityDataOffset, current)) return OwnerState::Failed;
    if (current != identity.data) return OwnerState::Missing;
    if (!ReadIdentity(c, current, observed)) return OwnerState::Failed;
    if (observed.id != identity.id || observed.index != identity.index || observed.version != identity.version)
        return OwnerState::Missing;
    data = reinterpret_cast<Object*>(current);
    return OwnerState::Ready;
}
bool HasElement(Context& c, const Runtime& runtime, Object* handle, std::uint64_t& unique, bool& present) {
    unique = 0;
    void* args[]{handle, &c.elementId, &unique};
    return InvokeValue(runtime, c.hasElement, nullptr, args, present);
}
bool FindElement(Context& c, const Runtime& runtime, Object* data, std::uint64_t unique, Object*& element) {
    Object* property{};
    void* args[]{&unique};
    return runtime.Invoke(c.getProperty, data, nullptr, &property) && property &&
        runtime.Invoke(c.getElement, property, args, &element);
}
void Detail(Context& c, const Runtime& runtime, std::uint32_t bit, std::string_view stage,
            Object* element = nullptr, std::uint64_t unique = 0) {
    if ((c.reported.fetch_or(bit) & bit) != 0) return;
    std::int32_t actualConfig{-1};
    std::string actualType = "unavailable";
    if (element) {
        const auto& api = runtime.Functions();
        actualType = runtime.TypeName(api.il2cpp_class_get_type(api.il2cpp_object_get_class(element)));
        static_cast<void>(InvokeValue(runtime, c.elementConfig, element, nullptr, actualConfig));
    }
    const auto requested = std::to_string(c.elementId), actual = std::to_string(actualConfig), id = std::to_string(unique);
    const std::string_view arguments[]{stage, requested, actual, actualType, id};
    const auto message = c.text.Format("log.detail",
        "God Mode: {0}; requested={1}; actualConfig={2}; type={3}; uniqueId={4}.", arguments);
    c.core->log(c.core->user, CABBIRD_CORE_LOG_LEVEL_V1_WARNING, StringView(message));
}
bool ConfirmElement(Context& c, const Runtime& runtime, Object* element, std::uint64_t unique) {
    // The canonical invincibility config creates a BuffElement. Its native Execute
    // owns the configured effects and child elements; Reset removes those children.
    if (runtime.Functions().il2cpp_object_get_class(element) != c.buffClass) {
        Detail(c, runtime, 8192, "ElementType", element, unique);
        return false;
    }
    std::int32_t config{};
    if (!InvokeValue(runtime, c.elementConfig, element, nullptr, config) || config != c.elementId) {
        Detail(c, runtime, 32768, "ElementConfig", element, unique);
        return false;
    }
    bool free{};
    if (!InvokeValue(runtime, c.elementFree, element, nullptr, free) || free) {
        Detail(c, runtime, 65536, "ElementInactive", element, unique);
        return false;
    }
    return true;
}
bool FindConfirmedElement(Context& c, const Runtime& runtime, Object* data, std::uint64_t unique) {
    Object* element{};
    if (!FindElement(c, runtime, data, unique, element) || !element) {
        Detail(c, runtime, 4096, "GetElement", nullptr, unique);
        return false;
    }
    return ConfirmElement(c, runtime, element, unique);
}
bool Restore(Context& c, const Runtime& runtime) {
    if (!c.owned.unique) return true;
    Object *handle{}, *data{};
    const auto owner = ResolveOwner(c, runtime, c.owned.owner, handle, data);
    if (owner == OwnerState::Failed) { Detail(c, runtime, 256, "ResolveOwner"); return false; }
    if (owner == OwnerState::Missing) {
        c.owned = {}; // The original entity has gone; never act on its replacement.
        return true;
    }
    Object* element{};
    if (!FindElement(c, runtime, data, c.owned.unique, element)) return false;
    if (element) {
        void* args[]{&c.owned.unique, &c.owned.owner.id};
        Object* ignored{};
        if (!runtime.Invoke(c.freeElement, nullptr, args, &ignored) ||
            !FindElement(c, runtime, data, c.owned.unique, element) || element) return false;
    }
    c.owned = {};
    return true;
}
bool Apply(Context& c, const Runtime& runtime, const Identity& identity) {
    Object *handle{}, *data{};
    const auto owner = ResolveOwner(c, runtime, identity, handle, data);
    if (owner == OwnerState::Failed) { Detail(c, runtime, 256, "ResolveOwner"); return false; }
    if (owner == OwnerState::Missing) return true; // Normal world/role transition.
    if (!runtime.Functions().il2cpp_class_is_assignable_from(c.heroClass,
            runtime.Functions().il2cpp_object_get_class(data))) return true;
    if (c.owned.unique) {
        Object* element{};
        if (!FindElement(c, runtime, data, c.owned.unique, element)) return false;
        if (element) return ConfirmElement(c, runtime, element, c.owned.unique);
        c.owned = {};
    }
    bool present{};
    std::uint64_t unique{};
    if (!HasElement(c, runtime, handle, unique, present)) { Detail(c, runtime, 512, "HasBefore"); return false; }
    if (present) return unique != 0 && FindConfirmedElement(c, runtime, data, unique);
    // The game's EnsureRecordingHeroInvincible caller supplies a zero AddElementStruct,
    // reads CustomBattleVerifyInfo.s_null, and uses this hero for attacker/executor.
    std::vector<std::uint8_t> add(c.addSize), verify(c.verifySize);
    runtime.Functions().il2cpp_field_static_get_value(c.nullVerify, verify.data());
    std::int32_t zero{};
    std::uint64_t zero64{};
    void* args[]{&c.elementId, &zero, const_cast<std::int32_t*>(&identity.id), const_cast<std::int32_t*>(&identity.id),
        add.data(), verify.data(), nullptr, &zero, nullptr, &zero, &zero64, &zero};
    Object* ignored{};
    const bool invoked = runtime.Invoke(c.inject, nullptr, args, &ignored);
    if (!invoked) Detail(c, runtime, 1024, "InjectInvoke");
    // A void return alone is not evidence of application. Capture the actual native element.
    if (!HasElement(c, runtime, handle, unique, present) || !present || unique == 0) {
        Detail(c, runtime, 2048, "HasAfter", nullptr, unique);
        return false;
    }
    c.owned = {identity, unique};
    if (invoked && FindConfirmedElement(c, runtime, data, unique)) return true;
    static_cast<void>(Restore(c, runtime));
    return false;
}
CabbirdStatusV1 CABBIRD_CALL GameWork(void* raw, const CabbirdIl2CppApiV1* api) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        if (!api || api->struct_size < sizeof(*api) || api->api_version != 1) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        Runtime runtime(*api);
        if (!Bind(c, runtime)) {
            if (c.mismatchedRva && (c.reported.fetch_or(64) & 64) == 0) {
                const auto rva = std::to_string(c.mismatchedRva);
                const std::string_view arguments[]{c.mismatchedEntry, rva};
                const auto message = c.text.Format("log.entry",
                    "God Mode: the {0} method entry does not match its profile (RVA {1}).", arguments);
                c.core->log(c.core->user, CABBIRD_CORE_LOG_LEVEL_V1_WARNING, StringView(message));
            } else if (!c.mismatchedRva && c.bindStatus != CABBIRD_STATUS_V1_UNAVAILABLE)
                LogOnce(c, 64, "log.binding", "God Mode: game metadata or native entries do not match the compatibility profile.");
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        }
        const auto& player = c.playerFrame;
        Identity identity;
        const bool active = c.running.load() &&
            (player.flags & CABBIRD_UNITY_PLAYER_V1_VALID) &&
            !(player.flags & CABBIRD_UNITY_PLAYER_V1_IDENTITY_PROXIMITY) &&
            ReadIdentity(c, static_cast<std::uintptr_t>(player.entity_id), identity);
        identity.scene = player.generation;
        if (c.owned.unique && (!active || c.owned.owner != identity) && !Restore(c, runtime))
            return Status(CABBIRD_STATUS_V1_FAILED);
        if (!active) return Ok();
        if (!Apply(c, runtime, identity)) return Status(CABBIRD_STATUS_V1_FAILED);
        return Ok();
    } catch (...) { return Status(CABBIRD_STATUS_V1_FAILED); }
}
CabbirdStatusV1 CABBIRD_CALL RestoreWork(void* raw, const CabbirdIl2CppApiV1* api) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        if (!api || api->struct_size < sizeof(*api) || api->api_version != 1) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        return Restore(c, Runtime(*api)) ? Ok() : Status(CABBIRD_STATUS_V1_FAILED);
    } catch (...) { return Status(CABBIRD_STATUS_V1_FAILED); }
}
CabbirdStatusV1 CABBIRD_CALL Cleanup(void* raw) noexcept {
    auto& c = *static_cast<Context*>(raw);
    c.running.store(false);
    if (!c.owned.unique) return Ok();
    const auto status = c.il2cpp->with_runtime(c.il2cpp->user, RestoreWork, &c);
    if (!Succeeded(status)) LogOnce(c, 4, "log.restore", "God Mode: the owned invincibility element could not be removed.");
    return status;
}
void CABBIRD_CALL Update(void* raw, double) noexcept {
    auto& c = *static_cast<Context*>(raw);
    try {
        if (!c.running.load()) return;
        c.playerFrame = {sizeof(c.playerFrame)};
        // The host plugin switch owns activation. Renew player demand on each active Game tick.
        static_cast<void>(c.player->snapshot(c.player->user, &c.playerFrame));
        const auto now = std::chrono::steady_clock::now();
        if (!c.bound && now < c.nextBind) return;
        if (!c.bound) c.nextBind = now + std::chrono::seconds(1);
        if (!c.owned.unique && now < c.nextApply) return;
        const auto status = c.il2cpp->with_runtime(c.il2cpp->user, GameWork, &c);
        if (!Succeeded(status)) {
            c.nextApply = now + std::chrono::seconds(1);
            if (status.code != CABBIRD_STATUS_V1_UNAVAILABLE && status.code != CABBIRD_STATUS_V1_NOT_FOUND)
                LogOnce(c, 2, "log.apply", "God Mode: the game's invincibility element could not be applied.");
        }
    } catch (...) { LogOnce(c, 2, "log.apply", "God Mode: the game's invincibility element could not be applied."); }
}

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** result) noexcept {
    if (!result) return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    *result = nullptr;
    try {
        auto c = std::make_unique<Context>(host);
        c->core = Query<CabbirdCoreServiceV1>(host, CABBIRD_CORE_SERVICE_V1_ID);
        c->player = Query<CabbirdUnityPlayerServiceV1>(host, CABBIRD_UNITY_PLAYER_SERVICE_V1_ID);
        c->il2cpp = Query<CabbirdIl2CppServiceV1>(host, CABBIRD_IL2CPP_SERVICE_V1_ID);
        c->metadata = Query<CabbirdUnityMetadataServiceV1>(host, CABBIRD_UNITY_METADATA_SERVICE_V1_ID);
        c->lifecycle = Query<CabbirdGameLifecycleServiceV1>(host, CABBIRD_GAME_LIFECYCLE_SERVICE_V1_ID);
        if (!c->core || !c->core->read_memory || !c->core->module_base || !c->core->log ||
            !c->player || !c->player->snapshot || !c->il2cpp || !c->il2cpp->with_runtime ||
            !c->metadata || !c->metadata->resolve_method || !c->metadata->resolve_instance_field ||
            !c->lifecycle || !c->lifecycle->register_stop_cleanup || !c->lifecycle->unregister_stop_cleanup)
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        auto profile = LoadProfile(Host(host), "compatibility.profile.json",
            {"ap", "GameAssembly.dll", "il2cpp", "cabbird.god-mode.v1"});
        if (!profile) {
            LogOnce(*c, 128, "log.profile", "God Mode: the compatibility profile is unavailable.");
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        }
        c->profile = std::move(profile).value();
        c->idOffset = c->profile.Value("dataId", 4096, 4);
        c->indexOffset = c->profile.Value("dataIndex", 4096, 4);
        c->versionOffset = c->profile.Value("dataVersion", 4096, 4);
        c->entityDataOffset = c->profile.Value("entityData", 4096, sizeof(void*));
        c->elementId = static_cast<std::int32_t>(c->profile.Value("elementId", INT32_MAX));
        c->addSize = c->profile.Value("addSize", 256, 8);
        c->verifySize = c->profile.Value("verifySize", 256, 8);
        if (!c->elementId || !c->addSize || !c->verifySize) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        const auto cleanup = c->lifecycle->register_stop_cleanup(c->lifecycle->user, Cleanup, c.get(), &c->cleanup);
        if (!Succeeded(cleanup)) return cleanup;
        *result = c.release();
        return Ok();
    } catch (...) { return Status(CABBIRD_STATUS_V1_FAILED); }
}
CabbirdStatusV1 CABBIRD_CALL Start(void* raw) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        c.running.store(true);
        return Ok();
    } catch (...) { return Status(CABBIRD_STATUS_V1_FAILED); }
}
CabbirdStatusV1 CABBIRD_CALL Stop(void* raw, std::uint32_t) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        c.running.store(false);
        if (c.owned.unique) return Status(CABBIRD_STATUS_V1_FAILED);
        if (c.cleanup.id) {
            const auto status = c.lifecycle->unregister_stop_cleanup(c.lifecycle->user, c.cleanup);
            if (!Succeeded(status) && status.code != CABBIRD_STATUS_V1_NOT_FOUND) return status;
            c.cleanup = {};
        }
        return Ok();
    } catch (...) { return Status(CABBIRD_STATUS_V1_FAILED); }
}
void CABBIRD_CALL Unload(void* raw) noexcept { try { delete static_cast<Context*>(raw); } catch (...) {} }
} // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(CabbirdPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor) || descriptor->api_major != CABBIRD_PLUGIN_API_V1_MAJOR)
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    descriptor->id = StringView("cabbird.god-mode"); descriptor->name = StringView("God Mode");
    descriptor->author = StringView("Cabbird"); descriptor->version = StringView("1.0.0");
    descriptor->on_load = Load; descriptor->on_start = Start; descriptor->on_stop = Stop;
    descriptor->on_unload = Unload; descriptor->on_update = Update; descriptor->on_draw = nullptr;
    return Ok();
}

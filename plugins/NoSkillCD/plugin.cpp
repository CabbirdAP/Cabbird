// plugin.cpp -- No Skill Cooldown for Azur Promilia (Cabbird built-in).
//
// Clears skill cooldowns and charge for the controlled character (E/R) and the pet
// (Q).  Enabled/disabled by the host via on_start / on_stop; no UI.
//
// Targets are resolved per frame from cabbird.unity.entities: the host's entity walk
// already classifies each entity, so the filter matches the data-class string and the
// managed object address comes from lookup.data.  Classes and methods are looked up
// by name; no RVA is hard-coded.
//
// DOMAIN: every IL2CPP call below runs inside on_update's with_runtime callback
// (Game domain).  on_draw is a no-op.

#include "cabbird/sdk/cabbird_sdk.h"
#include "cabbird/sdk/cpp.hpp"

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

namespace {

using cabbird::sdk::StringView;
using cabbird::sdk::Ok;

// Verified symbols, namespace Lens.Gameplay.Modules.BigWorld (AzurPromilia dump.cs):
//   SkillCDManager.ClearAllSkillCoolDown(AliveData)   static, argc=1   dump.cs:361631
//   SkillProperty.get_skills()                        argc=0           dump.cs:271695
//   Skill.get_coolDown()                              argc=0           dump.cs:271241
//   CoolDown.set_currentChargeCount(Int32)            argc=1           dump.cs:271024
//   CoolDown.chargeMaxCount (field @ 0x14)                              dump.cs:270999
constexpr const char* kNs = "Lens.Gameplay.Modules.BigWorld";
constexpr const char* kClsSkillCDManager = "SkillCDManager";
constexpr const char* kClsSkillProperty  = "SkillProperty";
constexpr const char* kClsCoolDown       = "CoolDown";
constexpr const char* kMClearAll         = "ClearAllSkillCoolDown"; // static (AliveData)
constexpr const char* kMGetSkillProperty = "get_skillProperty";
constexpr const char* kMGetSkills        = "get_skills";
constexpr const char* kMGetCoolDown      = "get_coolDown";
constexpr const char* kMSetCurrentCharge = "set_currentChargeCount"; // (Int32)
constexpr const char* kFChargeMax        = "chargeMaxCount";

// Standard IL2CPP List<T> (class) and T[] array layout.
constexpr uintptr_t kListItems = 0x10; // T[] _items
constexpr uintptr_t kListSize  = 0x18; // int  _size (4 bytes)
constexpr uintptr_t kArrayData = 0x20; // array elements begin here (8-byte pointers)

// Never trust a pointer: only dereference values in a sane user-mode range.
inline bool ptr_ok(uintptr_t p) {
    return p > 0x10000 && p < 0x00007FFFFFFFFFFFull;
}

struct Ctx {
    const CabbirdCoreServiceV1* core = nullptr;
    const CabbirdIl2CppServiceV1* il2cpp = nullptr;
    const CabbirdUnityEntitiesServiceV1* entities = nullptr;

    // Written in on_start/on_stop, read in Game.
    std::atomic<bool> enabled{false};

    // Game-domain cached metadata (runtime-owned; safe across frames).
    CabbirdIl2CppClassV1* cls_skill_cd_mgr = nullptr;
    CabbirdIl2CppClassV1* cls_skill_property = nullptr;
    CabbirdIl2CppClassV1* cls_cool_down = nullptr;
    const CabbirdMethodInfoV1* m_clear_all = nullptr;
    const CabbirdMethodInfoV1* m_get_skill_property = nullptr;
    const CabbirdMethodInfoV1* m_get_skills = nullptr;
    const CabbirdMethodInfoV1* m_set_current_charge = nullptr;
    const CabbirdMethodInfoV1* m_get_cool_down = nullptr; // resolved on first skill
    CabbirdFieldInfoV1* f_charge_max = nullptr;
    std::atomic<bool> metadata_ok{false};

    int32_t logged_error = -1;
    bool logged_entities = false;
};

void logv(const CabbirdCoreServiceV1* core, uint32_t level, const char* fmt, ...) {
    if (!core || !core->log) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    core->log(core->user, level, StringView(buf));
}

bool rd_u64(const CabbirdCoreServiceV1* core, uintptr_t addr, uint64_t* out) {
    if (!core || !core->read_memory || !out) return false;
    CabbirdMutableByteSpanV1 dst{reinterpret_cast<uint8_t*>(out), 8};
    return core->read_memory(core->user, addr, dst).code == CABBIRD_STATUS_V1_OK;
}

bool rd_u32(const CabbirdCoreServiceV1* core, uintptr_t addr, uint32_t* out) {
    if (!core || !core->read_memory || !out) return false;
    CabbirdMutableByteSpanV1 dst{reinterpret_cast<uint8_t*>(out), 4};
    return core->read_memory(core->user, addr, dst).code == CABBIRD_STATUS_V1_OK;
}

CabbirdIl2CppClassV1* find_class(const CabbirdIl2CppApiV1* api, const char* ns, const char* name) {
    if (!api || !api->il2cpp_domain_get || !api->il2cpp_domain_get_assemblies ||
        !api->il2cpp_assembly_get_image || !api->il2cpp_class_from_name) {
        return nullptr;
    }
    CabbirdIl2CppDomainV1* domain = api->il2cpp_domain_get();
    if (!domain) return nullptr;
    size_t count = 0;
    const CabbirdIl2CppAssemblyV1** assemblies = api->il2cpp_domain_get_assemblies(domain, &count);
    if (!assemblies || count == 0) return nullptr;
    for (size_t i = 0; i < count; ++i) {
        if (!assemblies[i]) continue;
        CabbirdIl2CppImageV1* image = api->il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        CabbirdIl2CppClassV1* klass = api->il2cpp_class_from_name(image, ns, name);
        if (klass) return klass;
    }
    return nullptr;
}

bool resolve_metadata(const CabbirdIl2CppApiV1* api, Ctx* ctx) {
    if (!api || !api->il2cpp_class_get_method_from_name || !api->il2cpp_runtime_invoke) {
        if (ctx->logged_error != 1) { logv(ctx->core, 2, "nsc: il2cpp api missing methods"); ctx->logged_error = 1; }
        return false;
    }
    ctx->cls_skill_cd_mgr = find_class(api, kNs, kClsSkillCDManager);
    ctx->m_clear_all = ctx->cls_skill_cd_mgr
        ? api->il2cpp_class_get_method_from_name(ctx->cls_skill_cd_mgr, kMClearAll, 1)
        : nullptr;
    if (!ctx->m_clear_all) {
        if (ctx->logged_error != 3) { logv(ctx->core, 2, "nsc: ClearAllSkillCoolDown not found"); ctx->logged_error = 3; }
        return false;
    }
    // Charge fill (SkillProperty -> Skill -> CoolDown).
    ctx->cls_skill_property = find_class(api, kNs, kClsSkillProperty);
    ctx->cls_cool_down = find_class(api, kNs, kClsCoolDown);
    if (ctx->cls_skill_property) {
        ctx->m_get_skill_property = nullptr; // resolved lazily on the entity's runtime class
        ctx->m_get_skills = api->il2cpp_class_get_method_from_name(ctx->cls_skill_property, kMGetSkills, 0);
    }
    if (ctx->cls_cool_down) {
        ctx->m_set_current_charge = api->il2cpp_class_get_method_from_name(ctx->cls_cool_down, kMSetCurrentCharge, 1);
        ctx->f_charge_max = api->il2cpp_class_get_field_from_name(ctx->cls_cool_down, kFChargeMax);
    }
    ctx->metadata_ok.store(true, std::memory_order_relaxed);
    if (ctx->logged_error != 0) { logv(ctx->core, 1, "nsc: metadata ready"); ctx->logged_error = 0; }
    return true;
}

// Resolve an instance method on the object's own runtime class.
const CabbirdMethodInfoV1* resolve_instance_method(
    const CabbirdIl2CppApiV1* api, uint64_t obj, const char* name) {
    if (!api || !obj || !api->il2cpp_object_get_class || !api->il2cpp_class_get_method_from_name) return nullptr;
    CabbirdIl2CppClassV1* klass = api->il2cpp_object_get_class(reinterpret_cast<CabbirdIl2CppObjectV1*>(obj));
    if (!klass) return nullptr;
    return api->il2cpp_class_get_method_from_name(klass, name, 0);
}

// Clear one entity's skill cooldown via the game's own static helper.
bool clear_one(const CabbirdIl2CppApiV1* api, Ctx* ctx, uint64_t data) {
    if (!ptr_ok(data)) return false;
    void* args[1] = {reinterpret_cast<void*>(data)};
    CabbirdIl2CppObjectV1* exception = nullptr;
    api->il2cpp_runtime_invoke(ctx->m_clear_all, nullptr, args, &exception);
    return exception == nullptr;
}

// Fill the entity's skill charge (set_currentChargeCount = chargeMaxCount per skill).
void fill_charge(const CabbirdIl2CppApiV1* api, Ctx* ctx, uint64_t aliveData) {
    if (!ctx->m_get_skills || !ctx->m_set_current_charge || !ctx->f_charge_max) return;
    if (!ctx->m_get_skill_property) {
        ctx->m_get_skill_property = resolve_instance_method(api, aliveData, kMGetSkillProperty);
    }
    if (!ctx->m_get_skill_property) return;
    CabbirdIl2CppObjectV1* exc = nullptr;
    CabbirdIl2CppObjectV1* skillProp = api->il2cpp_runtime_invoke(
        ctx->m_get_skill_property, reinterpret_cast<CabbirdIl2CppObjectV1*>(aliveData), nullptr, &exc);
    if (exc || !skillProp) return;
    CabbirdIl2CppObjectV1* skills = api->il2cpp_runtime_invoke(ctx->m_get_skills, skillProp, nullptr, &exc);
    if (exc || !skills) return;
    uint64_t items = 0;
    uint32_t size = 0;
    if (!rd_u64(ctx->core, reinterpret_cast<uintptr_t>(skills) + kListItems, &items) || !ptr_ok(items)) return;
    if (!rd_u32(ctx->core, reinterpret_cast<uintptr_t>(skills) + kListSize, &size)) return;
    if (size == 0 || size > 64) return;
    for (uint32_t i = 0; i < size; ++i) {
        uint64_t skill = 0;
        if (!rd_u64(ctx->core, items + kArrayData + static_cast<uint64_t>(i) * 8, &skill) || !ptr_ok(skill)) continue;
        if (!ctx->m_get_cool_down) {
            CabbirdIl2CppClassV1* skill_klass = api->il2cpp_object_get_class(
                reinterpret_cast<CabbirdIl2CppObjectV1*>(skill));
            if (skill_klass) {
                ctx->m_get_cool_down = api->il2cpp_class_get_method_from_name(skill_klass, kMGetCoolDown, 0);
            }
        }
        if (!ctx->m_get_cool_down) continue;
        CabbirdIl2CppObjectV1* coolDown = api->il2cpp_runtime_invoke(
            ctx->m_get_cool_down, reinterpret_cast<CabbirdIl2CppObjectV1*>(skill), nullptr, &exc);
        if (exc || !coolDown) { exc = nullptr; continue; }
        int32_t cmax = 0;
        api->il2cpp_field_get_value(coolDown, ctx->f_charge_max, &cmax);
        int32_t value = cmax;
        void* args[1] = {&value};
        api->il2cpp_runtime_invoke(ctx->m_set_current_charge, coolDown, args, &exc);
        exc = nullptr;
    }
}

// Enumerate the entity source and fill the controlled character, heroes and the pet.
// Each target is identified by its data-class name via the entities `lookup`, and the
// managed object address comes from `lookup.data`.
void fill_all_entities(const CabbirdIl2CppApiV1* api, Ctx* ctx) {
    if (!ctx->entities || !ctx->entities->entity_count || !ctx->entities->entity_at ||
        !ctx->entities->lookup) return;
    const uint32_t count = ctx->entities->entity_count(ctx->entities->user);
    if (count == 0 || count > 4096) return;
    uint32_t cleared = 0;
    uint32_t pets = 0;
    for (uint32_t i = 0; i < count; ++i) {
        CabbirdUnityEntityV1 e{};
        e.struct_size = sizeof(e);
        if (ctx->entities->entity_at(ctx->entities->user, i, &e).code != CABBIRD_STATUS_V1_OK) continue;
        const uint64_t key = e.entity_id;
        if (!ptr_ok(key)) continue;
        CabbirdUnityEntitiesLookupV1 lk{};
        lk.struct_size = sizeof(lk);
        if (ctx->entities->lookup(ctx->entities->user, key, &lk).code != CABBIRD_STATUS_V1_OK) continue;
        if (!std::strstr(lk.data_class, "PlayerData") &&
            !std::strstr(lk.data_class, "HeroData") &&
            !std::strstr(lk.data_class, "Pet") &&
            !std::strstr(lk.data_class, "Kibo")) continue;
        // `lk.data` is the managed BaseData object's address.
        const uint64_t data = lk.data;
        if (!ptr_ok(data)) continue;
        ++pets;
        if (clear_one(api, ctx, data)) ++cleared;
        fill_charge(api, ctx, data);
    }
    if (!ctx->logged_entities) {
        logv(ctx->core, 1, "nsc: entities filled (%u entities, %u cleared)", pets, cleared);
        ctx->logged_entities = true;
    }
}

CabbirdStatusV1 CABBIRD_CALL runtime_tick(void* user, const CabbirdIl2CppApiV1* api) {
    Ctx* ctx = static_cast<Ctx*>(user);
    if (!ctx || !api) return Ok();

    if (!ctx->metadata_ok.load(std::memory_order_relaxed)) {
        if (!resolve_metadata(api, ctx)) return Ok();
    }
    if (!ctx->core || !ctx->core->read_memory) {
        if (ctx->logged_error != 5) { logv(ctx->core, 2, "nsc: core.read_memory missing"); ctx->logged_error = 5; }
        return Ok();
    }
    if (ctx->enabled.load(std::memory_order_relaxed)) {
        fill_all_entities(api, ctx);
    }
    return Ok();
}

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** context) {
    if (!host || !context) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    Ctx* ctx = new (std::nothrow) Ctx();
    if (!ctx) return {CABBIRD_STATUS_V1_FAILED, 0, {nullptr, 0}};
    *context = ctx;

    const cabbird::sdk::Host services(host);
    ctx->core = services.Query<CabbirdCoreServiceV1>(CABBIRD_CORE_SERVICE_V1_ID,
                                                      CABBIRD_CORE_SERVICE_V1_VERSION).get();
    ctx->il2cpp = services.Query<CabbirdIl2CppServiceV1>(CABBIRD_IL2CPP_SERVICE_V1_ID,
                                                         CABBIRD_IL2CPP_SERVICE_V1_VERSION).get();
    ctx->entities = services.Query<CabbirdUnityEntitiesServiceV1>(CABBIRD_UNITY_ENTITIES_SERVICE_V1_ID,
                                                                  CABBIRD_UNITY_ENTITIES_SERVICE_V1_VERSION).get();
    logv(ctx->core, 1, "nsc: load core=%d il2cpp=%d entities=%d",
         ctx->core ? 1 : 0, ctx->il2cpp ? 1 : 0, ctx->entities ? 1 : 0);
    return Ok();
}

CabbirdStatusV1 CABBIRD_CALL Start(void* context) {
    Ctx* ctx = static_cast<Ctx*>(context);
    if (ctx) ctx->enabled.store(true, std::memory_order_relaxed);
    return Ok();
}

CabbirdStatusV1 CABBIRD_CALL Stop(void* context, uint32_t) {
    Ctx* ctx = static_cast<Ctx*>(context);
    if (ctx) ctx->enabled.store(false, std::memory_order_relaxed);
    return Ok();
}

void CABBIRD_CALL Unload(void* context) {
    delete static_cast<Ctx*>(context);
}

void CABBIRD_CALL Update(void* context, double /*delta_seconds*/) {
    Ctx* ctx = static_cast<Ctx*>(context);
    if (!ctx || !ctx->il2cpp || !ctx->il2cpp->with_runtime) return;

    const CabbirdStatusV1 st = ctx->il2cpp->with_runtime(ctx->il2cpp->user, runtime_tick, ctx);
    if (st.code != CABBIRD_STATUS_V1_OK) {
        if (ctx->logged_error != 100 + (int)st.code) {
            logv(ctx->core, 2, "nsc: with_runtime status=%d", (int)st.code);
            ctx->logged_error = 100 + (int)st.code;
        }
    }
}

void CABBIRD_CALL Draw(void* /*context*/, const CabbirdUiServiceV1* /*ui*/) {
    // No UI window: the effect follows the host's enable/disable of this plugin.
}

}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }
    *descriptor = {
        sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        StringView("cabbird.no-skill-cd"),
        StringView("No Skill Cooldown"),
        StringView("NEKONIKO / Cabbird"),
        StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return Ok();
}

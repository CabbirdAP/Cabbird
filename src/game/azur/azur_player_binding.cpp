#include "cabbird/azur_player_binding.hpp"

#include "cabbird/il2cpp.hpp"
#include "cabbird/memory.hpp"
#include "cabbird/hook_manager.hpp"
#include "cabbird/unity_build_profile.hpp"

#include <Windows.h>

#include <cstring>
#include <cstdio>
#include <initializer_list>
#include <utility>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace cabbird::azur {
namespace {

constexpr std::string_view kWorldNamespace = "Lens.Gameplay.Modules.BigWorld";

il2cpp::Il2CppClass* WorldClass(std::string_view name) {
    return il2cpp::FindClass("Assembly-CSharp.dll", kWorldNamespace, name);
}

std::optional<std::size_t> VerifiedField(il2cpp::Il2CppClass* klass,
    std::string_view name, std::string_view type, std::size_t size) {
    const auto field = il2cpp::ResolveInstanceField(klass, name, size);
    if (!field.offset || il2cpp::TypeName(field.type) != type) return std::nullopt;
    return field.offset;
}

// Keep SEH in a POD-only frame. The getter can encounter a world being streamed out
// after the singleton check, and that must remain an unavailable snapshot.
bool InvokeControllingEntity(const il2cpp::Api& api, const il2cpp::MethodInfo* method,
                             std::uintptr_t* handle) noexcept {
    __try {
        il2cpp::Il2CppObject* exception = nullptr;
        const auto* result = api.il2cpp_runtime_invoke(method, nullptr, nullptr, &exception);
        if (exception != nullptr || result == nullptr) return false;
        *handle = reinterpret_cast<std::uintptr_t>(result);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

const il2cpp::MethodInfo* MotionMethod(const il2cpp::Api& api, il2cpp::Il2CppClass* klass,
    const char* name, std::string_view result, std::initializer_list<std::string_view> parameters,
    bool is_static = false) {
    const il2cpp::MethodInfo* selected{};
    void* iterator{};
    while (const auto* method = api.il2cpp_class_get_methods(klass, &iterator)) {
        const auto* candidate = api.il2cpp_method_get_name(method);
        std::uint32_t implementation{};
        if (!candidate || std::strcmp(candidate, name) || api.il2cpp_method_get_param_count(method) != parameters.size() ||
            (((api.il2cpp_method_get_flags(method, &implementation) & 0x10u) != 0) != is_static) ||
            il2cpp::TypeName(api.il2cpp_method_get_return_type(method)) != result) continue;
        std::uint32_t index{};
        bool matches = true;
        for (const auto parameter : parameters)
            if (il2cpp::TypeName(api.il2cpp_method_get_param(method, index++)) != parameter) matches = false;
        if (!matches) continue;
        if (selected) return nullptr;
        selected = method;
    }
    return selected;
}

struct MotionVector3 { float x{}, y{}, z{}; };
static_assert(sizeof(MotionVector3) == 12);

struct TeleportObservation {
    std::uintptr_t transform{};
    MotionVector3 before{}, after{}, input_after{};
    bool have_before{}, have_after{};
    std::int32_t interpolation{};
};

bool InvokeTeleport(const il2cpp::Api& api, const PlayerMotionBinding& binding,
    std::uintptr_t entity, const double position[3], TeleportObservation& observed) noexcept {
    il2cpp::Il2CppGCHandle root{};
    bool restore_interpolation{};
    bool success{};
    __try {
        __try {
            auto* object = reinterpret_cast<il2cpp::Il2CppObject*>(entity);
            if (!api.il2cpp_class_is_assignable_from(reinterpret_cast<il2cpp::Il2CppClass*>(binding.alive_class),
                    api.il2cpp_object_get_class(object))) __leave;
            il2cpp::Il2CppObject* exception{};
            auto* movement = api.il2cpp_runtime_invoke(reinterpret_cast<const il2cpp::MethodInfo*>(binding.get_movement),
                object, nullptr, &exception);
            if (exception || !movement || !api.il2cpp_class_is_assignable_from(
                    reinterpret_cast<il2cpp::Il2CppClass*>(binding.movement_class), api.il2cpp_object_get_class(movement))) __leave;
            root = api.il2cpp_gchandle_new(movement, false);
            if (!root) __leave;
            auto* transform = api.il2cpp_runtime_invoke(reinterpret_cast<const il2cpp::MethodInfo*>(binding.get_transform),
                movement, nullptr, &exception);
            if (exception || !transform) __leave;
            observed.transform = reinterpret_cast<std::uintptr_t>(transform);
            auto* before = api.il2cpp_runtime_invoke(reinterpret_cast<const il2cpp::MethodInfo*>(binding.get_position),
                movement, nullptr, &exception);
            if (exception || !before) __leave;
            const auto* before_value = api.il2cpp_object_unbox(before);
            if (!before_value) __leave;
            std::memcpy(&observed.before, before_value, sizeof(observed.before));
            observed.have_before = true;
            auto* interpolation = api.il2cpp_runtime_invoke(
                reinterpret_cast<const il2cpp::MethodInfo*>(binding.get_interpolation), movement, nullptr, &exception);
            if (exception || !interpolation) __leave;
            const auto* interpolation_value = api.il2cpp_object_unbox(interpolation);
            if (!interpolation_value) __leave;
            std::memcpy(&observed.interpolation, interpolation_value, sizeof(observed.interpolation));
            // Character.TeleportPosition(..., interpolating:true, ...) brackets
            // SetPosition with Rigidbody interpolation off/on. Preserve the
            // actual previous enum instead of forcing an initially un-interpolated body on.
            std::int32_t no_interpolation{};
            void* interpolation_arguments[]{&no_interpolation};
            restore_interpolation = true;
            api.il2cpp_runtime_invoke(reinterpret_cast<const il2cpp::MethodInfo*>(binding.set_interpolation),
                movement, interpolation_arguments, &exception);
            if (exception) __leave;
            MotionVector3 zero{};
            void* velocity[]{&zero};
            api.il2cpp_runtime_invoke(reinterpret_cast<const il2cpp::MethodInfo*>(binding.set_velocity),
                api.il2cpp_gchandle_get_target(root), velocity, &exception);
            if (exception) __leave;
            api.il2cpp_runtime_invoke(reinterpret_cast<const il2cpp::MethodInfo*>(binding.clear_forces),
                api.il2cpp_gchandle_get_target(root), nullptr, &exception);
            if (exception) __leave;
            MotionVector3 target{static_cast<float>(position[0]), static_cast<float>(position[1]), static_cast<float>(position[2])};
            bool update_ground = false;
            void* arguments[]{&target, &update_ground};
            api.il2cpp_runtime_invoke(reinterpret_cast<const il2cpp::MethodInfo*>(binding.set_position),
                api.il2cpp_gchandle_get_target(root), arguments, &exception);
            if (exception) __leave;
            observed.input_after = target;
            auto* after = api.il2cpp_runtime_invoke(reinterpret_cast<const il2cpp::MethodInfo*>(binding.get_position),
                api.il2cpp_gchandle_get_target(root), nullptr, &exception);
            if (exception || !after) __leave;
            const auto* after_value = api.il2cpp_object_unbox(after);
            if (!after_value) __leave;
            std::memcpy(&observed.after, after_value, sizeof(observed.after));
            observed.have_after = true;
            success = true;
        } __finally {
            if (restore_interpolation) {
                il2cpp::Il2CppObject* exception{};
                void* arguments[]{&observed.interpolation};
                api.il2cpp_runtime_invoke(reinterpret_cast<const il2cpp::MethodInfo*>(binding.set_interpolation),
                    api.il2cpp_gchandle_get_target(root), arguments, &exception);
                if (exception) success = false;
            }
            if (root) api.il2cpp_gchandle_free(root);
        }
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    return success;
}

}  // namespace

namespace {
constexpr auto kMotionOwner = "cabbird.unity.player-motion";
using NativeMove = std::int32_t(__fastcall*)(il2cpp::Il2CppObject*, const MotionVector3*, float, const il2cpp::MethodInfo*);
using NativeTick = void(__fastcall*)(il2cpp::Il2CppObject*, float, const il2cpp::MethodInfo*);
using NativeGround = void(__fastcall*)(il2cpp::Il2CppObject*, bool, const il2cpp::MethodInfo*);
std::atomic<std::shared_ptr<PlayerMotionState>> g_motion_owner;

// The only game-field changes are bounded by a native call's finally block.
// No flag survives the callback, so release/stop need no Game-domain restoration.
std::int32_t GuardMotionMove(const il2cpp::Api& api, il2cpp::FieldInfo* ground_field,
    NativeGround ground_setter, const il2cpp::MethodInfo* setter_method, NativeMove original,
    il2cpp::Il2CppObject* self, const MotionVector3* velocity, float delta,
    const il2cpp::MethodInfo* method) {
    bool previous{};
    api.il2cpp_field_get_value(self, ground_field, &previous);
    __try {
        ground_setter(self, false, setter_method);
        return original(self, velocity, delta, method);
    } __finally { ground_setter(self, previous, setter_method); }
}
void GuardRecoveryTick(const il2cpp::Api& api, il2cpp::FieldInfo* field,
    NativeTick original, il2cpp::Il2CppObject* self, float delta, const il2cpp::MethodInfo* method) {
    bool previous{}, disabled{};
    api.il2cpp_field_get_value(self, field, &previous);
    __try {
        api.il2cpp_field_set_value(self, field, &disabled);
        original(self, delta, method);
    } __finally { api.il2cpp_field_set_value(self, field, &previous); }
}
}

struct PlayerMotionState {
    struct Intent {
        CabbirdUnityPlayerMotionRequestV1 request{};
        ControlledPlayer player{};
        std::uintptr_t movement{}, component{};
        std::uint64_t order{};
    };
    std::unique_ptr<HookManager> hooks;
    std::uintptr_t move_entry{}, tick_entry{};
    NativeMove original_move{};
    NativeTick original_tick{};
    NativeGround set_ground{};
    const il2cpp::MethodInfo *ground_method{}, *get_movement{}, *get_manager{};
    il2cpp::FieldInfo *manager_controller{}, *controller_player{}, *check{}, *ground{};
    std::size_t movement_id{};
    PlayerBinding identity;
    std::uint64_t generation{}, next_id{1}, next_order{1};
    DWORD game_thread{};
    std::mutex mutex;
    std::unordered_map<std::uint64_t, Intent> intents;

    bool Target(Intent& intent, std::string& reason) {
        if (!ResolveControlledPlayer(identity, intent.player, reason)) return false;
        if (intent.player.entity_id != intent.request.entity_id) {
            reason = "motion request does not name the current controlled player"; return false;
        }
        const auto& api = il2cpp::Functions();
        il2cpp::Il2CppObject* exception{};
        auto* movement = api.il2cpp_runtime_invoke(get_movement,
            reinterpret_cast<il2cpp::Il2CppObject*>(intent.player.entity), nullptr, &exception);
        if (exception || !movement) { reason = "controlled movement is unavailable"; return false; }
        const auto game_id = mem::Read<std::int32_t>(intent.player.data + identity.data_id);
        const auto controller_id = mem::Read<std::int32_t>(reinterpret_cast<std::uintptr_t>(movement) + movement_id);
        if (!game_id || !controller_id || *game_id != *controller_id) {
            reason = "controlled movement identity does not match its entity"; return false;
        }
        intent.movement = reinterpret_cast<std::uintptr_t>(movement);
        if (intent.request.mode != CABBIRD_UNITY_PLAYER_MOTION_V1_OVERRIDE_VELOCITY) {
            auto* manager = api.il2cpp_runtime_invoke(get_manager, nullptr, nullptr, &exception);
            il2cpp::Il2CppObject *controller{}, *component{};
            if (exception || !manager) { reason = "local player position manager is unavailable"; return false; }
            api.il2cpp_field_get_value(manager, manager_controller, &controller);
            if (controller) api.il2cpp_field_get_value(controller, controller_player, &component);
            if (!component) { reason = "local player position component is unavailable"; return false; }
            intent.component = reinterpret_cast<std::uintptr_t>(component);
        }
        return true;
    }
    bool Select(std::uintptr_t self, bool tick, Intent& selected) {
        if (GetCurrentThreadId() != game_thread) return false;
        bool found{};
        {
            std::scoped_lock lock(mutex);
            for (const auto& [id, candidate] : intents) {
                const auto mode = candidate.request.mode;
                if (tick ? candidate.component != self : candidate.movement != self || mode == 0) continue;
                if (!found || (mode == CABBIRD_UNITY_PLAYER_MOTION_V1_HOLD_POSITION &&
                        selected.request.mode != CABBIRD_UNITY_PLAYER_MOTION_V1_HOLD_POSITION) ||
                    (mode == selected.request.mode && candidate.order > selected.order)) {
                    selected = candidate; found = true;
                }
            }
        }
        if (!found) return false;
        ControlledPlayer current;
        std::string reason;
        if (!ResolveControlledPlayer(identity, current, reason) || current.entity != selected.player.entity ||
            current.data != selected.player.data || current.object_index != selected.player.object_index ||
            current.object_version != selected.player.object_version) {
            std::scoped_lock lock(mutex);
            std::erase_if(intents, [&](const auto& entry) { return entry.second.player.data == selected.player.data; });
            return false;
        }
        // Revalidate the entity's actual movement, since changing a character's
        // controller must not make a retained request select another instance.
        il2cpp::Il2CppObject* exception{};
        auto* movement = il2cpp::Functions().il2cpp_runtime_invoke(get_movement,
            reinterpret_cast<il2cpp::Il2CppObject*>(current.entity), nullptr, &exception);
        if (exception || reinterpret_cast<std::uintptr_t>(movement) != selected.movement) return false;
        if (tick) {
            auto* manager = il2cpp::Functions().il2cpp_runtime_invoke(get_manager, nullptr, nullptr, &exception);
            il2cpp::Il2CppObject *controller{}, *component{};
            if (exception || !manager) return false;
            il2cpp::Functions().il2cpp_field_get_value(manager, manager_controller, &controller);
            if (controller) il2cpp::Functions().il2cpp_field_get_value(controller, controller_player, &component);
            if (reinterpret_cast<std::uintptr_t>(component) != self) return false;
        }
        return true;
    }
    static std::int32_t __fastcall Move(il2cpp::Il2CppObject* self, const MotionVector3* velocity,
        float delta, const il2cpp::MethodInfo* method) noexcept {
        const auto state = g_motion_owner.load(std::memory_order_acquire);
        if (!state || !state->original_move) return 0;
        auto lease = state->hooks->AcquireCallback(kMotionOwner, state->generation, reinterpret_cast<void*>(state->move_entry));
        Intent intent;
        bool selected{};
        try { selected = lease && state->Select(reinterpret_cast<std::uintptr_t>(self), false, intent); }
        catch (...) { selected = false; }
        try {
            if (selected && intent.request.mode == CABBIRD_UNITY_PLAYER_MOTION_V1_HOLD_POSITION) return 0;
            MotionVector3 override_velocity;
            if (selected) {
                override_velocity = {static_cast<float>(intent.request.velocity[0]),
                    static_cast<float>(intent.request.velocity[1]), static_cast<float>(intent.request.velocity[2])};
                velocity = &override_velocity;
                if (intent.request.flags & CABBIRD_UNITY_PLAYER_MOTION_V1_FREE_GROUND)
                    return GuardMotionMove(il2cpp::Functions(), state->ground, state->set_ground,
                        state->ground_method, state->original_move, self, velocity, delta, method);
            }
            return state->original_move(self, velocity, delta, method);
        } catch (...) { return 0; }
    }
    static void __fastcall Tick(il2cpp::Il2CppObject* self, float delta, const il2cpp::MethodInfo* method) noexcept {
        const auto state = g_motion_owner.load(std::memory_order_acquire);
        if (!state || !state->original_tick) return;
        auto lease = state->hooks->AcquireCallback(kMotionOwner, state->generation, reinterpret_cast<void*>(state->tick_entry));
        Intent intent;
        bool selected{};
        try { selected = lease && state->Select(reinterpret_cast<std::uintptr_t>(self), true, intent); }
        catch (...) { selected = false; }
        try {
            if (selected) GuardRecoveryTick(il2cpp::Functions(), state->check, state->original_tick, self, delta, method);
            else state->original_tick(self, delta, method);
        } catch (...) {}
    }
};

bool PlayerMotionCoordinator::Acquire(const UnityBuildProfile& profile, std::uint64_t generation,
    const CabbirdUnityPlayerMotionRequestV1& request, CabbirdGenerationHandleV1& handle, std::string& reason) {
    auto active = state_.load(std::memory_order_acquire);
    if (!active) {
        const auto move = profile.ResolveInGame("azur.player.motion.move");
        const auto tick = profile.ResolveInGame("azur.player.position.live_tick");
        const auto ground = profile.ResolveInGame("azur.player.motion.set_ground");
        if (!move.resolved || !tick.resolved || !ground.resolved) {
            reason = "native player motion profile is unavailable: " + move.how + "; " + tick.how + "; " + ground.how;
            return false;
        }
        const auto& api = il2cpp::Functions();
        auto state = std::make_shared<PlayerMotionState>();
        auto* alive = WorldClass("AliveEntity");
        auto* movement = il2cpp::FindClass("Assembly-CSharp.dll",
            "Lens.Gameplay.Modules.BigWorld.CharacterMovement", "CharacterMovement");
        auto* world = il2cpp::FindClass("Assembly-CSharp.dll", "Lens.Gameplay.Modules.Azur", "AzurWorld");
        auto* manager = il2cpp::FindClass("Assembly-CSharp.dll", "Azur.Gameplay.PlayerModule", "PositionManager");
        auto* controller = il2cpp::FindClass("Assembly-CSharp.dll", "Azur.Gameplay.PlayerModule", "PositionController");
        auto* component = il2cpp::FindClass("Assembly-CSharp.dll", "Azur.Gameplay.PlayerModule", "MyPlayerPositionComponent");
        if (!alive || !movement || !world || !manager || !controller || !component ||
            !api.il2cpp_field_get_value || !api.il2cpp_field_set_value) {
            reason = "native motion metadata APIs/classes are unavailable"; return false;
        }
        state->get_movement = MotionMethod(api, alive, "get_characterMovement",
            "Lens.Gameplay.Modules.BigWorld.CharacterMovement.CharacterMovement", {});
        state->get_manager = MotionMethod(api, world, "get_positionMgr", "Azur.Gameplay.PlayerModule.PositionManager", {}, true);
        state->ground_method = MotionMethod(api, movement, "set_constrainToGround", "System.Void", {"System.Boolean"});
        const auto controller_field = VerifiedField(manager, "m_Controller", "Azur.Gameplay.PlayerModule.PositionController", sizeof(void*));
        const auto player_field = VerifiedField(controller, "<MyPlayer>k__BackingField", "Azur.Gameplay.PlayerModule.MyPlayerPositionComponent", sizeof(void*));
        const auto check_field = VerifiedField(component, "m_check", "System.Boolean", 1);
        const auto ground_field = VerifiedField(movement, "_isConstrainedToGround", "System.Boolean", 1);
        const auto movement_id = VerifiedField(movement, "m_entityid", "System.Int32", sizeof(std::int32_t));
        if (!state->get_movement || !state->get_manager || !state->ground_method || !controller_field || !player_field ||
            !check_field || !ground_field || !movement_id) { reason = "native motion metadata shape is incompatible"; return false; }
        state->manager_controller = il2cpp::FindField(manager, "m_Controller");
        state->controller_player = il2cpp::FindField(controller, "<MyPlayer>k__BackingField");
        state->check = il2cpp::FindField(component, "m_check");
        state->ground = il2cpp::FindField(movement, "_isConstrainedToGround");
        state->movement_id = *movement_id;
        state->set_ground = reinterpret_cast<NativeGround>(ground.address);
        state->move_entry = move.address; state->tick_entry = tick.address;
        state->generation = generation; state->game_thread = GetCurrentThreadId();
        state->hooks = std::make_unique<HookManager>(CreateMinHookBackend());
        state_.store(state, std::memory_order_release);
        active = state;
        g_motion_owner.store(state, std::memory_order_release);
        if (!state->hooks->Create(kMotionOwner, generation, "player-motion.move", reinterpret_cast<void*>(move.address),
                reinterpret_cast<void*>(&PlayerMotionState::Move), reinterpret_cast<void**>(&state->original_move)) ||
            !state->hooks->Create(kMotionOwner, generation, "player-motion.recovery", reinterpret_cast<void*>(tick.address),
                reinterpret_cast<void*>(&PlayerMotionState::Tick), reinterpret_cast<void**>(&state->original_tick)) ||
            !state->hooks->EnableOwner(kMotionOwner, generation)) {
            reason = "shared native player motion hooks could not be installed";
            static_cast<void>(Stop(std::chrono::seconds(5))); return false;
        }
    }
    PlayerMotionState::Intent intent;
    intent.request = request;
    if (!active->Target(intent, reason)) return false;
    std::scoped_lock lock(active->mutex);
    std::erase_if(active->intents, [&](const auto& entry) {
        return entry.second.player.entity != intent.player.entity || entry.second.player.object_index != intent.player.object_index ||
            entry.second.player.object_version != intent.player.object_version;
    });
    const auto id = active->next_id++;
    intent.order = active->next_order++;
    active->intents.emplace(id, intent);
    handle = {id, active->generation};
    return true;
}
bool PlayerMotionCoordinator::Update(CabbirdGenerationHandleV1 handle,
    const CabbirdUnityPlayerMotionRequestV1& request, std::string& reason) {
    const auto active = state_.load(std::memory_order_acquire);
    if (!active || handle.generation != active->generation) return false;
    PlayerMotionState::Intent updated;
    updated.request = request;
    if (!active->Target(updated, reason)) return false;
    std::scoped_lock lock(active->mutex);
    const auto found = active->intents.find(handle.id);
    if (found == active->intents.end()) return false;
    const auto& old = found->second;
    if (old.player.entity != updated.player.entity || old.player.object_index != updated.player.object_index ||
        old.player.object_version != updated.player.object_version) return false;
    updated.order = active->next_order++;
    found->second = updated;
    return true;
}
bool PlayerMotionCoordinator::Release(CabbirdGenerationHandleV1 handle) noexcept {
    const auto active = state_.load(std::memory_order_acquire);
    if (!active || handle.generation != active->generation) return false;
    std::scoped_lock lock(active->mutex);
    return active->intents.erase(handle.id) != 0;
}
void PlayerMotionCoordinator::Invalidate() noexcept {
    const auto active = state_.load(std::memory_order_acquire);
    if (active) { std::scoped_lock lock(active->mutex); active->intents.clear(); }
}
bool PlayerMotionCoordinator::Stop(std::chrono::milliseconds timeout) noexcept {
    const auto state = state_.load(std::memory_order_acquire);
    if (!state) return true;
    Invalidate();
    if (!state->hooks->RemoveOwner(kMotionOwner, state->generation, timeout)) return false;
    auto expected = state;
    static_cast<void>(g_motion_owner.compare_exchange_strong(expected, {}));
    state_.store({}, std::memory_order_release);
    return true;
}
bool PlayerMotionCoordinator::BeginRecovery(const UnityBuildProfile& profile, std::uint64_t generation,
    std::uint64_t entity_id, CabbirdGenerationHandleV1& handle, std::string& reason) {
    CabbirdUnityPlayerMotionRequestV1 request{sizeof(request)};
    request.entity_id = entity_id;
    // Internal recovery-only intent never selects the Move branch.
    return Acquire(profile, generation, request, handle, reason);
}

bool ResolveControlledPlayer(
    PlayerBinding& binding, ControlledPlayer& player, std::string& reason) {
    player = {};
    const auto& api = il2cpp::Functions();
    if (!il2cpp::Ready()) {
        reason = "IL2CPP runtime is unavailable: " + il2cpp::LastError();
        return false;
    }
    if (api.il2cpp_class_get_methods == nullptr || api.il2cpp_method_get_name == nullptr ||
        api.il2cpp_method_get_param_count == nullptr || api.il2cpp_method_get_return_type == nullptr ||
        api.il2cpp_method_get_flags == nullptr || api.il2cpp_object_get_class == nullptr ||
        api.il2cpp_class_is_assignable_from == nullptr ||
        api.il2cpp_runtime_invoke == nullptr || api.il2cpp_class_get_name == nullptr ||
        api.il2cpp_class_get_parent == nullptr || api.il2cpp_class_get_field_from_name == nullptr ||
        api.il2cpp_field_static_get_value == nullptr || api.il2cpp_domain_get == nullptr) {
        reason = "IL2CPP APIs for EntityManager.get_ControllingEntity are unavailable";
        return false;
    }
    if (api.il2cpp_domain_get() == nullptr) {
        reason = "IL2CPP domain has not been created yet";
        return false;
    }
    if (binding.getter == 0) {
        auto* manager_class = WorldClass("EntityManager");
        auto* handle_class = WorldClass("EntityHandle");
        auto* entity_class = WorldClass("Entity");
        auto* data_class = WorldClass("BaseData");
        if (!manager_class || !handle_class || !entity_class || !data_class) {
            reason = "EntityManager/EntityHandle/Entity/BaseData metadata is unavailable";
            return false;
        }
        auto* instance_field = il2cpp::FindField(manager_class, "p_instance");
        constexpr std::uint32_t kStaticFieldFlag = 0x10;
        if (instance_field == nullptr || api.il2cpp_field_get_flags == nullptr ||
            (api.il2cpp_field_get_flags(instance_field) & kStaticFieldFlag) == 0) {
            reason = "EntityManager.p_instance is not readable in this build";
            return false;
        }
        const il2cpp::MethodInfo* getter = nullptr;
        void* iterator = nullptr;
        while (const auto* method = api.il2cpp_class_get_methods(manager_class, &iterator)) {
            const auto* name = api.il2cpp_method_get_name(method);
            if (!name || std::strcmp(name, "get_ControllingEntity") != 0 ||
                api.il2cpp_method_get_param_count(method) != 0) continue;
            std::uint32_t implementation_flags = 0;
            if ((api.il2cpp_method_get_flags(method, &implementation_flags) & kStaticFieldFlag) == 0 ||
                il2cpp::TypeName(api.il2cpp_method_get_return_type(method)) !=
                    "Lens.Gameplay.Modules.BigWorld.EntityHandle" || getter != nullptr) {
                reason = "EntityManager.get_ControllingEntity metadata shape is incompatible or ambiguous";
                return false;
            }
            getter = method;
        }
        if (getter == nullptr) {
            reason = "EntityManager.get_ControllingEntity was not found";
            return false;
        }
        const auto handle_entity = VerifiedField(handle_class, "<entity>k__BackingField",
            "Lens.Gameplay.Modules.BigWorld.Entity", sizeof(void*));
        const auto entity_data = VerifiedField(entity_class, "data",
            "Lens.Gameplay.Modules.BigWorld.BaseData", sizeof(void*));
        const auto data_id = VerifiedField(data_class, "<entityId>k__BackingField", "System.Int32", sizeof(std::int32_t));
        const auto object_index = VerifiedField(data_class, "<objIndex>k__BackingField", "System.UInt32", sizeof(std::uint32_t));
        const auto object_version = VerifiedField(data_class, "<version>k__BackingField", "System.Int32", sizeof(std::int32_t));
        if (!handle_entity || !entity_data || !data_id || !object_index || !object_version) {
            reason = "controlled-player instance fields are not verified by live metadata";
            return false;
        }
        PlayerBinding resolved{reinterpret_cast<std::uintptr_t>(getter),
                   reinterpret_cast<std::uintptr_t>(instance_field),
                   *handle_entity, *entity_data, *data_id, *object_index, *object_version};
        // The authoritative controlling getter changes to the mount handle on
        // ride entry. Validate only the supported entity/data metadata pairs;
        // another AliveEntity in the world is not a controlled player binding.
        constexpr std::pair<std::string_view, std::string_view> controlled_types[]{
            {"PlayerEntity", "PlayerData"},
            {"HeroEntity", "HeroData"},
            {"MountEntity", "MountData"},
        };
        for (std::size_t index = 0; index < resolved.controlled_types.size(); ++index) {
            auto* expected_entity = WorldClass(controlled_types[index].first);
            auto* expected_data = WorldClass(controlled_types[index].second);
            if (!expected_entity || !expected_data ||
                !api.il2cpp_class_is_assignable_from(entity_class, expected_entity) ||
                !api.il2cpp_class_is_assignable_from(data_class, expected_data)) {
                reason = "controlled entity/data inheritance metadata is incompatible: " +
                    std::string(controlled_types[index].first) + "/" + std::string(controlled_types[index].second);
                return false;
            }
            resolved.controlled_types[index] = {reinterpret_cast<std::uintptr_t>(expected_entity),
                                               reinterpret_cast<std::uintptr_t>(expected_data)};
        }
        binding = resolved;
    }
    std::uintptr_t instance = 0;
    api.il2cpp_field_static_get_value(
        reinterpret_cast<il2cpp::FieldInfo*>(binding.instance_field), &instance);
    if (instance == 0) {
        reason = "the world is not loaded yet (EntityManager.p_instance is null)";
        return false;
    }
    std::uintptr_t handle = 0;
    if (!InvokeControllingEntity(api,
            reinterpret_cast<const il2cpp::MethodInfo*>(binding.getter), &handle)) {
        reason = "EntityManager.get_ControllingEntity is unavailable (world not ready or faulted)";
        return false;
    }
    const auto entity = mem::Read<std::uintptr_t>(handle + binding.handle_entity);
    if (!entity || *entity == 0) {
        reason = "EntityManager.get_ControllingEntity.entity is null or unreadable";
        return false;
    }
    const auto data = mem::Read<std::uintptr_t>(*entity + binding.entity_data);
    if (!data || *data == 0) {
        reason = "the controlled entity or its data is unreadable";
        return false;
    }
    auto* klass = api.il2cpp_object_get_class(reinterpret_cast<il2cpp::Il2CppObject*>(*data));
    auto* entity_klass = api.il2cpp_object_get_class(reinterpret_cast<il2cpp::Il2CppObject*>(*entity));
    if (!klass || !entity_klass) {
        reason = "the controlled data has no readable managed class";
        return false;
    }
    bool supported = false;
    for (const auto& expected : binding.controlled_types) {
        if (api.il2cpp_class_is_assignable_from(
                reinterpret_cast<il2cpp::Il2CppClass*>(expected.entity), entity_klass) &&
            api.il2cpp_class_is_assignable_from(
                reinterpret_cast<il2cpp::Il2CppClass*>(expected.data), klass)) {
            supported = true;
            break;
        }
    }
    if (!supported) {
        reason = "the controlled object is not a supported Player/Hero/Mount entity-data pair";
        return false;
    }
    // BaseData.entityId is shared by PlayerData/HeroData/MountData. HeroEntity has no
    // m_entityId field, so reading a guessed offset on the entity itself is invalid.
    const auto entity_id = mem::Read<std::int32_t>(*data + binding.data_id);
    if (!entity_id || *entity_id == 0) {
        reason = "controlled player data entityId is null or unreadable";
        return false;
    }
    const auto object_index = mem::Read<std::uint32_t>(*data + binding.data_object_index);
    const auto object_version = mem::Read<std::int32_t>(*data + binding.data_object_version);
    if (!object_index || !object_version) {
        reason = "controlled player object identity/version is unreadable";
        return false;
    }
    // SDK identities are the managed data object's address. The game's int32 id
    // belongs to gameplay APIs and must never replace this address identity.
    player = {*data, reinterpret_cast<std::uintptr_t>(klass), static_cast<std::uint64_t>(*data),
              instance, *object_index, *object_version};
    player.entity = *entity;
    reason.clear();
    return true;
}

bool TeleportControlledEntity(PlayerMotionBinding& binding,
    std::uintptr_t entity, std::uintptr_t observed_transform, const double position[3], std::string& reason) {
    const auto& api = il2cpp::Functions();
    if (!entity || !api.il2cpp_runtime_invoke || !api.il2cpp_gchandle_new ||
        !api.il2cpp_gchandle_get_target || !api.il2cpp_gchandle_free || !api.il2cpp_object_unbox) {
        reason = "controlled character movement runtime is unavailable";
        return false;
    }
    if (!binding.set_position) {
        auto* alive = WorldClass("AliveEntity");
        auto* movement = il2cpp::FindClass("Assembly-CSharp.dll",
            "Lens.Gameplay.Modules.BigWorld.CharacterMovement", "CharacterMovement");
        if (!alive || !movement) { reason = "controlled character movement metadata is unavailable"; return false; }
        const auto* getter = MotionMethod(api, alive, "get_characterMovement",
            "Lens.Gameplay.Modules.BigWorld.CharacterMovement.CharacterMovement", {});
        const auto* velocity = MotionMethod(api, movement, "set_velocity", "System.Void", {"UnityEngine.Vector3"});
        const auto* forces = MotionMethod(api, movement, "ClearAccumulatedForces", "System.Void", {});
        const auto* setter = MotionMethod(api, movement, "SetPosition", "System.Void", {"UnityEngine.Vector3", "System.Boolean"});
        const auto* transform = MotionMethod(api, movement, "get_transform", "UnityEngine.Transform", {});
        const auto* position_getter = MotionMethod(api, movement, "get_position", "UnityEngine.Vector3", {});
        const auto* interpolation_getter = MotionMethod(api, movement, "get_interpolation", "UnityEngine.RigidbodyInterpolation", {});
        const auto* interpolation_setter = MotionMethod(api, movement, "set_interpolation", "System.Void", {"UnityEngine.RigidbodyInterpolation"});
        if (!getter || !velocity || !forces || !setter || !transform || !position_getter ||
            !interpolation_getter || !interpolation_setter) {
            reason = "controlled character teleport method shapes are incompatible or ambiguous";
            return false;
        }
        PlayerMotionBinding resolved{reinterpret_cast<std::uintptr_t>(alive), reinterpret_cast<std::uintptr_t>(movement),
            reinterpret_cast<std::uintptr_t>(getter), reinterpret_cast<std::uintptr_t>(velocity),
            reinterpret_cast<std::uintptr_t>(forces), reinterpret_cast<std::uintptr_t>(setter),
            reinterpret_cast<std::uintptr_t>(transform), reinterpret_cast<std::uintptr_t>(position_getter),
            reinterpret_cast<std::uintptr_t>(interpolation_getter), reinterpret_cast<std::uintptr_t>(interpolation_setter)};
        binding = std::move(resolved);
    }
    TeleportObservation observed;
    const bool success = InvokeTeleport(api, binding, entity, position, observed);
    char diagnostics[512]{};
    std::snprintf(diagnostics, sizeof(diagnostics),
        "movement=%s sdkTransform=0x%llx movementTransform=0x%llx same=%u interpolation=%d "
        "movementBefore[%u]=(%.6f,%.6f,%.6f) movementAfter[%u]=(%.6f,%.6f,%.6f) inputAfter=(%.6f,%.6f,%.6f)",
        success ? "synchronized" : "failed", static_cast<unsigned long long>(observed_transform),
        static_cast<unsigned long long>(observed.transform), observed_transform == observed.transform ? 1u : 0u,
        observed.interpolation, observed.have_before ? 1u : 0u, observed.before.x, observed.before.y, observed.before.z,
        observed.have_after ? 1u : 0u, observed.after.x, observed.after.y, observed.after.z,
        observed.input_after.x, observed.input_after.y, observed.input_after.z);
    reason = diagnostics;
    return success;
}

bool ResolveDataTransformLayout(DataTransformLayout& layout, std::string& reason) {
    layout = {};
    const auto data_transform = VerifiedField(WorldClass("BaseData"), "<transform>k__BackingField",
        "Lens.Gameplay.Modules.BigWorld.RelativeTransform", sizeof(void*));
    const auto relative_transform = VerifiedField(WorldClass("RelativeTransform"), "m_transform",
        "UnityEngine.Transform", sizeof(void*));
    if (!data_transform || !relative_transform) {
        reason = "BaseData/RelativeTransform instance fields are not verified by live metadata";
        return false;
    }
    layout = {*data_transform, *relative_transform};
    reason.clear();
    return true;
}

}  // namespace cabbird::azur

#pragma once

#include "cabbird/sdk/cpp.hpp"
#include "cabbird/sdk/il2cpp.hpp"
#include "cabbird/sdk/services/unity_metadata.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace cabbird::plugins {
struct EntityIdentity final {
    std::int32_t gameId{}, version{};
    std::uint32_t objectIndex{};
    bool valid{};
    bool operator==(const EntityIdentity&) const = default;
};
using EntityIdentities = std::unordered_map<std::uint64_t, EntityIdentity>;

// Game-domain access through the public runtime facade. Only metadata handles
// and scalar identities survive a callback; managed objects are resolved afresh.
class EntityPositionAccess final {
   public:
    void Bind(const CabbirdHostApiV1* host) {
        const auto runtime = sdk::Host(host).Query<CabbirdIl2CppServiceV1>(CABBIRD_IL2CPP_SERVICE_V1_ID);
        const auto metadata = sdk::Host(host).Query<CabbirdUnityMetadataServiceV1>(
            CABBIRD_UNITY_METADATA_SERVICE_V1_ID);
        runtime_ = runtime && runtime->struct_size >= sizeof(*runtime.get()) && runtime->with_runtime
                       ? runtime.get() : nullptr;
        metadata_ = metadata && metadata->struct_size >= sizeof(*metadata.get()) &&
                            metadata->resolve_method && metadata->resolve_instance_field
                        ? metadata.get() : nullptr;
    }
    bool Available() const noexcept { return runtime_ && metadata_; }
    bool Ready() const noexcept { return ready_; }
    EntityIdentities Capture(const std::vector<std::uint64_t>& addresses) {
        EntityIdentities identities;
        Work work{this};
        work.addresses = &addresses;
        work.identities = &identities;
        Run(work);
        return identities;
    }
    bool Check(std::uint64_t data, const EntityIdentity& expected) {
        Work work{this};
        work.data = data;
        work.expected = &expected;
        return Run(work);
    }
    bool Write(std::uint64_t data, const EntityIdentity& expected,
               const std::array<double, 3>& position, const std::array<double, 3>* rotation) {
        Work work{this};
        work.data = data;
        work.expected = &expected;
        work.position = &position;
        work.rotation = rotation;
        return Run(work);
    }

   private:
    using Object = CabbirdIl2CppObjectV1;
    using Class = CabbirdIl2CppClassV1;
    using Method = CabbirdMethodInfoV1;
    using Field = CabbirdFieldInfoV1;
    using Runtime = sdk::il2cpp::Context;
    struct Vector3 { float x, y, z; };
    struct Quaternion { float x, y, z, w; };
    static_assert(sizeof(Vector3) == 12 && sizeof(Quaternion) == 16);
    struct Work {
        EntityPositionAccess* self;
        const std::vector<std::uint64_t>* addresses{};
        EntityIdentities* identities{};
        std::uint64_t data{};
        const EntityIdentity* expected{};
        const std::array<double, 3>* position{};
        const std::array<double, 3>* rotation{};
    };
    static CabbirdStatusV1 CABBIRD_CALL Entry(void* raw, const CabbirdIl2CppApiV1* api) noexcept {
        // A live-cache address is still a borrowed object. Contain native faults
        // at the runtime callback boundary as well as managed invocation errors.
        __try { return InvokeWork(raw, api); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return {CABBIRD_STATUS_V1_FAILED, 0, {}}; }
    }
    static CabbirdStatusV1 InvokeWork(void* raw, const CabbirdIl2CppApiV1* api) noexcept {
        try {
            if (!api || api->struct_size < sizeof(*api) || api->api_version != 1)
                return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
            auto& work = *static_cast<Work*>(raw);
            Runtime runtime(*api);
            return work.self->Execute(runtime, work) ? sdk::Ok()
                                                     : CabbirdStatusV1{CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
        } catch (...) { return {CABBIRD_STATUS_V1_FAILED, 0, {}}; }
    }
    bool Run(Work& work) {
        return Available() && sdk::Succeeded(runtime_->with_runtime(runtime_->user, Entry, &work));
    }
    const Method* MethodShape(const Runtime& runtime, Class* klass, const char* type,
                              const char* name, const char* result,
                              std::initializer_list<const char*> parameters, bool isStatic = false) {
        std::vector<CabbirdStringViewV1> types;
        for (const auto* parameter : parameters) types.push_back(sdk::StringView(parameter));
        CabbirdUnityMethodRequestV1 request{sizeof(request)};
        request.method_kind = isStatic ? CABBIRD_UNITY_METHOD_V1_STATIC : CABBIRD_UNITY_METHOD_V1_INSTANCE;
        request.module_name = sdk::StringView("GameAssembly.dll");
        request.type_name = sdk::StringView(type);
        request.method_name = sdk::StringView(name);
        request.return_type = sdk::StringView(result);
        request.parameter_types = types.data();
        request.parameter_count = types.size();
        CabbirdUnityMethodResultV1 resolved{sizeof(resolved)};
        if (!sdk::Succeeded(metadata_->resolve_method(metadata_->user, &request, &resolved))) return nullptr;
        const auto* method = runtime.FindMethodBySignature(klass, name, parameters);
        const auto& api = runtime.Functions();
        if (!method || runtime.TypeName(api.il2cpp_method_get_return_type(method)) != result ||
            ((api.il2cpp_method_get_flags(method, nullptr) & 0x10u) != 0) != isStatic)
            return nullptr;
        return method;
    }
    Field* FieldShape(const Runtime& runtime, Class* klass, const char* type, const char* name,
                      const char* valueType, std::uint32_t size) {
        CabbirdUnityFieldRequestV1 request{sizeof(request), size, sdk::StringView(type),
                                          sdk::StringView(name), sdk::StringView(valueType)};
        CabbirdUnityFieldResultV1 result{sizeof(result)};
        if (!sdk::Succeeded(metadata_->resolve_instance_field(metadata_->user, &request, &result)))
            return nullptr;
        auto* field = runtime.Functions().il2cpp_class_get_field_from_name(klass, name);
        return field && runtime.TypeName(runtime.Functions().il2cpp_field_get_type(field)) == valueType
                   ? field : nullptr;
    }
    bool Resolve(const Runtime& runtime) {
        if (ready_) return true;
        constexpr auto* ns = "Lens.Gameplay.Modules.BigWorld";
        baseData_ = runtime.FindClassAnyImage(ns, "BaseData");
        entity_ = runtime.FindClassAnyImage(ns, "Entity");
        alive_ = runtime.FindClassAnyImage(ns, "AliveEntity");
        auto* manager = runtime.FindClassAnyImage(ns, "EntityManager");
        auto* handle = runtime.FindClassAnyImage(ns, "EntityHandle");
        if (!baseData_ || !entity_ || !alive_ || !manager || !handle) return false;
        constexpr auto* dataName = "Lens.Gameplay.Modules.BigWorld.BaseData";
        constexpr auto* entityName = "Lens.Gameplay.Modules.BigWorld.Entity";
        constexpr auto* aliveName = "Lens.Gameplay.Modules.BigWorld.AliveEntity";
        gameId_ = FieldShape(runtime, baseData_, dataName, "<entityId>k__BackingField", "System.Int32", 4);
        version_ = FieldShape(runtime, baseData_, dataName, "<version>k__BackingField", "System.Int32", 4);
        objectIndex_ = FieldShape(runtime, baseData_, dataName, "<objIndex>k__BackingField", "System.UInt32", 4);
        entityData_ = FieldShape(runtime, entity_, entityName, "data", dataName, sizeof(void*));
        aliveData_ = FieldShape(runtime, alive_, aliveName, "data",
                                "Lens.Gameplay.Modules.BigWorld.AliveData", sizeof(void*));
        getEntity_ = MethodShape(runtime, manager, "Lens.Gameplay.Modules.BigWorld.EntityManager",
                                  "GetEntity", "Lens.Gameplay.Modules.BigWorld.EntityHandle",
                                  {"System.Int32"}, true);
        handleEntity_ = MethodShape(runtime, handle, "Lens.Gameplay.Modules.BigWorld.EntityHandle",
                                     "get_entity", entityName, {});
        isValid_ = MethodShape(runtime, entity_, entityName, "IsValid", "System.Boolean", {});
        alivePosition_ = MethodShape(runtime, alive_, aliveName, "SetWorldPosition", "System.Void",
                                      {"UnityEngine.Vector3"});
        dataPosition_ = MethodShape(runtime, baseData_, dataName, "SetPosition", "System.Void",
                                     {"UnityEngine.Vector3"});
        ready_ = gameId_ && version_ && objectIndex_ && entityData_ && aliveData_ && getEntity_ &&
                 handleEntity_ && isValid_ && alivePosition_ && dataPosition_;
        return ready_;
    }
    EntityIdentity Identity(const Runtime& runtime, Object* data) const {
        const auto& api = runtime.Functions();
        EntityIdentity identity;
        if (!data || !api.il2cpp_class_is_assignable_from(baseData_, api.il2cpp_object_get_class(data)))
            return identity;
        api.il2cpp_field_get_value(data, gameId_, &identity.gameId);
        api.il2cpp_field_get_value(data, version_, &identity.version);
        api.il2cpp_field_get_value(data, objectIndex_, &identity.objectIndex);
        identity.valid = identity.gameId != 0;
        return identity;
    }
    bool Rotation(const Runtime& runtime) {
        if (euler_ && aliveRotation_ && dataRotation_) return true;
        auto* quaternion = runtime.FindClassAnyImage("UnityEngine", "Quaternion");
        if (!quaternion) return false;
        euler_ = MethodShape(runtime, quaternion, "UnityEngine.Quaternion", "Euler", "UnityEngine.Quaternion",
                              {"UnityEngine.Vector3"}, true);
        aliveRotation_ = MethodShape(runtime, alive_, "Lens.Gameplay.Modules.BigWorld.AliveEntity",
                                      "SetRotation", "System.Void", {"UnityEngine.Quaternion"});
        dataRotation_ = MethodShape(runtime, baseData_, "Lens.Gameplay.Modules.BigWorld.BaseData",
                                     "SetRotation", "System.Void", {"UnityEngine.Quaternion"});
        return euler_ && aliveRotation_ && dataRotation_;
    }
    bool Execute(const Runtime& runtime, Work& work) {
        if (!Resolve(runtime)) return false;
        if (work.addresses) {
            for (const auto address : *work.addresses) {
                const auto identity = Identity(runtime, reinterpret_cast<Object*>(address));
                if (identity.valid) work.identities->emplace(address, identity);
            }
            return true;
        }
        auto* data = reinterpret_cast<Object*>(work.data);
        if (!work.expected->valid || Identity(runtime, data) != *work.expected) return false;
        auto gameId = work.expected->gameId;
        void* idArgs[]{&gameId};
        Object *handle{}, *entity{}, *valid{};
        if (!runtime.Invoke(getEntity_, nullptr, idArgs, &handle) || !handle ||
            !runtime.Invoke(handleEntity_, handle, nullptr, &entity) || !entity) return false;
        const auto& api = runtime.Functions();
        if (!api.il2cpp_class_is_assignable_from(entity_, api.il2cpp_object_get_class(entity))) return false;
        Object* ownerData{};
        api.il2cpp_field_get_value(entity, entityData_, &ownerData);
        if (ownerData != data || !runtime.Invoke(isValid_, entity, nullptr, &valid) || !valid) return false;
        const auto* unboxedValid = static_cast<const bool*>(api.il2cpp_object_unbox(valid));
        if (!unboxedValid || !*unboxedValid) return false;
        auto* entityClass = api.il2cpp_object_get_class(entity);
        const bool alive = api.il2cpp_class_is_assignable_from(alive_, entityClass);
        // PlatformEntity has its own movement owner. Only the ordinary Entity
        // path is established for BaseData's direct world-position setter.
        if (!alive && entityClass != entity_) return false;
        if (alive) {
            api.il2cpp_field_get_value(entity, aliveData_, &ownerData);
            if (ownerData != data) return false;
        }
        if (!work.position) return true;
        Quaternion rotation{};
        Object* result{};
        if (work.rotation) {
            if (!Rotation(runtime)) return false;
            Vector3 angles{static_cast<float>((*work.rotation)[0]), static_cast<float>((*work.rotation)[1]),
                            static_cast<float>((*work.rotation)[2])};
            void* args[]{&angles};
            if (!runtime.Invoke(euler_, nullptr, args, &result) || !result) return false;
            const auto* value = api.il2cpp_object_unbox(result);
            if (!value) return false;
            std::memcpy(&rotation, value, sizeof(rotation));
        }
        Vector3 position{static_cast<float>((*work.position)[0]), static_cast<float>((*work.position)[1]),
                          static_cast<float>((*work.position)[2])};
        void* args[]{&position};
        // AliveEntity handles the world origin and synchronizes CharacterMovement,
        // Rigidbody and Transform. Non-Alive BaseData owns a plain world Transform.
        if (!runtime.Invoke(alive ? alivePosition_ : dataPosition_, alive ? entity : data, args, &result))
            return false;
        if (work.rotation) {
            void* rotationArgs[]{&rotation};
            if (!runtime.Invoke(alive ? aliveRotation_ : dataRotation_, alive ? entity : data,
                                  rotationArgs, &result)) return false;
        }
        return true;
    }
    const CabbirdIl2CppServiceV1* runtime_{};
    const CabbirdUnityMetadataServiceV1* metadata_{};
    Class *baseData_{}, *entity_{}, *alive_{};
    Field *gameId_{}, *version_{}, *objectIndex_{}, *entityData_{}, *aliveData_{};
    const Method *getEntity_{}, *handleEntity_{}, *isValid_{}, *alivePosition_{}, *dataPosition_{};
    const Method *euler_{}, *aliveRotation_{}, *dataRotation_{};
    bool ready_{};
};
}  // namespace cabbird::plugins

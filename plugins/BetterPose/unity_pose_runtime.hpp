#pragma once

#include "cabbird/sdk/cpp.hpp"
#include "cabbird/sdk/il2cpp.hpp"
#include "cabbird/sdk/services/interop.h"
#include "cabbird/sdk/services/unity.h"
#include "cabbird/sdk/services/unity_metadata.h"

#include "joint_limits.hpp"
#include "orbit_camera.hpp"
#include "pose_math.hpp"
#include "skeleton_filter.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace betterpose {

struct UnityVec3 {
    float x{}, y{}, z{};
};
struct UnityQuat {
    // UnityEngine.Quaternion layout is (x, y, z, w).
    float x{}, y{}, z{}, w{1.f};
};
struct UnityMat4 {
    float m[16]{};
};
static_assert(sizeof(UnityVec3) == 12);
static_assert(sizeof(UnityQuat) == 16);
static_assert(sizeof(UnityMat4) == 64);

inline Vec3 FromUnity(const UnityVec3& v) noexcept {
    return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}
inline UnityVec3 ToUnity(const Vec3& v) noexcept {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}
inline Quat FromUnity(const UnityQuat& q) noexcept {
    return Quat{static_cast<double>(q.w), static_cast<double>(q.x),
                static_cast<double>(q.y), static_cast<double>(q.z)}.Normalized();
}
inline UnityQuat ToUnity(const Quat& q) noexcept {
    const Quat n = q.Normalized();
    return {static_cast<float>(n.x), static_cast<float>(n.y),
            static_cast<float>(n.z), static_cast<float>(n.w)};
}

enum class TargetKind : int {
    LocalPlayer = 0,
    Player,
    Npc,
    Monster,
    WorldEntity,
    SceneRig
};

struct TargetEntry {
    uint64_t id{};
    TargetKind kind{TargetKind::WorldEntity};
    std::string rawName;
    double distanceMeters{};
    bool hasPlayableAnimator{};
    int boneCount{};
    // Retained only on the Game thread inside Context; published snapshots leave handles null.
    CabbirdIl2CppGCHandleV1 gameObjectHandle{};
    CabbirdIl2CppGCHandleV1 playableAnimHandle{};
    CabbirdIl2CppGCHandleV1 animatorHandle{};
};

struct BoneRenderData {
    std::string name;
    int parent{-1};
    Vec3 worldPos{};
    Quat worldRot{Quat::Identity()};
    Vec3 restWorldPos{};
    Quat restWorldRot{Quat::Identity()};
    Quat restLocalRot{Quat::Identity()};
    EulerAngles offsetEuler{};
    bool isBodyBone{true};
    bool isFaceBone{false};
    JointLimit limit{};
    IkChain ikChain{};
};

struct RenderSnapshot {
    uint64_t revision{};
    bool bound{};
    bool postLateHookActive{};
    bool targetAlive{};
    bool useBindBase{};
    std::string statusDetail;
    uint64_t activeTargetId{};
    std::string activeTargetName;
    TargetKind activeTargetKind{TargetKind::LocalPlayer};
    int rootBoneIndex{-1};
    Vec3 rootOffset{};
    Quat rootParentWorldRot{Quat::Identity()};
    Vec3 rootForward{0.0, 0.0, 1.0};
    Vec3 rootRight{1.0, 0.0, 0.0};
    CameraProjection camera{};
    OrbitCameraState orbit{};
    bool orbitActive{};
    std::vector<BoneRenderData> bones;
    std::vector<TargetEntry> targets;
};

// Native Windows x64 IL2CPP signatures for UnityEngine *_Injected and property methods.
// All addresses and MethodInfo pointers are resolved dynamically through cabbird.unity.metadata
// and validated at runtime against il2cpp_runtime_invoke before use.
using NativeTransformVec3Fn = void (CABBIRD_CALL*)(CabbirdIl2CppObjectV1*, UnityVec3*, const CabbirdMethodInfoV1*);
using NativeTransformQuatFn = void (CABBIRD_CALL*)(CabbirdIl2CppObjectV1*, UnityQuat*, const CabbirdMethodInfoV1*);
using NativeCameraMat4Fn    = void (CABBIRD_CALL*)(CabbirdIl2CppObjectV1*, UnityMat4*, const CabbirdMethodInfoV1*);
using NativeCameraSetFovFn  = void (CABBIRD_CALL*)(CabbirdIl2CppObjectV1*, float, const CabbirdMethodInfoV1*);
using NativeCameraGetFovFn  = float (CABBIRD_CALL*)(CabbirdIl2CppObjectV1*, const CabbirdMethodInfoV1*);
using NativeCameraGetIntFn  = int32_t (CABBIRD_CALL*)(CabbirdIl2CppObjectV1*, const CabbirdMethodInfoV1*);
using NativeAfterLateFn     = void (CABBIRD_CALL*)(const CabbirdMethodInfoV1*);

// POD-only SEH leaf helpers for safe native calls on the Game thread.
namespace seh {

inline bool ReadPtr(std::uintptr_t address, std::uintptr_t& out) noexcept {
    __try {
        std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(out));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
}

inline bool ReadMat4(std::uintptr_t address, UnityMat4& out) noexcept {
    __try {
        std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(out));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = {};
        return false;
    }
}

inline CabbirdIl2CppClassV1* GetObjectClass(const CabbirdIl2CppApiV1& api,
                                            CabbirdIl2CppObjectV1* obj) noexcept {
    if (!obj || !api.il2cpp_object_get_class) return nullptr;
    __try {
        return api.il2cpp_object_get_class(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

inline bool CallTransformVec3(NativeTransformVec3Fn fn, CabbirdIl2CppObjectV1* obj,
                              UnityVec3* val, const CabbirdMethodInfoV1* mi) noexcept {
    if (!fn || !obj || !val || !mi) return false;
    __try {
        fn(obj, val, mi);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool CallTransformQuat(NativeTransformQuatFn fn, CabbirdIl2CppObjectV1* obj,
                              UnityQuat* val, const CabbirdMethodInfoV1* mi) noexcept {
    if (!fn || !obj || !val || !mi) return false;
    __try {
        fn(obj, val, mi);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool CallCameraMat4(NativeCameraMat4Fn fn, CabbirdIl2CppObjectV1* obj,
                           UnityMat4* val, const CabbirdMethodInfoV1* mi) noexcept {
    if (!fn || !obj || !val || !mi) return false;
    __try {
        fn(obj, val, mi);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool CallCameraSetFov(NativeCameraSetFovFn fn, CabbirdIl2CppObjectV1* obj,
                             float val, const CabbirdMethodInfoV1* mi) noexcept {
    if (!fn || !obj || !mi) return false;
    __try {
        fn(obj, val, mi);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool CallCameraGetFov(NativeCameraGetFovFn fn, CabbirdIl2CppObjectV1* obj,
                             const CabbirdMethodInfoV1* mi, float& out) noexcept {
    if (!fn || !obj || !mi) return false;
    __try {
        out = fn(obj, mi);
        return std::isfinite(out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool CallCameraGetInt(NativeCameraGetIntFn fn, CabbirdIl2CppObjectV1* obj,
                             const CabbirdMethodInfoV1* mi, int32_t& out) noexcept {
    if (!fn || !obj || !mi) return false;
    __try {
        out = fn(obj, mi);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace seh

struct UnityBindings {
    using Object = CabbirdIl2CppObjectV1;
    using Class = CabbirdIl2CppClassV1;
    using Method = CabbirdMethodInfoV1;
    using Field = CabbirdFieldInfoV1;

    bool bound{};
    std::string bindError;

    // Classes & runtime System.Type objects (GC-rooted).
    Class* unityObjectClass{};
    Class* baseDataClass{};
    Class* skinnedMeshClass{};
    Class* animatorClass{};
    Class* cinemachineBrainClass{};
    CabbirdIl2CppGCHandleV1 skinnedMeshTypeHandle{};
    CabbirdIl2CppGCHandleV1 animatorTypeHandle{};
    CabbirdIl2CppGCHandleV1 cinemachineBrainTypeHandle{};

    // Verified UnityEngine.Object.m_CachedPtr offset (resolved and checked against live metadata).
    Field* cachedPtrField{};
    uint32_t cachedPtrOffset{};
    bool hasCachedPtrOffset{};

    // Standard IL2CPP reflection methods (used during on_update / Cleanup inside with_runtime).
    const Method* objGetName{};
    const Method* objFindObjectsByType2{};
    const Method* objFindObjectsOfType1{};
    const Method* compGetTransform{};
    const Method* compGetGameObject{};
    const Method* goGetActiveInHierarchy{};
    const Method* goGetComponentsInChildren2{};
    const Method* goGetComponentsInChildren1{};
    const Method* goGetComponent1{};
    const Method* behaviourGetEnabled{};
    const Method* behaviourSetEnabled{};

    const Method* tfGetPosition{};
    const Method* tfSetPosition{};
    const Method* tfGetRotation{};
    const Method* tfSetRotation{};
    const Method* tfGetLocalPosition{};
    const Method* tfSetLocalPosition{};
    const Method* tfGetLocalRotation{};
    const Method* tfSetLocalRotation{};
    const Method* tfGetParent{};

    const Method* smrGetBones{};
    const Method* smrGetRootBone{};
    const Method* smrGetSharedMesh{};
    const Method* smrGetUpdateWhenOffscreen{};
    const Method* smrSetUpdateWhenOffscreen{};
    const Method* meshGetBindposes{};

    const Method* animGetSpeed{};
    const Method* animSetSpeed{};

    const Method* camGetMain{};
    const Method* camGetWorldToCamera{};
    const Method* camGetProjection{};
    const Method* camGetPixelWidth{};
    const Method* camGetPixelHeight{};
    const Method* camGetFov{};
    const Method* camSetFov{};

    // Runtime-verified game methods.
    const Method* camMgrGetMainCamera{};
    const Method* baseDataGetTransform{};
    const Method* baseDataGetAnimator{};
    const Method* baseDataGetConfigName{};
    const Method* relTfGetGameObject{};
    const Method* relTfGetRawTransform{};
    const Method* playableAnimGetAnimator{};
    const Method* playableAnimGetSpeed{};
    const Method* playableAnimSetSpeed{};
    const Method* playableAnimRebind{};

    // Metadata-verified native *_Injected methods for fast, zero-lag Post-LateUpdate execution.
    struct DirectEntry {
        std::uintptr_t address{};
        const Method* method{};
    };
    DirectEntry nativeGetPos{};
    DirectEntry nativeSetPos{};
    DirectEntry nativeGetRot{};
    DirectEntry nativeSetRot{};
    DirectEntry nativeGetLocalPos{};
    DirectEntry nativeSetLocalPos{};
    DirectEntry nativeGetLocalRot{};
    DirectEntry nativeSetLocalRot{};
    DirectEntry nativeCamWorldToCam{};
    DirectEntry nativeCamProj{};
    DirectEntry nativeCamGetPixelW{};
    DirectEntry nativeCamGetPixelH{};
    DirectEntry nativeCamGetFov{};
    DirectEntry nativeCamSetFov{};

    std::uintptr_t afterLateUpdateAddress{};
    bool nativeDirectReady{};
    bool nativeDirectVerified{};
    bool nativeDirectRejected{};

    bool IsUnityObjectAlive(const cabbird::sdk::il2cpp::Context& rt, Object* obj) const noexcept {
        if (!obj) return false;
        if (cachedPtrField) {
            std::uintptr_t nativePtr = 0;
            rt.Functions().il2cpp_field_get_value(obj, cachedPtrField, &nativePtr);
            return nativePtr != 0;
        }
        return true;
    }

    bool IsUnityObjectAliveFast(Object* obj) const noexcept {
        if (!obj || !hasCachedPtrOffset) return false;
        std::uintptr_t nativePtr = 0;
        return seh::ReadPtr(reinterpret_cast<std::uintptr_t>(obj) + cachedPtrOffset, nativePtr) && nativePtr != 0;
    }

    bool ResolveField(const CabbirdUnityMetadataServiceV1* metadata,
                            const char* typeName, const char* fieldName,
                            const char* fieldType, uint32_t storageSize,
                            uint32_t* outOffset = nullptr) {
        if (!metadata || !metadata->resolve_instance_field) {
            bindError = std::string(typeName) + "." + fieldName + ": metadata service unavailable";
            return false;
        }
        CabbirdUnityFieldRequestV1 req{
            sizeof(req), storageSize,
            cabbird::sdk::StringView(typeName),
            cabbird::sdk::StringView(fieldName),
            cabbird::sdk::StringView(fieldType)
        };
        CabbirdUnityFieldResultV1 res{sizeof(res)};
        const auto st = metadata->resolve_instance_field(metadata->user, &req, &res);
        if (!cabbird::sdk::Succeeded(st)) {
            bindError = std::string(typeName) + "." + fieldName + ": metadata status " + std::to_string(st.code);
            return false;
        }
        if (res.storage_size != storageSize || res.offset % alignof(void*) != 0) {
            bindError = std::string(typeName) + "." + fieldName + ": field storage size or alignment mismatch";
            return false;
        }
        if (outOffset) *outOffset = res.offset;
        return true;
    }

    // Resolve the unique complete method shape and cache its verified native entry.
    const Method* ResolveVerifiedMethod(const CabbirdUnityMetadataServiceV1* metadata,
                                        const cabbird::sdk::il2cpp::Context& rt,
                                        const char* ns, const char* clsName,
                                        const char* methodName, const char* returnType,
                                        std::initializer_list<const char*> params,
                                        bool isStatic,
                                        std::uintptr_t* outAddress = nullptr) {
        if (outAddress) *outAddress = 0;
        if (!metadata || !metadata->resolve_method) return nullptr;

        const std::string fullType = (ns && ns[0]) ? (std::string(ns) + "." + clsName) : std::string(clsName);
        std::vector<CabbirdStringViewV1> paramViews;
        paramViews.reserve(params.size());
        for (const char* p : params) paramViews.push_back(cabbird::sdk::StringView(p));

        CabbirdUnityMethodRequestV1 req{sizeof(req)};
        req.method_kind = isStatic ? CABBIRD_UNITY_METHOD_V1_STATIC : CABBIRD_UNITY_METHOD_V1_INSTANCE;
        req.module_name = cabbird::sdk::StringView("GameAssembly.dll");
        req.type_name = cabbird::sdk::StringView(fullType);
        req.method_name = cabbird::sdk::StringView(methodName);
        req.return_type = cabbird::sdk::StringView(returnType);
        req.parameter_types = paramViews.data();
        req.parameter_count = paramViews.size();

        CabbirdUnityMethodResultV1 res{sizeof(res)};
        const auto st = metadata->resolve_method(metadata->user, &req, &res);
        if (!cabbird::sdk::Succeeded(st) || !res.address) {
            bindError = fullType + "." + methodName + ": metadata status " + std::to_string(st.code);
            return nullptr;
        }
        auto* klass = rt.FindClassAnyImage(ns, clsName);
        if (!klass) {
            bindError = fullType + ": class not found";
            return nullptr;
        }
        const Method* mi = rt.FindMethodBySignature(klass, methodName, params);
        const auto& api = rt.Functions();
        uint32_t implFlags = 0;
        if (!mi || rt.TypeName(api.il2cpp_method_get_return_type(mi)) != returnType ||
            (((api.il2cpp_method_get_flags(mi, &implFlags) & 0x10U) != 0) != isStatic)) {
            bindError = fullType + "." + methodName + ": signature verification failed";
            return nullptr;
        }
        if (outAddress) *outAddress = res.address;
        return mi;
    }

    static CabbirdIl2CppGCHandleV1 RetainRuntimeType(const cabbird::sdk::il2cpp::Context& rt, Class* klass) {
        if (!klass) return nullptr;
        const auto& api = rt.Functions();
        const auto* type = api.il2cpp_class_get_type(klass);
        if (!type) return nullptr;
        Object* typeObj = api.il2cpp_type_get_object(type);
        if (!typeObj) return nullptr;
        return api.il2cpp_gchandle_new(typeObj, false);
    }

    void ReleaseTypeHandles(const CabbirdIl2CppApiV1* api) noexcept {
        if (!api || !api->il2cpp_gchandle_free) return;
        auto freeHandle = [&](CabbirdIl2CppGCHandleV1& h) {
            if (h) { api->il2cpp_gchandle_free(h); h = nullptr; }
        };
        freeHandle(skinnedMeshTypeHandle);
        freeHandle(animatorTypeHandle);
        freeHandle(cinemachineBrainTypeHandle);
    }

    void CollectHandles(std::vector<CabbirdIl2CppGCHandleV1>& out) {
        if (skinnedMeshTypeHandle) out.push_back(skinnedMeshTypeHandle);
        if (animatorTypeHandle) out.push_back(animatorTypeHandle);
        if (cinemachineBrainTypeHandle) out.push_back(cinemachineBrainTypeHandle);
        skinnedMeshTypeHandle = nullptr;
        animatorTypeHandle = nullptr;
        cinemachineBrainTypeHandle = nullptr;
    }

    bool Bind(const cabbird::sdk::il2cpp::Context& rt, const CabbirdUnityMetadataServiceV1* metadata) {
        if (bound) return true;
        bindError.clear();
        unityObjectClass = rt.FindClassAnyImage("UnityEngine", "Object");
        baseDataClass = rt.FindClassAnyImage("Lens.Gameplay.Modules.BigWorld", "BaseData");
        skinnedMeshClass = rt.FindClassAnyImage("UnityEngine", "SkinnedMeshRenderer");
        animatorClass = rt.FindClassAnyImage("UnityEngine", "Animator");
        cinemachineBrainClass = rt.FindClassAnyImage("Cinemachine", "CinemachineBrain");
        if (!unityObjectClass || !skinnedMeshClass) {
            bindError = "UnityEngine core classes unavailable";
            return false;
        }

        // Resolve and validate field layouts once in the Game domain.
        cachedPtrField = rt.FindField(unityObjectClass, "m_CachedPtr");
        if (!ResolveField(metadata, "UnityEngine.Object", "m_CachedPtr", "System.IntPtr",
                                sizeof(void*), &cachedPtrOffset)) {
            return false;
        }
        hasCachedPtrOffset = true;
        if (!ResolveField(metadata, "Lens.Gameplay.Modules.BigWorld.BaseData",
                                "<transform>k__BackingField", "Lens.Gameplay.Modules.BigWorld.RelativeTransform",
                                sizeof(void*)) ||
            !ResolveField(metadata, "Lens.Gameplay.Modules.BigWorld.BaseData",
                                "<animator>k__BackingField",
                                "Lens.Gameplay.Modules.BigWorld.PlayableAnimator.PlayableAnimator",
                                sizeof(void*)) ||
            !ResolveField(metadata, "Lens.Gameplay.Modules.BigWorld.RelativeTransform",
                                "m_transform", "UnityEngine.Transform",
                                sizeof(void*))) {
            return false;
        }

        // Core UnityEngine methods validated via cabbird.unity.metadata.
        objGetName = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Object", "get_name", "System.String", {}, false);
        compGetTransform = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Component", "get_transform", "UnityEngine.Transform", {}, false);
        compGetGameObject = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Component", "get_gameObject", "UnityEngine.GameObject", {}, false);
        goGetActiveInHierarchy = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "GameObject", "get_activeInHierarchy", "System.Boolean", {}, false);
        behaviourGetEnabled = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Behaviour", "get_enabled", "System.Boolean", {}, false);
        behaviourSetEnabled = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Behaviour", "set_enabled", "System.Void", {"System.Boolean"}, false);

        tfGetPosition = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Transform", "get_position", "UnityEngine.Vector3", {}, false);
        tfSetPosition = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Transform", "set_position", "System.Void", {"UnityEngine.Vector3"}, false);
        tfGetRotation = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Transform", "get_rotation", "UnityEngine.Quaternion", {}, false);
        tfSetRotation = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Transform", "set_rotation", "System.Void", {"UnityEngine.Quaternion"}, false);
        tfGetLocalPosition = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Transform", "get_localPosition", "UnityEngine.Vector3", {}, false);
        tfSetLocalPosition = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Transform", "set_localPosition", "System.Void", {"UnityEngine.Vector3"}, false);
        tfGetLocalRotation = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Transform", "get_localRotation", "UnityEngine.Quaternion", {}, false);
        tfSetLocalRotation = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Transform", "set_localRotation", "System.Void", {"UnityEngine.Quaternion"}, false);
        tfGetParent = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Transform", "get_parent", "UnityEngine.Transform", {}, false);
        if (!tfGetParent) {
            tfGetParent = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Transform", "GetParent", "UnityEngine.Transform", {}, false);
        }

        smrGetBones = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "SkinnedMeshRenderer", "get_bones", "UnityEngine.Transform[]", {}, false);
        if (!objGetName || !compGetTransform || !compGetGameObject || !tfGetPosition || !tfSetPosition ||
            !tfGetRotation || !tfSetRotation || !tfGetLocalPosition || !tfSetLocalPosition ||
            !tfGetLocalRotation || !tfSetLocalRotation || !tfGetParent || !smrGetBones) {
            return false;
        }

        // Camera & optional UnityEngine helpers.
        camGetWorldToCamera = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Camera", "get_worldToCameraMatrix", "UnityEngine.Matrix4x4", {}, false);
        camGetProjection = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Camera", "get_projectionMatrix", "UnityEngine.Matrix4x4", {}, false);
        camGetPixelWidth = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Camera", "get_pixelWidth", "System.Int32", {}, false);
        camGetPixelHeight = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Camera", "get_pixelHeight", "System.Int32", {}, false);
        if (!camGetWorldToCamera || !camGetProjection || !camGetPixelWidth || !camGetPixelHeight) {
            return false;
        }

        // Clear transient bindError while resolving optional overloads.
        const std::string savedErr = bindError;
        camGetMain = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Camera", "get_main", "UnityEngine.Camera", {}, true);
        camGetFov = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Camera", "get_fieldOfView", "System.Single", {}, false);
        camSetFov = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Camera", "set_fieldOfView", "System.Void", {"System.Single"}, false);
        smrGetRootBone = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "SkinnedMeshRenderer", "get_rootBone", "UnityEngine.Transform", {}, false);
        smrGetSharedMesh = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "SkinnedMeshRenderer", "get_sharedMesh", "UnityEngine.Mesh", {}, false);
        smrGetUpdateWhenOffscreen = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "SkinnedMeshRenderer", "get_updateWhenOffscreen", "System.Boolean", {}, false);
        smrSetUpdateWhenOffscreen = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "SkinnedMeshRenderer", "set_updateWhenOffscreen", "System.Void", {"System.Boolean"}, false);
        meshGetBindposes = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Mesh", "get_bindposes", "UnityEngine.Matrix4x4[]", {}, false);
        animGetSpeed = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Animator", "get_speed", "System.Single", {}, false);
        animSetSpeed = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Animator", "set_speed", "System.Void", {"System.Single"}, false);
        objFindObjectsByType2 = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Object", "FindObjectsOfType", "UnityEngine.Object[]", {"System.Type", "System.Boolean"}, true);
        objFindObjectsOfType1 = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Object", "FindObjectsOfType", "UnityEngine.Object[]", {"System.Type"}, true);
        goGetComponentsInChildren2 = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "GameObject", "GetComponentsInChildren", "UnityEngine.Component[]", {"System.Type", "System.Boolean"}, false);
        goGetComponentsInChildren1 = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "GameObject", "GetComponentsInChildren", "UnityEngine.Component[]", {"System.Type"}, false);
        goGetComponent1 = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "GameObject", "GetComponent", "UnityEngine.Component", {"System.Type"}, false);
        bindError = savedErr;

        // Game methods resolved by their complete runtime signatures.
        camMgrGetMainCamera = ResolveVerifiedMethod(
            metadata, rt, "Lens.Framework.Managers", "CameraManager", "get_mainCamera",
            "UnityEngine.Camera", {}, true);
        baseDataGetTransform = ResolveVerifiedMethod(
            metadata, rt, "Lens.Gameplay.Modules.BigWorld", "BaseData", "get_transform",
            "Lens.Gameplay.Modules.BigWorld.RelativeTransform", {}, false);
        baseDataGetAnimator = ResolveVerifiedMethod(
            metadata, rt, "Lens.Gameplay.Modules.BigWorld", "BaseData", "get_animator",
            "Lens.Gameplay.Modules.BigWorld.PlayableAnimator.PlayableAnimator", {}, false);
        baseDataGetConfigName = ResolveVerifiedMethod(
            metadata, rt, "Lens.Gameplay.Modules.BigWorld", "BaseData", "get_configName",
            "System.String", {}, false);
        relTfGetGameObject = ResolveVerifiedMethod(
            metadata, rt, "Lens.Gameplay.Modules.BigWorld", "RelativeTransform", "get_gameObject",
            "UnityEngine.GameObject", {}, false);
        relTfGetRawTransform = ResolveVerifiedMethod(
            metadata, rt, "Lens.Gameplay.Modules.BigWorld", "RelativeTransform", "GetRawTransform",
            "UnityEngine.Transform", {}, false);
        playableAnimGetAnimator = ResolveVerifiedMethod(
            metadata, rt, "Lens.Gameplay.Modules.BigWorld.PlayableAnimator", "PlayableAnimator", "get_animator",
            "UnityEngine.Animator", {}, false);
        playableAnimGetSpeed = ResolveVerifiedMethod(
            metadata, rt, "Lens.Gameplay.Modules.BigWorld.PlayableAnimator", "PlayableAnimator", "get_speed",
            "System.Single", {}, false);
        playableAnimSetSpeed = ResolveVerifiedMethod(
            metadata, rt, "Lens.Gameplay.Modules.BigWorld.PlayableAnimator", "PlayableAnimator", "set_speed",
            "System.Void", {"System.Single"}, false);
        playableAnimRebind = ResolveVerifiedMethod(
            metadata, rt, "Lens.Gameplay.Modules.BigWorld.PlayableAnimator", "PlayableAnimator", "Rebind",
            "System.Void", {}, false);

        std::uintptr_t afterLateAddr = 0;
        const Method* afterLateMethod = ResolveVerifiedMethod(
            metadata, rt, "Lens.Framework", "UpdateSource", "RunAfterLateUpdate",
            "System.Void", {}, true, &afterLateAddr);

        if (!camMgrGetMainCamera || !baseDataGetTransform || !baseDataGetAnimator || !baseDataGetConfigName ||
            !relTfGetGameObject || !relTfGetRawTransform || !playableAnimGetAnimator ||
            !playableAnimGetSpeed || !playableAnimSetSpeed || !playableAnimRebind || !afterLateMethod) {
            return false;
        }
        afterLateUpdateAddress = afterLateAddr;

        skinnedMeshTypeHandle = RetainRuntimeType(rt, skinnedMeshClass);
        animatorTypeHandle = RetainRuntimeType(rt, animatorClass);
        cinemachineBrainTypeHandle = RetainRuntimeType(rt, cinemachineBrainClass);
        if (!skinnedMeshTypeHandle) {
            bindError = "failed to retain SkinnedMeshRenderer runtime type";
            return false;
        }

        // Resolve optional native *_Injected methods for fast Post-LateUpdate pass.
        auto resolveDirect = [&](const char* ns, const char* cls, const char* name,
                                 const char* ret, std::initializer_list<const char*> params,
                                 DirectEntry& out) -> bool {
            out = {};
            std::uintptr_t addr = 0;
            const Method* m = ResolveVerifiedMethod(metadata, rt, ns, cls, name, ret, params, false, &addr);
            if (!m || !addr) return false;
            out.address = addr;
            out.method = m;
            return true;
        };
        const bool okTf =
            resolveDirect("UnityEngine", "Transform", "get_position_Injected", "System.Void", {"UnityEngine.Vector3&"}, nativeGetPos) &&
            resolveDirect("UnityEngine", "Transform", "set_position_Injected", "System.Void", {"UnityEngine.Vector3&"}, nativeSetPos) &&
            resolveDirect("UnityEngine", "Transform", "get_rotation_Injected", "System.Void", {"UnityEngine.Quaternion&"}, nativeGetRot) &&
            resolveDirect("UnityEngine", "Transform", "set_rotation_Injected", "System.Void", {"UnityEngine.Quaternion&"}, nativeSetRot) &&
            resolveDirect("UnityEngine", "Transform", "get_localPosition_Injected", "System.Void", {"UnityEngine.Vector3&"}, nativeGetLocalPos) &&
            resolveDirect("UnityEngine", "Transform", "set_localPosition_Injected", "System.Void", {"UnityEngine.Vector3&"}, nativeSetLocalPos) &&
            resolveDirect("UnityEngine", "Transform", "get_localRotation_Injected", "System.Void", {"UnityEngine.Quaternion&"}, nativeGetLocalRot) &&
            resolveDirect("UnityEngine", "Transform", "set_localRotation_Injected", "System.Void", {"UnityEngine.Quaternion&"}, nativeSetLocalRot);
        const bool okCam =
            resolveDirect("UnityEngine", "Camera", "get_worldToCameraMatrix_Injected", "System.Void", {"UnityEngine.Matrix4x4&"}, nativeCamWorldToCam) &&
            resolveDirect("UnityEngine", "Camera", "get_projectionMatrix_Injected", "System.Void", {"UnityEngine.Matrix4x4&"}, nativeCamProj) &&
            resolveDirect("UnityEngine", "Camera", "get_pixelWidth", "System.Int32", {}, nativeCamGetPixelW) &&
            resolveDirect("UnityEngine", "Camera", "get_pixelHeight", "System.Int32", {}, nativeCamGetPixelH) &&
            resolveDirect("UnityEngine", "Camera", "get_fieldOfView", "System.Single", {}, nativeCamGetFov) &&
            resolveDirect("UnityEngine", "Camera", "set_fieldOfView", "System.Void", {"System.Single"}, nativeCamSetFov);
        nativeDirectReady = okTf && okCam && hasCachedPtrOffset;
        bindError.clear();

        bound = true;
        return true;
    }

    template <class T>
    static bool InvokeValue(const cabbird::sdk::il2cpp::Context& rt, const Method* m,
                            Object* self, void** args, T& out) {
        Object* boxed = nullptr;
        if (!m || !rt.Invoke(m, self, args, &boxed) || !boxed) return false;
        const auto* ptr = static_cast<const T*>(rt.Functions().il2cpp_object_unbox(boxed));
        if (!ptr) return false;
        out = *ptr;
        return true;
    }

    std::string GetObjectName(const cabbird::sdk::il2cpp::Context& rt, Object* obj) const {
        if (!obj || !objGetName) return {};
        Object* strObj = nullptr;
        if (!rt.Invoke(objGetName, obj, nullptr, &strObj) || !strObj) return {};
        return rt.ReadString(strObj).value_or(std::string{});
    }

    Object* GetMainCamera(const cabbird::sdk::il2cpp::Context& rt) const {
        Object* cam = nullptr;
        if (camMgrGetMainCamera && rt.Invoke(camMgrGetMainCamera, nullptr, nullptr, &cam) && cam &&
            IsUnityObjectAlive(rt, cam)) {
            return cam;
        }
        if (camGetMain && rt.Invoke(camGetMain, nullptr, nullptr, &cam) && cam &&
            IsUnityObjectAlive(rt, cam)) {
            return cam;
        }
        return nullptr;
    }

    bool ReadCameraProjectionReflection(const cabbird::sdk::il2cpp::Context& rt, Object* cam,
                                        CameraProjection& out) const {
        out = {};
        if (!cam || !IsUnityObjectAlive(rt, cam)) return false;
        UnityMat4 w2cRaw{}, projRaw{};
        int32_t pw = 0, ph = 0;
        if (!InvokeValue(rt, camGetWorldToCamera, cam, nullptr, w2cRaw) ||
            !InvokeValue(rt, camGetProjection, cam, nullptr, projRaw) ||
            !InvokeValue(rt, camGetPixelWidth, cam, nullptr, pw) ||
            !InvokeValue(rt, camGetPixelHeight, cam, nullptr, ph) ||
            pw <= 1 || ph <= 1) {
            return false;
        }
        const Mat4 w2c = Mat4::FromUnityColumnMajor(w2cRaw.m);
        const Mat4 proj = Mat4::FromUnityColumnMajor(projRaw.m);
        if (!w2c.IsFinite() || !proj.IsFinite()) return false;
        out.viewProjection = Mat4::Multiply(proj, w2c);
        if (!out.viewProjection.IsFinite()) return false;
        out.viewportW = static_cast<float>(pw);
        out.viewportH = static_cast<float>(ph);

        float fov = 45.f;
        if (camGetFov && InvokeValue(rt, camGetFov, cam, nullptr, fov) && std::isfinite(fov)) {
            out.verticalFovDeg = fov;
        }
        Object* camTf = nullptr;
        if (rt.Invoke(compGetTransform, cam, nullptr, &camTf) && camTf && IsUnityObjectAlive(rt, camTf)) {
            UnityVec3 pos{};
            UnityQuat rot{};
            if (InvokeValue(rt, tfGetPosition, camTf, nullptr, pos)) out.cameraPos = FromUnity(pos);
            if (InvokeValue(rt, tfGetRotation, camTf, nullptr, rot)) out.cameraRot = FromUnity(rot);
        }
        out.valid = true;
        return true;
    }

    // Live self-check verifying native *_Injected ABI against il2cpp_runtime_invoke on the active camera.
    bool VerifyNativeDirectAbi(const cabbird::sdk::il2cpp::Context& rt, Object* cam) {
        if (nativeDirectVerified) return true;
        if (!nativeDirectReady || nativeDirectRejected || !cam) return false;
        Object* camTf = nullptr;
        if (!rt.Invoke(compGetTransform, cam, nullptr, &camTf) || !camTf || !IsUnityObjectAlive(rt, camTf)) {
            return false;
        }
        UnityVec3 refPos{};
        UnityQuat refRot{};
        UnityMat4 refW2c{};
        if (!InvokeValue(rt, tfGetPosition, camTf, nullptr, refPos) ||
            !InvokeValue(rt, tfGetRotation, camTf, nullptr, refRot) ||
            !InvokeValue(rt, camGetWorldToCamera, cam, nullptr, refW2c)) {
            return false;
        }
        UnityVec3 natPos{};
        UnityQuat natRot{};
        UnityMat4 natW2c{};
        if (!seh::CallTransformVec3(reinterpret_cast<NativeTransformVec3Fn>(nativeGetPos.address), camTf, &natPos, nativeGetPos.method) ||
            !seh::CallTransformQuat(reinterpret_cast<NativeTransformQuatFn>(nativeGetRot.address), camTf, &natRot, nativeGetRot.method) ||
            !seh::CallCameraMat4(reinterpret_cast<NativeCameraMat4Fn>(nativeCamWorldToCam.address), cam, &natW2c, nativeCamWorldToCam.method)) {
            nativeDirectRejected = true;
            return false;
        }
        const double posErr = std::fabs(natPos.x - refPos.x) + std::fabs(natPos.y - refPos.y) + std::fabs(natPos.z - refPos.z);
        const double rotErr = std::fabs(natRot.x - refRot.x) + std::fabs(natRot.y - refRot.y) +
                              std::fabs(natRot.z - refRot.z) + std::fabs(natRot.w - refRot.w);
        double matErr = 0.0;
        for (int i = 0; i < 16; ++i) matErr += std::fabs(natW2c.m[i] - refW2c.m[i]);

        if (!(posErr < 1e-2) || !(rotErr < 1e-2) || !(matErr < 1e-1)) {
            nativeDirectRejected = true;
            return false;
        }
        nativeDirectVerified = true;
        return true;
    }

    bool ReadCameraProjectionNative(Object* cam, Object* camTf, CameraProjection& out) const noexcept {
        out = {};
        if (!nativeDirectVerified || !IsUnityObjectAliveFast(cam) || !IsUnityObjectAliveFast(camTf)) {
            return false;
        }
        UnityMat4 w2cRaw{}, projRaw{};
        int32_t pw = 0, ph = 0;
        if (!seh::CallCameraMat4(reinterpret_cast<NativeCameraMat4Fn>(nativeCamWorldToCam.address), cam, &w2cRaw, nativeCamWorldToCam.method) ||
            !seh::CallCameraMat4(reinterpret_cast<NativeCameraMat4Fn>(nativeCamProj.address), cam, &projRaw, nativeCamProj.method) ||
            !seh::CallCameraGetInt(reinterpret_cast<NativeCameraGetIntFn>(nativeCamGetPixelW.address), cam, nativeCamGetPixelW.method, pw) ||
            !seh::CallCameraGetInt(reinterpret_cast<NativeCameraGetIntFn>(nativeCamGetPixelH.address), cam, nativeCamGetPixelH.method, ph) ||
            pw <= 1 || ph <= 1) {
            return false;
        }
        const Mat4 w2c = Mat4::FromUnityColumnMajor(w2cRaw.m);
        const Mat4 proj = Mat4::FromUnityColumnMajor(projRaw.m);
        if (!w2c.IsFinite() || !proj.IsFinite()) return false;
        out.viewProjection = Mat4::Multiply(proj, w2c);
        if (!out.viewProjection.IsFinite()) return false;
        out.viewportW = static_cast<float>(pw);
        out.viewportH = static_cast<float>(ph);

        float fov = 45.f;
        if (seh::CallCameraGetFov(reinterpret_cast<NativeCameraGetFovFn>(nativeCamGetFov.address), cam, nativeCamGetFov.method, fov)) {
            out.verticalFovDeg = fov;
        }
        UnityVec3 pos{};
        UnityQuat rot{};
        if (seh::CallTransformVec3(reinterpret_cast<NativeTransformVec3Fn>(nativeGetPos.address), camTf, &pos, nativeGetPos.method)) {
            out.cameraPos = FromUnity(pos);
        }
        if (seh::CallTransformQuat(reinterpret_cast<NativeTransformQuatFn>(nativeGetRot.address), camTf, &rot, nativeGetRot.method)) {
            out.cameraRot = FromUnity(rot);
        }
        out.valid = true;
        return true;
    }
};

}  // namespace betterpose

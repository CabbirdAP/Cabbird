#pragma once

#include "cabbird/sdk/cpp.hpp"
#include "cabbird/sdk/il2cpp.hpp"
#include "cabbird/sdk/services/core.h"
#include "cabbird/sdk/services/unity_metadata.h"

#include "camera_math.hpp"

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace cameratools {

struct UnityVec3 {
    float x{}, y{}, z{};
};
struct UnityQuat {
    float x{}, y{}, z{}, w{1.f};
};
static_assert(sizeof(UnityVec3) == 12);
static_assert(sizeof(UnityQuat) == 16);

inline Vec3 FromUnity(const UnityVec3& v) noexcept {
    return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}
inline UnityVec3 ToUnity(const Vec3& v) noexcept {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}
inline Quat FromUnity(const UnityQuat& q) noexcept {
    return Quat{static_cast<double>(q.x), static_cast<double>(q.y),
                static_cast<double>(q.z), static_cast<double>(q.w)}.Normalized();
}

// Native Windows x64 IL2CPP signatures resolved dynamically via cabbird.unity.metadata
// and live-verified against il2cpp_runtime_invoke before direct pre-render execution.
using NativeTransformVec3Fn      = void (CABBIRD_CALL*)(CabbirdIl2CppObjectV1*, UnityVec3*, const CabbirdMethodInfoV1*);
using NativeTransformQuatFn      = void (CABBIRD_CALL*)(CabbirdIl2CppObjectV1*, UnityQuat*, const CabbirdMethodInfoV1*);
// Win64 x64 hidden-return-buffer ABI for 12-byte Vector3 / 16-byte Quaternion returns:
// RCX = caller-allocated output buffer, RDX = this (Transform*), R8 = MethodInfo*.
using NativeTransformRetVec3Fn   = UnityVec3* (CABBIRD_CALL*)(UnityVec3*, CabbirdIl2CppObjectV1*, const CabbirdMethodInfoV1*);
using NativeTransformRetQuatFn   = UnityQuat* (CABBIRD_CALL*)(UnityQuat*, CabbirdIl2CppObjectV1*, const CabbirdMethodInfoV1*);
using NativeCameraGetFovFn       = float (CABBIRD_CALL*)(CabbirdIl2CppObjectV1*, const CabbirdMethodInfoV1*);
using NativeCameraSetFovFn       = void (CABBIRD_CALL*)(CabbirdIl2CppObjectV1*, float, const CabbirdMethodInfoV1*);

// Hooked game method signatures (Windows x64 IL2CPP ABI).
using UrpRenderFn        = void (CABBIRD_CALL*)(CabbirdIl2CppObjectV1*, std::uintptr_t, CabbirdIl2CppObjectV1*, const CabbirdMethodInfoV1*);
using InstanceVoid0Fn    = void (CABBIRD_CALL*)(CabbirdIl2CppObjectV1*, const CabbirdMethodInfoV1*);
using InstanceVoid1ObjFn = void (CABBIRD_CALL*)(CabbirdIl2CppObjectV1*, CabbirdIl2CppObjectV1*, const CabbirdMethodInfoV1*);

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

inline bool CallTransformRetVec3(NativeTransformRetVec3Fn fn, CabbirdIl2CppObjectV1* obj,
                                 UnityVec3* out, const CabbirdMethodInfoV1* mi) noexcept {
    if (!fn || !obj || !out || !mi) return false;
    __try {
        fn(out, obj, mi);
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

inline bool CallTransformRetQuat(NativeTransformRetQuatFn fn, CabbirdIl2CppObjectV1* obj,
                                 UnityQuat* out, const CabbirdMethodInfoV1* mi) noexcept {
    if (!fn || !obj || !out || !mi) return false;
    __try {
        fn(out, obj, mi);
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

}  // namespace seh

struct CameraBindings {
    using Object = CabbirdIl2CppObjectV1;
    using Class = CabbirdIl2CppClassV1;
    using Method = CabbirdMethodInfoV1;
    using Field = CabbirdFieldInfoV1;

    bool coreReady{};
    bool aoiReady{};
    std::string bindDiagnostics;

    // Verified UnityEngine.Object.m_CachedPtr offset (cross-checked with live metadata).
    Field* cachedPtrField{};
    uint32_t cachedPtrOffset{};
    bool hasCachedPtrOffset{};

    // Managed reflection methods (Game domain inside with_runtime).
    const Method* camGetMain{};
    const Method* camMgrGetMain{};
    const Method* compGetTransform{};
    const Method* tfGetPosition{};
    const Method* tfSetPosition{};
    const Method* tfGetRotation{};
    const Method* camGetFov{};
    const Method* camSetFov{};
    const Method* appGetFocused{};
    const Method* worldGetAoi{};
    const Method* worldUpdateAoi{};

    // Metadata-verified direct native entries for zero-lag pre-render execution.
    struct DirectEntry {
        std::uintptr_t address{};
        const Method* method{};
    };
    DirectEntry nativeGetPos{};
    bool nativeGetPosUsesRetBuf{};
    DirectEntry nativeSetPos{};
    DirectEntry nativeGetRot{};
    bool nativeGetRotUsesRetBuf{};
    DirectEntry nativeGetFov{};
    DirectEntry nativeSetFov{};
    bool nativeDirectReady{};
    bool nativeDirectVerified{};
    bool nativeDirectRejected{};

    // Hook target addresses resolved and cached from complete method metadata.
    std::uintptr_t addrUrpRender{};
    std::uintptr_t addrInputMovement{};
    std::uintptr_t addrInputJump{};
    std::uintptr_t addrInputEvade{};
    std::uintptr_t addrInputFlyUp{};
    std::uintptr_t addrInputFlyFall{};
    std::uintptr_t addrInputFlySprint{};
    std::uintptr_t addrInputSwimSprint{};

    void AppendDiag(std::string_view entry) {
        if (entry.empty()) return;
        if (!bindDiagnostics.empty()) bindDiagnostics += "; ";
        bindDiagnostics.append(entry);
    }

    bool ResolveField(const CabbirdUnityMetadataServiceV1* metadata,
                            const char* typeName, const char* fieldName,
                            const char* fieldType, uint32_t storageSize,
                            uint32_t* outOffset = nullptr) {
        if (!metadata || !metadata->resolve_instance_field) return false;
        CabbirdUnityFieldRequestV1 req{
            sizeof(req), storageSize,
            cabbird::sdk::StringView(typeName),
            cabbird::sdk::StringView(fieldName),
            cabbird::sdk::StringView(fieldType)
        };
        CabbirdUnityFieldResultV1 res{sizeof(res)};
        if (!cabbird::sdk::Succeeded(metadata->resolve_instance_field(metadata->user, &req, &res))) {
            return false;
        }
        if (res.storage_size != storageSize || res.offset % alignof(void*) != 0) {
            return false;
        }
        if (outOffset) *outOffset = res.offset;
        return true;
    }

    const Method* ResolveVerifiedMethod(const CabbirdUnityMetadataServiceV1* metadata,
                                        const cabbird::sdk::il2cpp::Context& rt,
                                        const char* ns, const char* clsName,
                                        const char* methodName, const char* returnType,
                                        std::initializer_list<const char*> params,
                                        bool isStatic,
                                        std::uintptr_t* outAddress = nullptr,
                                        std::string* outDiag = nullptr) {
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
            if (outDiag) {
                std::string msg = fullType + "." + methodName + ": meta=" + std::to_string(st.code);
                if (st.message.data && st.message.size) {
                    msg += "(";
                    msg.append(st.message.data, st.message.size);
                    msg += ")";
                }
                *outDiag = std::move(msg);
            }
            return nullptr;
        }
        auto* klass = rt.FindClassAnyImage(ns, clsName);
        if (!klass) {
            if (outDiag) *outDiag = fullType + ": class-not-found";
            return nullptr;
        }
        const Method* mi = rt.FindMethodBySignature(klass, methodName, params);
        const auto& api = rt.Functions();
        uint32_t implFlags = 0;
        if (!mi || rt.TypeName(api.il2cpp_method_get_return_type(mi)) != returnType ||
            (((api.il2cpp_method_get_flags(mi, &implFlags) & 0x10U) != 0) != isStatic)) {
            if (outDiag) *outDiag = fullType + "." + methodName + ": rt-signature-mismatch";
            return nullptr;
        }
        if (outAddress) *outAddress = res.address;
        return mi;
    }

    bool Bind(const CabbirdUnityMetadataServiceV1* metadata, const cabbird::sdk::il2cpp::Context& rt) {
        if (!metadata) return false;
        bindDiagnostics.clear();

        // UnityEngine.Object.m_CachedPtr (resolved and checked against live metadata).
        if (!hasCachedPtrOffset) {
            uint32_t offset = 0;
            if (ResolveField(metadata, "UnityEngine.Object", "m_CachedPtr",
                                   "System.IntPtr", sizeof(void*), &offset)) {
                cachedPtrOffset = offset;
                hasCachedPtrOffset = true;
                if (auto* objCls = rt.FindClassAnyImage("UnityEngine", "Object")) {
                    cachedPtrField = rt.FindField(objCls, "m_CachedPtr");
                }
            } else {
                AppendDiag("m_CachedPtr:field-mismatch");
            }
        }

        // Core Unity camera/transform/focus methods (managed runtime identifiers).
        camGetMain = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Camera", "get_main",
                                           "UnityEngine.Camera", {}, true);
        camMgrGetMain = ResolveVerifiedMethod(metadata, rt, "Lens.Framework.Managers", "CameraManager",
                                              "get_mainCamera", "UnityEngine.Camera", {}, true);
        compGetTransform = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Component", "get_transform",
                                                 "UnityEngine.Transform", {}, false);
        std::uintptr_t getPosAddr = 0;
        tfGetPosition = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Transform", "get_position",
                                              "UnityEngine.Vector3", {}, false, &getPosAddr);
        tfSetPosition = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Transform", "set_position",
                                              "System.Void", {"UnityEngine.Vector3"}, false);
        std::uintptr_t getRotAddr = 0;
        tfGetRotation = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Transform", "get_rotation",
                                              "UnityEngine.Quaternion", {}, false, &getRotAddr);
        camGetFov = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Camera", "get_fieldOfView",
                                          "System.Single", {}, false, &nativeGetFov.address);
        nativeGetFov.method = camGetFov;
        camSetFov = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Camera", "set_fieldOfView",
                                          "System.Void", {"System.Single"}, false, &nativeSetFov.address);
        nativeSetFov.method = camSetFov;
        appGetFocused = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Application", "get_isFocused",
                                              "System.Boolean", {}, true);

        // Native transform methods for fast pre-render execution: prefer *_Injected,
        // fall back cleanly to get_position/get_rotation hidden-return-buffer native entry
        // (live self-checked in EnsureNativeDirectVerified before use).
        std::string diag;
        nativeGetPos = {};
        nativeGetPosUsesRetBuf = false;
        nativeGetPos.method = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Transform",
                                                    "get_position_Injected", "System.Void",
                                                    {"UnityEngine.Vector3&"}, false,
                                                    &nativeGetPos.address, &diag);
        if (!nativeGetPos.address || !nativeGetPos.method) {
            if (tfGetPosition && getPosAddr) {
                nativeGetPos.address = getPosAddr;
                nativeGetPos.method = tfGetPosition;
                nativeGetPosUsesRetBuf = true;
            } else if (!diag.empty()) {
                AppendDiag(diag);
            }
        }

        diag.clear();
        nativeSetPos = {};
        nativeSetPos.method = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Transform",
                                                    "set_position_Injected", "System.Void",
                                                    {"UnityEngine.Vector3&"}, false,
                                                    &nativeSetPos.address, &diag);
        if (!nativeSetPos.address || !nativeSetPos.method) AppendDiag(diag);

        diag.clear();
        nativeGetRot = {};
        nativeGetRotUsesRetBuf = false;
        nativeGetRot.method = ResolveVerifiedMethod(metadata, rt, "UnityEngine", "Transform",
                                                    "get_rotation_Injected", "System.Void",
                                                    {"UnityEngine.Quaternion&"}, false,
                                                    &nativeGetRot.address, &diag);
        if (!nativeGetRot.address || !nativeGetRot.method) {
            if (tfGetRotation && getRotAddr) {
                nativeGetRot.address = getRotAddr;
                nativeGetRot.method = tfGetRotation;
                nativeGetRotUsesRetBuf = true;
            } else if (!diag.empty()) {
                AppendDiag(diag);
            }
        }

        nativeDirectReady = hasCachedPtrOffset &&
                            nativeGetPos.address && nativeGetPos.method &&
                            nativeSetPos.address && nativeSetPos.method &&
                            nativeGetRot.address && nativeGetRot.method &&
                            nativeGetFov.address && nativeGetFov.method &&
                            nativeSetFov.address && nativeSetFov.method;

        // Pre-render URP hook resolved from runtime metadata.
        diag.clear();
        ResolveVerifiedMethod(metadata, rt, "UnityEngine.Rendering.Azur", "UniversalRenderPipeline",
                              "Render", "System.Void",
                              {"UnityEngine.Rendering.ScriptableRenderContext",
                               "System.Collections.Generic.List<UnityEngine.Camera>"},
                              false, &addrUrpRender, &diag);
        if (!addrUrpRender && !diag.empty()) AppendDiag(diag);

        // Player movement suppression hook targets (each verified independently).
        diag.clear();
        ResolveVerifiedMethod(metadata, rt, "Lens.Framework.Managers", "InputManager",
                              "OnMainMovement", "System.Void", {"UnityEngine.InputSystem.InputValue"}, false, &addrInputMovement, &diag);
        if (!addrInputMovement && !diag.empty()) AppendDiag(diag);
        ResolveVerifiedMethod(metadata, rt, "Lens.Framework.Managers", "InputManager",
                              "OnMainJump", "System.Void", {}, false, &addrInputJump);
        ResolveVerifiedMethod(metadata, rt, "Lens.Framework.Managers", "InputManager",
                              "OnMainEvade", "System.Void", {"UnityEngine.InputSystem.InputValue"}, false, &addrInputEvade);
        ResolveVerifiedMethod(metadata, rt, "Lens.Framework.Managers", "InputManager",
                              "OnMainFlyUp", "System.Void", {"UnityEngine.InputSystem.InputValue"}, false, &addrInputFlyUp);
        ResolveVerifiedMethod(metadata, rt, "Lens.Framework.Managers", "InputManager",
                              "OnMainFlyFall", "System.Void", {"UnityEngine.InputSystem.InputValue"}, false, &addrInputFlyFall);
        ResolveVerifiedMethod(metadata, rt, "Lens.Framework.Managers", "InputManager",
                              "OnMainFlySprint", "System.Void", {"UnityEngine.InputSystem.InputValue"}, false, &addrInputFlySprint);
        ResolveVerifiedMethod(metadata, rt, "Lens.Framework.Managers", "InputManager",
                              "OnMainSwimSprint", "System.Void", {"UnityEngine.InputSystem.InputValue"}, false, &addrInputSwimSprint);

        // Scene streaming (AOI) methods.
        worldGetAoi = ResolveVerifiedMethod(metadata, rt, "Lens.Gameplay.Modules.Azur", "AzurWorld",
                                            "get_AOI", "Lens.Gameplay.Modules.Azur.WorldAOIMgr",
                                            {}, true);
        worldUpdateAoi = ResolveVerifiedMethod(metadata, rt, "Lens.Gameplay.Modules.Azur", "WorldAOIMgr",
                                               "UpdateAOI", "System.Void", {"UnityEngine.Vector3"},
                                               false);
        aoiReady = worldGetAoi != nullptr && worldUpdateAoi != nullptr;

        coreReady = (camGetMain || camMgrGetMain) && compGetTransform && tfGetPosition &&
                    tfSetPosition && tfGetRotation && camGetFov && camSetFov && appGetFocused;
        return coreReady;
    }

    bool CallNativeGetPos(Object* camTf, UnityVec3& out) const noexcept {
        if (!nativeGetPos.address || !nativeGetPos.method || !camTf) return false;
        if (nativeGetPosUsesRetBuf) {
            return seh::CallTransformRetVec3(
                reinterpret_cast<NativeTransformRetVec3Fn>(nativeGetPos.address),
                camTf, &out, nativeGetPos.method);
        }
        return seh::CallTransformVec3(
            reinterpret_cast<NativeTransformVec3Fn>(nativeGetPos.address),
            camTf, &out, nativeGetPos.method);
    }

    bool CallNativeGetRot(Object* camTf, UnityQuat& out) const noexcept {
        if (!nativeGetRot.address || !nativeGetRot.method || !camTf) return false;
        if (nativeGetRotUsesRetBuf) {
            return seh::CallTransformRetQuat(
                reinterpret_cast<NativeTransformRetQuatFn>(nativeGetRot.address),
                camTf, &out, nativeGetRot.method);
        }
        return seh::CallTransformQuat(
            reinterpret_cast<NativeTransformQuatFn>(nativeGetRot.address),
            camTf, &out, nativeGetRot.method);
    }

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

    Object* ResolveMainCamera(const cabbird::sdk::il2cpp::Context& rt) const noexcept {
        Object* cam = nullptr;
        if (camMgrGetMain && rt.Invoke(camMgrGetMain, nullptr, nullptr, &cam) && cam && IsUnityObjectAlive(rt, cam)) {
            return cam;
        }
        cam = nullptr;
        if (camGetMain && rt.Invoke(camGetMain, nullptr, nullptr, &cam) && cam && IsUnityObjectAlive(rt, cam)) {
            return cam;
        }
        return nullptr;
    }

    Object* ResolveComponentTransform(const cabbird::sdk::il2cpp::Context& rt, Object* comp) const noexcept {
        if (!comp || !compGetTransform) return nullptr;
        Object* tf = nullptr;
        if (!rt.Invoke(compGetTransform, comp, nullptr, &tf) || !tf || !IsUnityObjectAlive(rt, tf)) {
            return nullptr;
        }
        return tf;
    }

    bool ReadAppFocused(const cabbird::sdk::il2cpp::Context& rt) const noexcept {
        if (!appGetFocused) return false;
        Object* boxed = nullptr;
        if (!rt.Invoke(appGetFocused, nullptr, nullptr, &boxed) || !boxed) return false;
        const auto* val = static_cast<const bool*>(rt.Functions().il2cpp_object_unbox(boxed));
        return val && *val;
    }

    bool ReadManagedPos(const cabbird::sdk::il2cpp::Context& rt, Object* tf, UnityVec3& out) const noexcept {
        if (!tf || !tfGetPosition) return false;
        Object* boxed = nullptr;
        if (!rt.Invoke(tfGetPosition, tf, nullptr, &boxed) || !boxed) return false;
        const auto* v = static_cast<const UnityVec3*>(rt.Functions().il2cpp_object_unbox(boxed));
        if (!v || !FromUnity(*v).IsFinite()) return false;
        out = *v;
        return true;
    }

    bool WriteManagedPos(const cabbird::sdk::il2cpp::Context& rt, Object* tf, const UnityVec3& val) const noexcept {
        if (!tf || !tfSetPosition) return false;
        UnityVec3 arg = val;
        void* params[1] = {&arg};
        Object* ignored = nullptr;
        return rt.Invoke(tfSetPosition, tf, params, &ignored);
    }

    bool ReadManagedRot(const cabbird::sdk::il2cpp::Context& rt, Object* tf, UnityQuat& out) const noexcept {
        if (!tf || !tfGetRotation) return false;
        Object* boxed = nullptr;
        if (!rt.Invoke(tfGetRotation, tf, nullptr, &boxed) || !boxed) return false;
        const auto* q = static_cast<const UnityQuat*>(rt.Functions().il2cpp_object_unbox(boxed));
        if (!q || !FromUnity(*q).IsFinite()) return false;
        out = *q;
        return true;
    }

    bool ReadManagedFov(const cabbird::sdk::il2cpp::Context& rt, Object* cam, float& out) const noexcept {
        if (!cam || !camGetFov) return false;
        Object* boxed = nullptr;
        if (!rt.Invoke(camGetFov, cam, nullptr, &boxed) || !boxed) return false;
        const auto* f = static_cast<const float*>(rt.Functions().il2cpp_object_unbox(boxed));
        if (!f || !std::isfinite(*f) || *f < 1.f || *f > 179.f) return false;
        out = *f;
        return true;
    }

    bool WriteManagedFov(const cabbird::sdk::il2cpp::Context& rt, Object* cam, float fov) const noexcept {
        if (!cam || !camSetFov || !std::isfinite(fov)) return false;
        float arg = fov;
        void* params[1] = {&arg};
        Object* ignored = nullptr;
        return rt.Invoke(camSetFov, cam, params, &ignored);
    }

    // Cross-verify direct native calls once against managed il2cpp_runtime_invoke
    // on a live camera before enabling fast pre-render execution.
    bool EnsureNativeDirectVerified(const cabbird::sdk::il2cpp::Context& rt,
                                    Object* cam, Object* camTf) noexcept {
        if (nativeDirectVerified) return true;
        if (nativeDirectRejected || !nativeDirectReady || !cam || !camTf) return false;

        UnityVec3 mPos{}, nPos{};
        UnityQuat mRot{}, nRot{};
        float mFov = 0.f, nFov = 0.f;
        if (!ReadManagedPos(rt, camTf, mPos) || !ReadManagedRot(rt, camTf, mRot) ||
            !ReadManagedFov(rt, cam, mFov)) {
            return false;
        }
        const bool okPos = CallNativeGetPos(camTf, nPos);
        const bool okRot = CallNativeGetRot(camTf, nRot);
        const bool okFov = seh::CallCameraGetFov(
            reinterpret_cast<NativeCameraGetFovFn>(nativeGetFov.address), cam, nativeGetFov.method, nFov);

        const auto nearf = [](float a, float b) { return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) < 1e-2f; };
        const bool rotMatch =
            (nearf(mRot.x, nRot.x) && nearf(mRot.y, nRot.y) && nearf(mRot.z, nRot.z) && nearf(mRot.w, nRot.w)) ||
            (nearf(mRot.x, -nRot.x) && nearf(mRot.y, -nRot.y) && nearf(mRot.z, -nRot.z) && nearf(mRot.w, -nRot.w));
        if (okPos && okRot && okFov &&
            nearf(mPos.x, nPos.x) && nearf(mPos.y, nPos.y) && nearf(mPos.z, nPos.z) &&
            rotMatch && nearf(mFov, nFov)) {
            nativeDirectVerified = true;
            return true;
        }
        nativeDirectRejected = true;
        return false;
    }

    bool UpdateAoiPosition(const cabbird::sdk::il2cpp::Context& rt, const Vec3& pos) const noexcept {
        if (!aoiReady || !pos.IsFinite()) return false;
        Object* aoi = nullptr;
        if (!rt.Invoke(worldGetAoi, nullptr, nullptr, &aoi) || !aoi) return false;
        UnityVec3 arg = ToUnity(pos);
        void* params[1] = {&arg};
        Object* ignored = nullptr;
        return rt.Invoke(worldUpdateAoi, aoi, params, &ignored);
    }
};

}  // namespace cameratools

#include "cabbird/plugin_support/spatial.hpp"
#include "cabbird/sdk/services/game_lifecycle.h"
#include "cabbird/sdk/services/interop.h"
#include "cabbird/sdk/services/player_motion.h"
#include "cabbird/sdk/services/unity.h"
#include "cabbird/sdk/services/unity_metadata.h"

#include "camera_math.hpp"
#include "camera_runtime.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace {

using namespace cameratools;
using cabbird::plugins::ConfigStore;
using cabbird::plugins::Json;
using cabbird::plugins::NumberSchema;
using cabbird::plugins::ObjectSchema;
using cabbird::plugins::PlayerValid;
using cabbird::plugins::Query;
using cabbird::plugins::Range;
using cabbird::plugins::ReadPlayer;
using cabbird::plugins::Status;
using cabbird::plugins::Toggle;
using cabbird::plugins::ToggleSchema;
using cabbird::plugins::ValidVector;
using cabbird::sdk::Ok;
using cabbird::sdk::StringView;
using cabbird::sdk::Succeeded;
using Object = CabbirdIl2CppObjectV1;
using Method = CabbirdMethodInfoV1;
using Runtime = cabbird::sdk::il2cpp::Context;

constexpr const char* kSettingsId = "camera-tools-settings";
constexpr std::uint32_t kDefaultToggleKey = VK_F7;
constexpr std::uint32_t kDefaultTeleportKey = static_cast<std::uint32_t>('1');

#define HAS_UI_FIELD(uiPtr, fieldName) \
    ((uiPtr) != nullptr && \
     (uiPtr)->struct_size >= offsetof(CabbirdUiServiceV1, fieldName) + sizeof((uiPtr)->fieldName) && \
     (uiPtr)->fieldName != nullptr)

struct Settings {
    double distance{};
    double fov{};
    int freeCamera{};
    int lodFollow{1};
    float speed{kDefaultSpeed};
    std::uint32_t toggleKey{kDefaultToggleKey};
    std::uint32_t teleportKey{kDefaultTeleportKey};

    bool operator==(const Settings&) const = default;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Settings,
    distance, fov, freeCamera, lodFollow, speed, toggleKey, teleportKey)

Json SettingsSchema() {
    return ObjectSchema({
        {"distance", NumberSchema(kMinExtraDistance, kMaxExtraDistance)},
        {"fov", NumberSchema(0.0, kMaxFov)},
        {"freeCamera", ToggleSchema()},
        {"lodFollow", ToggleSchema()},
        {"speed", NumberSchema(kMinSpeed, kMaxSpeed)},
        {"toggleKey", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}}},
        {"teleportKey", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}}}
    });
}

bool SettingsValid(const Settings& s) noexcept {
    return IsValidDistanceSetting(s.distance) &&
           IsValidFovSetting(s.fov) &&
           Toggle(s.freeCamera) &&
           Toggle(s.lodFollow) &&
           Range(s.speed, kMinSpeed, kMaxSpeed) &&
           s.toggleKey <= 255U &&
           s.teleportKey <= 255U;
}

enum class CaptureTarget : int {
    None = 0,
    ToggleKey,
    TeleportKey
};

struct HookSlot {
    CabbirdGenerationHandleV1 handle{};
    std::atomic<std::uintptr_t> original{0};
    bool installed{};
};

struct HookLease {
    const CabbirdHookServiceV1* hooks{};
    CabbirdGenerationHandleV1 lease{};

    HookLease(const CabbirdHookServiceV1* service, CabbirdGenerationHandleV1 hookHandle) noexcept
        : hooks(service) {
        if (hooks && hooks->begin_callback && hookHandle.id != 0) {
            if (!Succeeded(hooks->begin_callback(hooks->user, hookHandle, &lease))) {
                lease = {};
            }
        }
    }
    ~HookLease() noexcept {
        if (hooks && hooks->end_callback && lease.id != 0) {
            static_cast<void>(hooks->end_callback(hooks->user, lease));
        }
    }
    HookLease(const HookLease&) = delete;
    HookLease& operator=(const HookLease&) = delete;

    explicit operator bool() const noexcept { return lease.id != 0; }
};

struct Context {
    explicit Context(const CabbirdHostApiV1* host) : text(host) {}

    cabbird::sdk::Localizer text;
    ConfigStore config;
    Settings settings;
    std::mutex settingsMutex;  // Guards settings, hotkey capture, and hotkey handles.

    // Host services.
    const CabbirdCoreServiceV1* core{};
    const CabbirdUiServiceV1* ui{};
    const CabbirdWindowServiceV1* windows{};
    const CabbirdInputServiceV1* input{};
    const CabbirdIl2CppServiceV1* il2cpp{};
    const CabbirdUnityMetadataServiceV1* metadata{};
    const CabbirdUnityPlayerServiceV1* player{};
    const CabbirdUnityPlayerTeleportServiceV1* teleport{};
    const CabbirdUnityPlayerMotionServiceV1* motion{};
    const CabbirdGameLifecycleServiceV1* lifecycle{};
    const CabbirdHookServiceV1* hooks{};

    // Host handles.
    CabbirdGenerationHandleV1 window{};
    CabbirdGenerationHandleV1 cleanup{};
    CabbirdGenerationHandleV1 toggleHotkey{};
    CabbirdGenerationHandleV1 teleportHotkey{};
    CabbirdGenerationHandleV1 motionLease{};
    std::uint64_t motionEntity{};

    // Hotkey capture state (Render thread under settingsMutex).
    CaptureTarget captureTarget{CaptureTarget::None};
    bool hotkeyError{};
    CabbirdInputSnapshotV1 captureInput{sizeof(captureInput)};

    // Lock-free mirrors consumed by Game thread and detours.
    std::atomic_bool running{false};
    std::atomic<double> distance{0.0};
    std::atomic<double> fov{0.0};
    std::atomic_bool freeCamera{false};
    std::atomic_bool lodFollow{true};
    std::atomic<float> speed{kDefaultSpeed};
    std::atomic_bool appFocused{false};
    std::atomic_bool developerMode{false};
    std::atomic_bool allowMoveCancelPass{false};
    std::atomic<std::uint32_t> reportedLogBits{0};

    // Published live observations for UI / teleport (written on Game thread, read on Render).
    std::mutex stateMutex;
    double observedGameFov{0.0};
    bool hasObservedGameFov{false};
    Vec3 lastCameraPos{};
    bool hasLastCameraPos{false};
    bool playerLive{false};
    std::uint64_t playerEntityId{0};
    std::uint64_t playerGeneration{0};
    Vec3 lastPlayerPos{};

    // Teleport queue & verification state.
    std::atomic_bool teleportRequested{false};
    std::atomic_bool teleportBusy{false};
    bool teleportPending{false};
    std::uint64_t teleportTargetEntity{0};
    std::uint64_t teleportTargetScene{0};
    std::uint32_t teleportBaselineSeq{0};

    // Game-thread runtime bindings & cached camera objects (GC-rooted, freed on Stop/Cleanup).
    CameraBindings bindings;
    std::chrono::steady_clock::time_point nextBindAttempt{};
    bool hooksInstalledAttempted{false};
    CabbirdIl2CppGCHandleV1 cachedCamHandle{};
    CabbirdIl2CppGCHandleV1 cachedCamTfHandle{};
    std::atomic<Object*> fastCamPtr{nullptr};
    std::atomic<Object*> fastCamTfPtr{nullptr};
    std::atomic_bool urpRenderObserved{false};

    // Free-camera state (Game thread only).
    bool freeCamInitialized{false};
    Vec3 freeCamPos{};
    std::chrono::steady_clock::time_point lastFreeCamTick{};
    bool hasLastFreeCamTick{false};

    // Fallback FOV restoration state (used only when URP hook is inactive).
    bool fallbackFovModified{false};
    float fallbackSavedFov{0.f};

    // AOI follow state (Game thread only).
    bool aoiModifiedByFreeCam{false};
    bool hasLastAoiPos{false};
    Vec3 lastAoiPos{};
    std::chrono::steady_clock::time_point lastAoiUpdate{};

    // Hook slots.
    HookSlot hookUrpRender;
    HookSlot hookInputMovement;
    HookSlot hookInputJump;
    HookSlot hookInputEvade;
    HookSlot hookInputFlyUp;
    HookSlot hookInputFlyFall;
    HookSlot hookInputFlySprint;
    HookSlot hookInputSwimSprint;
};

std::atomic<Context*> g_activeContext{nullptr};

void LogOnceText(Context& c, std::uint32_t bit, std::string_view message) noexcept {
    try {
        if ((c.reportedLogBits.fetch_or(bit) & bit) == 0 && c.core && c.core->log) {
            c.core->log(c.core->user, CABBIRD_CORE_LOG_LEVEL_V1_WARNING, StringView(message));
        }
    } catch (...) {}
}

void LogWarning(Context& c, std::string_view message) noexcept {
    try {
        if (c.core && c.core->log) {
            c.core->log(c.core->user, CABBIRD_CORE_LOG_LEVEL_V1_WARNING, StringView(message));
        }
    } catch (...) {}
}

bool KeyDown(const CabbirdInputSnapshotV1& snap, unsigned vk) noexcept {
    return vk < 256U && (snap.keys[vk / 8U] & (1U << (vk % 8U))) != 0;
}

bool ReleaseMotionHold(Context& c) noexcept {
    if (!c.motionLease.id) return true;
    if (!c.motion || !c.motion->release) {
        c.motionLease = {};
        c.motionEntity = 0;
        return true;
    }
    const auto status = c.motion->release(c.motion->user, c.motionLease);
    if (!Succeeded(status) && status.code != CABBIRD_STATUS_V1_NOT_FOUND) return false;
    c.motionLease = {};
    c.motionEntity = 0;
    return true;
}

void UpdateMotionHold(Context& c, const CabbirdUnityPlayerSnapshotV1& player, bool wantHold) noexcept {
    if (!wantHold || !c.motion || !c.motion->acquire || !c.motion->update || !PlayerValid(player)) {
        static_cast<void>(ReleaseMotionHold(c));
        return;
    }
    if (c.motionLease.id && c.motionEntity != player.entity_id) {
        if (!ReleaseMotionHold(c)) return;
    }
    CabbirdUnityPlayerMotionRequestV1 req{sizeof(req)};
    req.mode = CABBIRD_UNITY_PLAYER_MOTION_V1_HOLD_POSITION;
    req.flags = 0;
    req.entity_id = player.entity_id;
    const auto st = c.motionLease.id
                        ? c.motion->update(c.motion->user, c.motionLease, &req)
                        : c.motion->acquire(c.motion->user, &req, &c.motionLease);
    if (Succeeded(st)) {
        c.motionEntity = player.entity_id;
    } else {
        static_cast<void>(ReleaseMotionHold(c));
    }
}

void SyncAtomicSettings(Context& c) noexcept {
    const bool prevFree = c.freeCamera.load(std::memory_order_relaxed);
    const bool nextFree = c.settings.freeCamera != 0;
    c.distance.store(c.settings.distance, std::memory_order_relaxed);
    c.fov.store(c.settings.fov, std::memory_order_relaxed);
    c.freeCamera.store(nextFree, std::memory_order_relaxed);
    c.lodFollow.store(c.settings.lodFollow != 0, std::memory_order_relaxed);
    c.speed.store(c.settings.speed, std::memory_order_relaxed);
    if (!prevFree && nextFree) {
        c.allowMoveCancelPass.store(true, std::memory_order_relaxed);
    } else if (!nextFree) {
        c.allowMoveCancelPass.store(false, std::memory_order_relaxed);
    }
}

// Caller holds settingsMutex while callbacks are active.
void SaveSettings(Context& c) {
    SyncAtomicSettings(c);
    if (!c.config.Queue(Json(c.settings))) {
        LogOnceText(c, 1U << 2, c.text.Text("log.save", "Camera Tools: settings could not be queued for saving."));
    }
}

void CABBIRD_CALL OnToggleHotkey(void* raw, CabbirdGenerationHandleV1 handle,
                                 const CabbirdInputSnapshotV1*) noexcept {
    auto& c = *static_cast<Context*>(raw);
    try {
        std::scoped_lock lock(c.settingsMutex);
        if (!c.running.load() || c.captureTarget != CaptureTarget::None ||
            handle.id != c.toggleHotkey.id) {
            return;
        }
        c.settings.freeCamera = c.settings.freeCamera ? 0 : 1;
        SaveSettings(c);
    } catch (...) {
        LogOnceText(c, 1U << 4, c.text.Text("log.hotkey", "Camera Tools: hotkey callback failed."));
    }
}

void CABBIRD_CALL OnTeleportHotkey(void* raw, CabbirdGenerationHandleV1 handle,
                                   const CabbirdInputSnapshotV1*) noexcept {
    auto& c = *static_cast<Context*>(raw);
    try {
        std::scoped_lock lock(c.settingsMutex);
        if (!c.running.load() || c.captureTarget != CaptureTarget::None ||
            handle.id != c.teleportHotkey.id) {
            return;
        }
        if (!c.developerMode.load(std::memory_order_relaxed)) {
            return;
        }
        c.teleportRequested.store(true, std::memory_order_release);
    } catch (...) {
        LogOnceText(c, 1U << 4, c.text.Text("log.hotkey", "Camera Tools: hotkey callback failed."));
    }
}

CabbirdStatusV1 BindSingleHotkey(Context& c, CabbirdGenerationHandleV1& slot,
                                 const char* idPrefix, std::uint32_t vk,
                                 CabbirdHotkeyCallbackV1 cb) {
    CabbirdGenerationHandleV1 next{};
    if (vk != 0) {
        const std::string id = std::string(idPrefix) + "." + std::to_string(vk);
        CabbirdHotkeySpecV1 spec{sizeof(spec)};
        spec.id = StringView(id);
        spec.virtual_key = vk;
        spec.flags = CABBIRD_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS;
        const auto status = c.input->register_hotkey(c.input->user, &spec, cb, &c, &next);
        if (!Succeeded(status)) return status;
    }
    if (slot.id != 0) {
        const auto status = c.input->release_hotkey(c.input->user, slot);
        if (!Succeeded(status) && status.code != CABBIRD_STATUS_V1_NOT_FOUND) {
            if (next.id != 0) static_cast<void>(c.input->release_hotkey(c.input->user, next));
            return status;
        }
    }
    slot = next;
    return Ok();
}

std::string FormatVirtualKey(const Context& c, std::uint32_t key) {
    if (key == 0) return c.text.Text("hotkey.none", "Unassigned");
    const auto scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
    const auto parameter = static_cast<LONG>(((scan & 0xFFU) << 16U) | ((scan & 0xFF00U) ? (1U << 24U) : 0U));
    wchar_t name[64]{};
    if (GetKeyNameTextW(parameter, name, static_cast<int>(std::size(name))) > 0) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
        if (size > 1) {
            std::string result(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, name, -1, result.data(), size, nullptr, nullptr);
            result.pop_back();
            return result;
        }
    }
    const std::string keyText = std::to_string(key);
    const std::array<std::string_view, 1> args{keyText};
    return c.text.Format("hotkey.key", "Key {0}", args);
}

FreeCameraInput SampleFreeCameraInput(const Context& c) noexcept {
    FreeCameraInput out{};
    out.appFocused = c.appFocused.load(std::memory_order_relaxed);
    if (!c.input || !c.input->snapshot) return out;
    CabbirdInputSnapshotV1 snap{sizeof(snap)};
    if (!Succeeded(c.input->snapshot(c.input->user, &snap))) return out;
    out.uiCapturesKeyboard =
        (snap.capture_flags & (CABBIRD_INPUT_CAPTURE_V1_KEYBOARD | CABBIRD_INPUT_CAPTURE_V1_TEXT)) != 0;
    out.forward  = KeyDown(snap, 'W');
    out.backward = KeyDown(snap, 'S');
    out.right    = KeyDown(snap, 'D');
    out.left     = KeyDown(snap, 'A');
    out.up       = KeyDown(snap, VK_SPACE);
    out.down     = KeyDown(snap, VK_SHIFT) || KeyDown(snap, VK_LSHIFT) || KeyDown(snap, VK_RSHIFT);
    out.boost    = KeyDown(snap, VK_CONTROL) || KeyDown(snap, VK_LCONTROL) || KeyDown(snap, VK_RCONTROL);
    return out;
}

// ============================================================================
// Game-thread Hook Detours
// ============================================================================

void CABBIRD_CALL DetourUrpRender(Object* self, std::uintptr_t renderCtx,
                                  Object* cameras, const Method* mi) noexcept {
    Context* c = g_activeContext.load(std::memory_order_acquire);
    if (!c) return;
    const auto origAddr = c->hookUrpRender.original.load(std::memory_order_acquire);
    const auto orig = reinterpret_cast<UrpRenderFn>(origAddr);
    if (!orig) return;

    HookLease lease(c->hooks, c->hookUrpRender.handle);
    if (!lease || !c->running.load(std::memory_order_acquire)) {
        orig(self, renderCtx, cameras, mi);
        return;
    }

    c->urpRenderObserved.store(true, std::memory_order_relaxed);

    Object* cam = c->fastCamPtr.load(std::memory_order_acquire);
    Object* camTf = c->fastCamTfPtr.load(std::memory_order_acquire);
    const bool directOk = c->bindings.nativeDirectVerified &&
                          cam && camTf &&
                          c->bindings.IsUnityObjectAliveFast(cam) &&
                          c->bindings.IsUnityObjectAliveFast(camTf);
    if (!directOk) {
        orig(self, renderCtx, cameras, mi);
        return;
    }

    UnityVec3 origPos{};
    UnityQuat origRot{};
    float origFov = 0.f;
    const bool gotPos = c->bindings.CallNativeGetPos(camTf, origPos);
    const bool gotRot = c->bindings.CallNativeGetRot(camTf, origRot);
    const bool gotFov = seh::CallCameraGetFov(
        reinterpret_cast<NativeCameraGetFovFn>(c->bindings.nativeGetFov.address),
        cam, c->bindings.nativeGetFov.method, origFov);

    const bool freeActive = c->freeCamera.load(std::memory_order_relaxed);
    const double extraDist = c->distance.load(std::memory_order_relaxed);
    const double fovOverride = c->fov.load(std::memory_order_relaxed);

    bool posModified = false;
    bool fovModified = false;
    Vec3 visiblePos = gotPos ? FromUnity(origPos) : Vec3{};

    try {
        if (gotPos && gotRot) {
            const Vec3 basePos = FromUnity(origPos);
            const Quat liveRot = FromUnity(origRot);
            if (freeActive) {
                const auto now = std::chrono::steady_clock::now();
                if (!c->freeCamInitialized) {
                    c->freeCamPos = ApplyExtraViewDistance(basePos, liveRot, extraDist);
                    c->freeCamInitialized = true;
                    c->lastFreeCamTick = now;
                    c->hasLastFreeCamTick = true;
                } else {
                    const double dt = c->hasLastFreeCamTick
                                          ? std::chrono::duration<double>(now - c->lastFreeCamTick).count()
                                          : 0.0;
                    c->lastFreeCamTick = now;
                    c->hasLastFreeCamTick = true;
                    const FreeCameraInput moveInput = SampleFreeCameraInput(*c);
                    const Vec3 step = ComputeFreeCameraStep(
                        liveRot, moveInput, static_cast<double>(c->speed.load(std::memory_order_relaxed)), dt);
                    const Vec3 nextPos = c->freeCamPos + step;
                    if (nextPos.IsFinite()) {
                        c->freeCamPos = nextPos;
                    }
                }
                UnityVec3 desired = ToUnity(c->freeCamPos);
                if (seh::CallTransformVec3(
                        reinterpret_cast<NativeTransformVec3Fn>(c->bindings.nativeSetPos.address),
                        camTf, &desired, c->bindings.nativeSetPos.method)) {
                    posModified = true;
                    visiblePos = c->freeCamPos;
                }
            } else {
                c->freeCamInitialized = false;
                c->hasLastFreeCamTick = false;
                if (extraDist > 1e-4) {
                    const Vec3 offsetPos = ApplyExtraViewDistance(basePos, liveRot, extraDist);
                    UnityVec3 desired = ToUnity(offsetPos);
                    if (seh::CallTransformVec3(
                            reinterpret_cast<NativeTransformVec3Fn>(c->bindings.nativeSetPos.address),
                            camTf, &desired, c->bindings.nativeSetPos.method)) {
                        posModified = true;
                        visiblePos = offsetPos;
                    }
                }
            }
        }

        if (gotFov && fovOverride >= kMinFov && fovOverride <= kMaxFov) {
            if (seh::CallCameraSetFov(
                    reinterpret_cast<NativeCameraSetFovFn>(c->bindings.nativeSetFov.address),
                    cam, static_cast<float>(fovOverride), c->bindings.nativeSetFov.method)) {
                fovModified = true;
            }
        }

        {
            std::scoped_lock lock(c->stateMutex);
            if (gotFov && origFov >= 1.f && origFov <= 179.f) {
                c->observedGameFov = static_cast<double>(origFov);
                c->hasObservedGameFov = true;
            }
            if (gotPos && visiblePos.IsFinite()) {
                c->lastCameraPos = visiblePos;
                c->hasLastCameraPos = true;
            }
        }
    } catch (...) {}

    // Render the entire frame with our applied camera position and FOV.
    orig(self, renderCtx, cameras, mi);

    // Immediately restore original camera transform and FOV so Cinemachine state,
    // dynamic FOV transitions, and static cameras never accumulate or drift.
    if (posModified) {
        seh::CallTransformVec3(
            reinterpret_cast<NativeTransformVec3Fn>(c->bindings.nativeSetPos.address),
            camTf, &origPos, c->bindings.nativeSetPos.method);
    }
    if (fovModified) {
        seh::CallCameraSetFov(
            reinterpret_cast<NativeCameraSetFovFn>(c->bindings.nativeSetFov.address),
            cam, origFov, c->bindings.nativeSetFov.method);
    }
}

void CABBIRD_CALL DetourInputMovement(Object* self, Object* inputValue, const Method* mi) noexcept {
    Context* c = g_activeContext.load(std::memory_order_acquire);
    if (!c) return;
    const auto orig = reinterpret_cast<InstanceVoid1ObjFn>(c->hookInputMovement.original.load(std::memory_order_acquire));
    if (!orig) return;
    HookLease lease(c->hooks, c->hookInputMovement.handle);
    if (lease && c->running.load(std::memory_order_acquire) &&
        c->freeCamera.load(std::memory_order_relaxed)) {
        if (c->allowMoveCancelPass.load(std::memory_order_relaxed) && c->input && c->input->snapshot) {
            CabbirdInputSnapshotV1 snap{sizeof(snap)};
            if (Succeeded(c->input->snapshot(c->input->user, &snap)) &&
                !KeyDown(snap, 'W') && !KeyDown(snap, 'A') &&
                !KeyDown(snap, 'S') && !KeyDown(snap, 'D')) {
                c->allowMoveCancelPass.store(false, std::memory_order_relaxed);
                orig(self, inputValue, mi);
                return;
            }
        }
        return;
    }
    orig(self, inputValue, mi);
}

void CABBIRD_CALL DetourInputJump(Object* self, const Method* mi) noexcept {
    Context* c = g_activeContext.load(std::memory_order_acquire);
    if (!c) return;
    const auto orig = reinterpret_cast<InstanceVoid0Fn>(c->hookInputJump.original.load(std::memory_order_acquire));
    if (!orig) return;
    HookLease lease(c->hooks, c->hookInputJump.handle);
    if (lease && c->running.load(std::memory_order_acquire) &&
        c->freeCamera.load(std::memory_order_relaxed)) {
        return;
    }
    orig(self, mi);
}

void CABBIRD_CALL DetourInputEvade(Object* self, Object* inputValue, const Method* mi) noexcept {
    Context* c = g_activeContext.load(std::memory_order_acquire);
    if (!c) return;
    const auto orig = reinterpret_cast<InstanceVoid1ObjFn>(c->hookInputEvade.original.load(std::memory_order_acquire));
    if (!orig) return;
    HookLease lease(c->hooks, c->hookInputEvade.handle);
    if (lease && c->running.load(std::memory_order_acquire) &&
        c->freeCamera.load(std::memory_order_relaxed)) {
        return;
    }
    orig(self, inputValue, mi);
}

void CABBIRD_CALL DetourInputFlyUp(Object* self, Object* inputValue, const Method* mi) noexcept {
    Context* c = g_activeContext.load(std::memory_order_acquire);
    if (!c) return;
    const auto orig = reinterpret_cast<InstanceVoid1ObjFn>(c->hookInputFlyUp.original.load(std::memory_order_acquire));
    if (!orig) return;
    HookLease lease(c->hooks, c->hookInputFlyUp.handle);
    if (lease && c->running.load(std::memory_order_acquire) &&
        c->freeCamera.load(std::memory_order_relaxed)) {
        return;
    }
    orig(self, inputValue, mi);
}

void CABBIRD_CALL DetourInputFlyFall(Object* self, Object* inputValue, const Method* mi) noexcept {
    Context* c = g_activeContext.load(std::memory_order_acquire);
    if (!c) return;
    const auto orig = reinterpret_cast<InstanceVoid1ObjFn>(c->hookInputFlyFall.original.load(std::memory_order_acquire));
    if (!orig) return;
    HookLease lease(c->hooks, c->hookInputFlyFall.handle);
    if (lease && c->running.load(std::memory_order_acquire) &&
        c->freeCamera.load(std::memory_order_relaxed)) {
        return;
    }
    orig(self, inputValue, mi);
}

void CABBIRD_CALL DetourInputFlySprint(Object* self, Object* inputValue, const Method* mi) noexcept {
    Context* c = g_activeContext.load(std::memory_order_acquire);
    if (!c) return;
    const auto orig = reinterpret_cast<InstanceVoid1ObjFn>(c->hookInputFlySprint.original.load(std::memory_order_acquire));
    if (!orig) return;
    HookLease lease(c->hooks, c->hookInputFlySprint.handle);
    if (lease && c->running.load(std::memory_order_acquire) &&
        c->freeCamera.load(std::memory_order_relaxed)) {
        return;
    }
    orig(self, inputValue, mi);
}

void CABBIRD_CALL DetourInputSwimSprint(Object* self, Object* inputValue, const Method* mi) noexcept {
    Context* c = g_activeContext.load(std::memory_order_acquire);
    if (!c) return;
    const auto orig = reinterpret_cast<InstanceVoid1ObjFn>(c->hookInputSwimSprint.original.load(std::memory_order_acquire));
    if (!orig) return;
    HookLease lease(c->hooks, c->hookInputSwimSprint.handle);
    if (lease && c->running.load(std::memory_order_acquire) &&
        c->freeCamera.load(std::memory_order_relaxed)) {
        return;
    }
    orig(self, inputValue, mi);
}

void InstallHook(Context& c, HookSlot& slot, std::uintptr_t target, void* detour, const char* label) noexcept {
    if (slot.installed || !target || !c.hooks || !c.hooks->create) return;
    CabbirdHookRequestV1 req{sizeof(req)};
    req.kind = CABBIRD_HOOK_V1_FUNCTION;
    req.target = target;
    req.detour = detour;
    req.label = StringView(label);
    std::uintptr_t orig = 0;
    CabbirdGenerationHandleV1 handle{};
    if (Succeeded(c.hooks->create(c.hooks->user, &req, &orig, &handle)) && orig && handle.id) {
        slot.original.store(orig, std::memory_order_release);
        slot.handle = handle;
        slot.installed = true;
    }
}

void ReleaseHook(Context& c, HookSlot& slot) noexcept {
    if (!slot.installed) return;
    if (c.hooks && c.hooks->release && slot.handle.id) {
        static_cast<void>(c.hooks->release(c.hooks->user, slot.handle));
    }
    slot.handle = {};
    slot.original.store(0, std::memory_order_release);
    slot.installed = false;
}

void InstallAllHooks(Context& c) noexcept {
    if (c.hooksInstalledAttempted || !c.hooks || !c.hooks->create) return;
    c.hooksInstalledAttempted = true;
    InstallHook(c, c.hookUrpRender,       c.bindings.addrUrpRender,       reinterpret_cast<void*>(&DetourUrpRender),       "camera-tools.urp-render");
    InstallHook(c, c.hookInputMovement,   c.bindings.addrInputMovement,   reinterpret_cast<void*>(&DetourInputMovement),   "camera-tools.input-movement");
    InstallHook(c, c.hookInputJump,       c.bindings.addrInputJump,       reinterpret_cast<void*>(&DetourInputJump),       "camera-tools.input-jump");
    InstallHook(c, c.hookInputEvade,      c.bindings.addrInputEvade,      reinterpret_cast<void*>(&DetourInputEvade),      "camera-tools.input-evade");
    InstallHook(c, c.hookInputFlyUp,      c.bindings.addrInputFlyUp,      reinterpret_cast<void*>(&DetourInputFlyUp),      "camera-tools.input-fly-up");
    InstallHook(c, c.hookInputFlyFall,    c.bindings.addrInputFlyFall,    reinterpret_cast<void*>(&DetourInputFlyFall),    "camera-tools.input-fly-fall");
    InstallHook(c, c.hookInputFlySprint,  c.bindings.addrInputFlySprint,  reinterpret_cast<void*>(&DetourInputFlySprint),  "camera-tools.input-fly-sprint");
    InstallHook(c, c.hookInputSwimSprint, c.bindings.addrInputSwimSprint, reinterpret_cast<void*>(&DetourInputSwimSprint), "camera-tools.input-swim-sprint");
}

void ReleaseAllHooks(Context& c) noexcept {
    ReleaseHook(c, c.hookUrpRender);
    ReleaseHook(c, c.hookInputMovement);
    ReleaseHook(c, c.hookInputJump);
    ReleaseHook(c, c.hookInputEvade);
    ReleaseHook(c, c.hookInputFlyUp);
    ReleaseHook(c, c.hookInputFlyFall);
    ReleaseHook(c, c.hookInputFlySprint);
    ReleaseHook(c, c.hookInputSwimSprint);
    c.hooksInstalledAttempted = false;
}

void UpdateCachedCameraHandles(Context& c, const Runtime& rt, Object* cam, Object* camTf) noexcept {
    const auto& api = rt.Functions();
    Object* curCam = (c.cachedCamHandle && api.il2cpp_gchandle_get_target)
                         ? api.il2cpp_gchandle_get_target(c.cachedCamHandle)
                         : nullptr;
    if (curCam != cam) {
        if (c.cachedCamHandle && api.il2cpp_gchandle_free) {
            api.il2cpp_gchandle_free(c.cachedCamHandle);
            c.cachedCamHandle = nullptr;
        }
        if (cam && api.il2cpp_gchandle_new) {
            c.cachedCamHandle = api.il2cpp_gchandle_new(cam, false);
        }
    }
    Object* curTf = (c.cachedCamTfHandle && api.il2cpp_gchandle_get_target)
                        ? api.il2cpp_gchandle_get_target(c.cachedCamTfHandle)
                        : nullptr;
    if (curTf != camTf) {
        if (c.cachedCamTfHandle && api.il2cpp_gchandle_free) {
            api.il2cpp_gchandle_free(c.cachedCamTfHandle);
            c.cachedCamTfHandle = nullptr;
        }
        if (camTf && api.il2cpp_gchandle_new) {
            c.cachedCamTfHandle = api.il2cpp_gchandle_new(camTf, false);
        }
    }
    c.fastCamPtr.store(cam, std::memory_order_release);
    c.fastCamTfPtr.store(camTf, std::memory_order_release);
}

void FreeCachedCameraHandles(Context& c, const CabbirdIl2CppApiV1* api) noexcept {
    c.fastCamPtr.store(nullptr, std::memory_order_release);
    c.fastCamTfPtr.store(nullptr, std::memory_order_release);
    if (!api || !api->il2cpp_gchandle_free) {
        c.cachedCamHandle = nullptr;
        c.cachedCamTfHandle = nullptr;
        return;
    }
    if (c.cachedCamHandle) {
        api->il2cpp_gchandle_free(c.cachedCamHandle);
        c.cachedCamHandle = nullptr;
    }
    if (c.cachedCamTfHandle) {
        api->il2cpp_gchandle_free(c.cachedCamTfHandle);
        c.cachedCamTfHandle = nullptr;
    }
}

// ============================================================================
// Game-Domain Update & Cleanup
// ============================================================================

struct TickWork {
    Context* c{};
    double deltaSeconds{};
    bool playerLive{};
    Vec3 playerPos{};
};

CabbirdStatusV1 CABBIRD_CALL GameTickRuntime(void* raw, const CabbirdIl2CppApiV1* api) noexcept {
    try {
        if (!api || api->struct_size < sizeof(*api) || api->api_version != 1) {
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        }
        auto& work = *static_cast<TickWork*>(raw);
        auto& c = *work.c;
        Runtime rt(*api);

        if (!c.bindings.coreReady) {
            const auto now = std::chrono::steady_clock::now();
            if (now < c.nextBindAttempt) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
            c.nextBindAttempt = now + std::chrono::seconds(2);
            if (!c.bindings.Bind(c.metadata, rt)) {
                LogOnceText(c, 1U << 1, c.text.Text("log.bind", "Camera Tools: game camera runtime binding is not ready yet."));
                return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
            }
            if (!c.bindings.bindDiagnostics.empty()) {
                const std::array<std::string_view, 1> args{c.bindings.bindDiagnostics};
                LogOnceText(c, 1U << 7, c.text.Format("log.bind_detail",
                                                      "Camera Tools: runtime binding diagnostic ({0}).", args));
            }
            InstallAllHooks(c);
        }

        c.appFocused.store(c.bindings.ReadAppFocused(rt), std::memory_order_relaxed);

        Object* cam = c.bindings.ResolveMainCamera(rt);
        Object* camTf = cam ? c.bindings.ResolveComponentTransform(rt, cam) : nullptr;
        UpdateCachedCameraHandles(c, rt, cam, camTf);
        if (cam && camTf) {
            c.bindings.EnsureNativeDirectVerified(rt, cam, camTf);
        }

        // Fallback path when URP render hook or direct native path is inactive.
        const bool urpActive = c.hookUrpRender.installed &&
                               c.urpRenderObserved.load(std::memory_order_relaxed) &&
                               c.bindings.nativeDirectVerified;
        if (!urpActive && cam && camTf) {
            UnityVec3 pos{};
            UnityQuat rot{};
            float liveFov = 0.f;
            const bool gotPos = c.bindings.ReadManagedPos(rt, camTf, pos);
            const bool gotRot = c.bindings.ReadManagedRot(rt, camTf, rot);
            const bool gotFov = c.bindings.ReadManagedFov(rt, cam, liveFov);
            const double fovOverride = c.fov.load(std::memory_order_relaxed);
            if (gotFov) {
                if (!c.fallbackFovModified) {
                    c.fallbackSavedFov = liveFov;
                    std::scoped_lock lock(c.stateMutex);
                    c.observedGameFov = static_cast<double>(liveFov);
                    c.hasObservedGameFov = true;
                }
                if (fovOverride >= kMinFov && fovOverride <= kMaxFov) {
                    if (c.bindings.WriteManagedFov(rt, cam, static_cast<float>(fovOverride))) {
                        c.fallbackFovModified = true;
                    }
                } else if (c.fallbackFovModified) {
                    c.bindings.WriteManagedFov(rt, cam, c.fallbackSavedFov);
                    c.fallbackFovModified = false;
                }
            }
            if (gotPos && gotRot) {
                const Vec3 basePos = FromUnity(pos);
                const Quat liveRot = FromUnity(rot);
                Vec3 visiblePos = basePos;
                if (c.freeCamera.load(std::memory_order_relaxed)) {
                    if (!c.freeCamInitialized) {
                        c.freeCamPos = ApplyExtraViewDistance(basePos, liveRot, c.distance.load(std::memory_order_relaxed));
                        c.freeCamInitialized = true;
                    } else {
                        const FreeCameraInput moveInput = SampleFreeCameraInput(c);
                        const Vec3 step = ComputeFreeCameraStep(
                            liveRot, moveInput, static_cast<double>(c.speed.load(std::memory_order_relaxed)),
                            work.deltaSeconds);
                        if ((c.freeCamPos + step).IsFinite()) {
                            c.freeCamPos = c.freeCamPos + step;
                        }
                    }
                    c.bindings.WriteManagedPos(rt, camTf, ToUnity(c.freeCamPos));
                    visiblePos = c.freeCamPos;
                } else {
                    c.freeCamInitialized = false;
                }
                std::scoped_lock lock(c.stateMutex);
                c.lastCameraPos = visiblePos;
                c.hasLastCameraPos = true;
            }
        }

        // Scene AOI streaming around Free Camera position.
        const bool freeNow = c.freeCamera.load(std::memory_order_relaxed);
        const bool followNow = c.lodFollow.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        if (freeNow && followNow && c.freeCamInitialized && c.bindings.aoiReady) {
            const double elapsed = c.hasLastAoiPos
                                       ? std::chrono::duration<double>(now - c.lastAoiUpdate).count()
                                       : 1.0;
            if (ShouldUpdateAoi(c.lastAoiPos, c.hasLastAoiPos, c.freeCamPos, elapsed)) {
                if (c.bindings.UpdateAoiPosition(rt, c.freeCamPos)) {
                    c.lastAoiPos = c.freeCamPos;
                    c.hasLastAoiPos = true;
                    c.lastAoiUpdate = now;
                    c.aoiModifiedByFreeCam = true;
                }
            }
        } else if (c.aoiModifiedByFreeCam && c.bindings.aoiReady) {
            if (work.playerLive && work.playerPos.IsFinite()) {
                c.bindings.UpdateAoiPosition(rt, work.playerPos);
            }
            c.aoiModifiedByFreeCam = false;
            c.hasLastAoiPos = false;
        }

        return Ok();
    } catch (...) {
        return Status(CABBIRD_STATUS_V1_FAILED);
    }
}

void PollTeleportState(Context& c, const CabbirdUnityPlayerSnapshotV1& player) {
    if (!c.teleportPending || !c.teleport || !c.teleport->state) return;
    if (!PlayerValid(player) || player.generation != c.teleportTargetScene ||
        player.entity_id != c.teleportTargetEntity) {
        c.teleportPending = false;
        c.teleportBusy.store(false, std::memory_order_relaxed);
        return;
    }
    CabbirdUnityPlayerWriteResultV1 result{sizeof(result)};
    if (!Succeeded(c.teleport->state(c.teleport->user, &result))) return;
    if (result.request_sequence == c.teleportBaselineSeq) return;
    if (result.entity_id != c.teleportTargetEntity) return;
    if (result.result_code == CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_PENDING) return;
    if (result.result_code != CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_OK) {
        const std::string codeText = std::to_string(result.result_code);
        const std::array<std::string_view, 1> args{codeText};
        LogWarning(c, c.text.Format("log.teleport_result",
                                    "Camera Tools: teleport did not hold (result {0}).", args));
    }
    c.teleportPending = false;
    c.teleportBusy.store(false, std::memory_order_relaxed);
}

void ProcessTeleportRequest(Context& c, const CabbirdUnityPlayerSnapshotV1& player) {
    if (!c.teleportRequested.exchange(false, std::memory_order_acq_rel)) return;
    if (!c.developerMode.load(std::memory_order_relaxed)) return;
    if (!c.teleport || !c.teleport->teleport || !c.teleport->state) return;
    if (!PlayerValid(player)) return;
    if (c.teleportBusy.exchange(true, std::memory_order_acq_rel)) return;

    Vec3 target{};
    bool hasTarget = false;
    {
        std::scoped_lock lock(c.stateMutex);
        target = c.lastCameraPos;
        hasTarget = c.hasLastCameraPos;
    }
    double coords[3]{target.x, target.y, target.z};
    if (!hasTarget || !ValidVector(coords)) {
        c.teleportBusy.store(false, std::memory_order_relaxed);
        return;
    }

    CabbirdUnityPlayerWriteResultV1 prev{sizeof(prev)};
    c.teleportBaselineSeq = Succeeded(c.teleport->state(c.teleport->user, &prev))
                                ? prev.request_sequence
                                : 0U;

    CabbirdUnityPlayerWriteRequestV1 req{sizeof(req)};
    req.entity_id = player.entity_id;
    req.flags = CABBIRD_UNITY_PLAYER_WRITE_V1_NONE;
    req.position[0] = coords[0];
    req.position[1] = coords[1];
    req.position[2] = coords[2];
    const auto status = c.teleport->teleport(c.teleport->user, &req);
    if (!Succeeded(status)) {
        const std::string codeText = std::to_string(status.code);
        const std::array<std::string_view, 1> args{codeText};
        LogWarning(c, c.text.Format("log.teleport",
                                    "Camera Tools: teleport to camera position failed (status {0}).", args));
        c.teleportBusy.store(false, std::memory_order_relaxed);
        return;
    }
    c.teleportTargetEntity = player.entity_id;
    c.teleportTargetScene = player.generation;
    c.teleportPending = true;
}

void CABBIRD_CALL Update(void* raw, double deltaSeconds) noexcept {
    auto& c = *static_cast<Context*>(raw);
    try {
        if (!c.running.load(std::memory_order_acquire)) {
            static_cast<void>(ReleaseMotionHold(c));
            return;
        }
        const bool devMode = HAS_UI_FIELD(c.ui, developer_mode_enabled) &&
                             c.ui->developer_mode_enabled(c.ui->user) != 0;
        c.developerMode.store(devMode, std::memory_order_relaxed);

        const auto player = ReadPlayer(c.player);
        const bool live = PlayerValid(player);
        Vec3 playerPos{};
        if (live) {
            playerPos = {player.position[0], player.position[1], player.position[2]};
        }
        {
            std::scoped_lock lock(c.stateMutex);
            c.playerLive = live;
            c.playerEntityId = live ? player.entity_id : 0;
            c.playerGeneration = live ? player.generation : 0;
            if (live) c.lastPlayerPos = playerPos;
        }

        // Hold player position cleanly via shared host motion controller while Free Camera is active.
        UpdateMotionHold(c, player, c.freeCamera.load(std::memory_order_relaxed));

        if (c.il2cpp && c.il2cpp->with_runtime) {
            TickWork work{&c, deltaSeconds, live, playerPos};
            static_cast<void>(c.il2cpp->with_runtime(c.il2cpp->user, GameTickRuntime, &work));
        }

        PollTeleportState(c, player);
        ProcessTeleportRequest(c, player);
    } catch (...) {
        LogOnceText(c, 1U << 6, c.text.Text("log.update", "Camera Tools: Game-domain update failed."));
    }
}

CabbirdStatusV1 CABBIRD_CALL GameCleanupRuntime(void* raw, const CabbirdIl2CppApiV1* api) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        if (!api || api->struct_size < sizeof(*api) || api->api_version != 1) {
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        }
        Runtime rt(*api);

        // Restore fallback FOV if modified without URP hook.
        if (c.fallbackFovModified) {
            if (Object* cam = c.bindings.ResolveMainCamera(rt)) {
                c.bindings.WriteManagedFov(rt, cam, c.fallbackSavedFov);
            }
            c.fallbackFovModified = false;
        }

        // Restore AOI streaming center back to the player if moved by Free Camera.
        if (c.aoiModifiedByFreeCam && c.bindings.aoiReady) {
            Vec3 playerPos{};
            bool hasPlayer = false;
            {
                std::scoped_lock lock(c.stateMutex);
                hasPlayer = c.playerLive && c.lastPlayerPos.IsFinite();
                playerPos = c.lastPlayerPos;
            }
            if (hasPlayer) {
                c.bindings.UpdateAoiPosition(rt, playerPos);
            }
            c.aoiModifiedByFreeCam = false;
            c.hasLastAoiPos = false;
        }

        FreeCachedCameraHandles(c, api);
        c.freeCamInitialized = false;
        c.hasLastFreeCamTick = false;
        return Ok();
    } catch (...) {
        return Status(CABBIRD_STATUS_V1_FAILED);
    }
}

CabbirdStatusV1 CABBIRD_CALL Cleanup(void* raw) noexcept {
    auto& c = *static_cast<Context*>(raw);
    try {
        c.running.store(false, std::memory_order_release);
        static_cast<void>(ReleaseMotionHold(c));
        ReleaseAllHooks(c);
        if (c.il2cpp && c.il2cpp->with_runtime) {
            static_cast<void>(c.il2cpp->with_runtime(c.il2cpp->user, GameCleanupRuntime, &c));
        }
        return Ok();
    } catch (...) {
        return Status(CABBIRD_STATUS_V1_FAILED);
    }
}

// ============================================================================
// Render-Domain UI
// ============================================================================

void PollHotkeyCapture(Context& c) {
    if (c.captureTarget == CaptureTarget::None) return;
    if (!(c.ui->frame_state(c.ui->user) & CABBIRD_UI_FRAME_V1_WINDOW_FOCUSED)) {
        c.captureTarget = CaptureTarget::None;
        return;
    }
    CabbirdInputSnapshotV1 snap{sizeof(snap)};
    if (!Succeeded(c.input->snapshot(c.input->user, &snap))) return;

    for (std::uint32_t vk = VK_BACK; vk <= 255U; ++vk) {
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
            !KeyDown(snap, vk) || KeyDown(c.captureInput, vk)) {
            continue;
        }
        const CaptureTarget target = c.captureTarget;
        c.captureTarget = CaptureTarget::None;
        if (vk == VK_ESCAPE) break;

        const std::uint32_t nextKey = (vk == VK_BACK) ? 0U : vk;
        if (target == CaptureTarget::ToggleKey) {
            if (nextKey != 0 && nextKey == c.settings.teleportKey) {
                static_cast<void>(BindSingleHotkey(c, c.teleportHotkey, "camera-tools.teleport", 0, OnTeleportHotkey));
                c.settings.teleportKey = 0;
            }
            const bool needBind = (nextKey != c.settings.toggleKey) || (nextKey != 0 && c.toggleHotkey.id == 0);
            c.hotkeyError = needBind && !Succeeded(BindSingleHotkey(c, c.toggleHotkey, "camera-tools.toggle", nextKey, OnToggleHotkey));
            if (!c.hotkeyError) c.settings.toggleKey = nextKey;
        } else if (target == CaptureTarget::TeleportKey) {
            if (nextKey != 0 && nextKey == c.settings.toggleKey) {
                static_cast<void>(BindSingleHotkey(c, c.toggleHotkey, "camera-tools.toggle", 0, OnToggleHotkey));
                c.settings.toggleKey = 0;
            }
            const bool needBind = (nextKey != c.settings.teleportKey) || (nextKey != 0 && c.teleportHotkey.id == 0);
            c.hotkeyError = needBind && !Succeeded(BindSingleHotkey(c, c.teleportHotkey, "camera-tools.teleport", nextKey, OnTeleportHotkey));
            if (!c.hotkeyError) c.settings.teleportKey = nextKey;
        }
        break;
    }
    c.captureInput = snap;
}

void DrawHotkeyButton(Context& c, CaptureTarget target, std::string displayText, const char* widgetId) {
    const bool capturingThis = (c.captureTarget == target);
    std::string label = capturingThis
                            ? c.text.Text("hotkey.capture", "Press a key...")
                            : std::move(displayText);
    label += "###";
    label += widgetId;
    if (c.ui->button(c.ui->user, StringView(label), 0.f, 0.f)) {
        if (capturingThis) {
            c.captureTarget = CaptureTarget::None;
        } else if (Succeeded(c.input->snapshot(c.input->user, &c.captureInput))) {
            c.captureTarget = target;
            c.hotkeyError = false;
        }
    }
}

void CABBIRD_CALL Draw(void* raw, const CabbirdUiServiceV1*) noexcept {
    auto& c = *static_cast<Context*>(raw);
    try {
        std::scoped_lock lock(c.settingsMutex);
        CabbirdWindowStateV1 winState{sizeof(winState)};
        if (!Succeeded(c.windows->state(c.windows->user, c.window, &winState)) || !winState.open) {
            c.captureTarget = CaptureTarget::None;
            return;
        }
        std::int32_t visible = 0;
        if (!Succeeded(c.windows->begin(c.windows->user, c.window, 0, &visible))) return;
        struct WindowGuard {
            Context& ctx;
            ~WindowGuard() { static_cast<void>(ctx.windows->end(ctx.windows->user, ctx.window)); }
        } guard{c};
        if (!visible) {
            c.captureTarget = CaptureTarget::None;
            return;
        }

        PollHotkeyCapture(c);

        double liveFov = 0.0;
        bool hasLiveFov = false;
        bool hasCamPos = false;
        bool playerLive = false;
        {
            std::scoped_lock stateLock(c.stateMutex);
            liveFov = c.observedGameFov;
            hasLiveFov = c.hasObservedGameFov;
            hasCamPos = c.hasLastCameraPos;
            playerLive = c.playerLive;
        }

        const Settings before = c.settings;
        const auto* ui = c.ui;

        // Distance and FOV rows in a clean fixed-fit table (no white boxes, no clipping).
        if (ui->begin_table && ui->table_next_row && ui->table_next_column && ui->end_table &&
            ui->begin_table(ui->user, StringView("camera-tools.metrics"), 2,
                            CABBIRD_UI_TABLE_V1_SIZING_FIXED_FIT, 0.f, 0.f)) {
            // Extra view distance row.
            ui->table_next_row(ui->user);
            ui->table_next_column(ui->user);
            if (HAS_UI_FIELD(ui, set_next_item_width)) ui->set_next_item_width(ui->user, 150.f);
            double distVal = c.settings.distance;
            const std::string distLabel = c.text.Label("settings.distance", "Extra view distance", "ct.dist");
            if (ui->input_double(ui->user, StringView(distLabel), &distVal, 0.5, 2.0)) {
                c.settings.distance = NormalizeDistanceSetting(distVal);
            }
            ui->table_next_column(ui->user);
            const std::string resetDistLabel = c.text.Label("action.reset_distance", "Restore game default", "ct.reset-dist");
            if (HAS_UI_FIELD(ui, button_enabled)) {
                if (ui->button_enabled(ui->user, StringView(resetDistLabel), 0.f, 0.f, c.settings.distance > 0.0 ? 1 : 0)) {
                    c.settings.distance = 0.0;
                }
            } else if (ui->button(ui->user, StringView(resetDistLabel), 0.f, 0.f)) {
                c.settings.distance = 0.0;
            }

            // FOV row (0 = game default, displaying live observed game FOV).
            ui->table_next_row(ui->user);
            ui->table_next_column(ui->user);
            if (HAS_UI_FIELD(ui, set_next_item_width)) ui->set_next_item_width(ui->user, 150.f);
            double fovDisplay = (c.settings.fov > 0.0) ? c.settings.fov : (hasLiveFov ? liveFov : 0.0);
            const std::string fovLabel = c.text.Label("settings.fov", "Field of view", "ct.fov");
            if (ui->input_double(ui->user, StringView(fovLabel), &fovDisplay, 1.0, 5.0)) {
                c.settings.fov = NormalizeFovSetting(fovDisplay);
            }
            ui->table_next_column(ui->user);
            const std::string resetFovLabel = c.text.Label("action.reset_fov", "Restore game default", "ct.reset-fov");
            if (HAS_UI_FIELD(ui, button_enabled)) {
                if (ui->button_enabled(ui->user, StringView(resetFovLabel), 0.f, 0.f, c.settings.fov > 0.0 ? 1 : 0)) {
                    c.settings.fov = 0.0;
                }
            } else if (ui->button(ui->user, StringView(resetFovLabel), 0.f, 0.f)) {
                c.settings.fov = 0.0;
            }

            ui->end_table(ui->user);
        }

        // Free Camera section.
        ui->separator(ui->user);
        ui->text(ui->user, StringView(c.text.Text("section.free_camera", "Free camera")));
        ui->checkbox(ui->user, StringView(c.text.Text("settings.free_camera", "Enabled")), &c.settings.freeCamera);
        if (HAS_UI_FIELD(ui, same_line)) ui->same_line(ui->user, 0.f, 12.f);
        {
            const std::string keyName = FormatVirtualKey(c, c.settings.toggleKey);
            const std::array<std::string_view, 1> args{keyName};
            DrawHotkeyButton(c, CaptureTarget::ToggleKey,
                             c.text.Format("hotkey.toggle", "Activation key: {0}", args),
                             "camera-tools.toggle-key");
        }

        ui->checkbox(ui->user, StringView(c.text.Text("settings.lod_follow", "Scene loads around free camera")),
                     &c.settings.lodFollow);

        if (HAS_UI_FIELD(ui, set_next_item_width)) ui->set_next_item_width(ui->user, 180.f);
        const std::string speedLabel = c.text.Label("settings.speed", "Movement speed (m/s)", "ct.speed");
        ui->slider_float(ui->user, StringView(speedLabel), &c.settings.speed, kMinSpeed, kMaxSpeed);
        if (HAS_UI_FIELD(ui, same_line)) ui->same_line(ui->user, 0.f, 8.f);
        const std::string resetSpeedLabel = c.text.Label("action.reset_speed", "Reset speed", "ct.reset-speed");
        if (ui->button(ui->user, StringView(resetSpeedLabel), 0.f, 0.f)) {
            c.settings.speed = kDefaultSpeed;
        }

        const std::string hint = c.text.Text("hint.controls",
                                             "WASD moves, Space goes up, Shift goes down, hold Ctrl to speed up.");
        if (HAS_UI_FIELD(ui, text_wrapped)) {
            ui->text_wrapped(ui->user, StringView(hint));
        } else {
            ui->text(ui->user, StringView(hint));
        }

        if (c.captureTarget != CaptureTarget::None) {
            ui->text(ui->user, StringView(c.text.Text("hotkey.hint", "Backspace clears; Esc cancels.")));
        }
        if (c.hotkeyError) {
            ui->text(ui->user, StringView(c.text.Text("hotkey.error", "Could not bind the hotkey. It may already be in use.")));
        }

        // Developer-only teleport section.
        const bool devMode = HAS_UI_FIELD(ui, developer_mode_enabled) && ui->developer_mode_enabled(ui->user) != 0;
        c.developerMode.store(devMode, std::memory_order_relaxed);
        if (devMode) {
            ui->separator(ui->user);
            ui->text(ui->user, StringView(c.text.Text("section.developer", "Developer tools")));
            const bool canTeleport = hasCamPos && playerLive && c.teleport != nullptr &&
                                     !c.teleportBusy.load(std::memory_order_relaxed);
            const std::string tpLabel = c.text.Label("action.teleport", "Teleport to camera position", "ct.tp");
            if (HAS_UI_FIELD(ui, button_enabled)) {
                if (ui->button_enabled(ui->user, StringView(tpLabel), 0.f, 0.f, canTeleport ? 1 : 0)) {
                    c.teleportRequested.store(true, std::memory_order_release);
                }
            } else if (canTeleport && ui->button(ui->user, StringView(tpLabel), 0.f, 0.f)) {
                c.teleportRequested.store(true, std::memory_order_release);
            }
            if (HAS_UI_FIELD(ui, same_line)) ui->same_line(ui->user, 0.f, 8.f);
            {
                const std::string keyName = FormatVirtualKey(c, c.settings.teleportKey);
                const std::array<std::string_view, 1> args{keyName};
                DrawHotkeyButton(c, CaptureTarget::TeleportKey,
                                 c.text.Format("hotkey.teleport", "Teleport key: {0}", args),
                                 "camera-tools.teleport-key");
            }
        }

        if (c.settings != before && SettingsValid(c.settings)) {
            SaveSettings(c);
        }
    } catch (...) {
        LogOnceText(c, 1U << 5, c.text.Text("log.draw", "Camera Tools: UI draw callback failed."));
    }
}

// ============================================================================
// Plugin Lifecycle (Load / Start / Stop / Unload)
// ============================================================================

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** result) noexcept {
    if (!host || !result) return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    *result = nullptr;
    try {
        auto c = std::make_unique<Context>(host);
        c->core      = Query<CabbirdCoreServiceV1>(host, CABBIRD_CORE_SERVICE_V1_ID);
        c->ui        = Query<CabbirdUiServiceV1>(host, CABBIRD_UI_SERVICE_V1_ID);
        c->windows   = Query<CabbirdWindowServiceV1>(host, CABBIRD_WINDOW_SERVICE_V1_ID);
        c->input     = Query<CabbirdInputServiceV1>(host, CABBIRD_INPUT_SERVICE_V1_ID);
        c->il2cpp    = Query<CabbirdIl2CppServiceV1>(host, CABBIRD_IL2CPP_SERVICE_V1_ID);
        c->metadata  = Query<CabbirdUnityMetadataServiceV1>(host, CABBIRD_UNITY_METADATA_SERVICE_V1_ID);
        c->lifecycle = Query<CabbirdGameLifecycleServiceV1>(host, CABBIRD_GAME_LIFECYCLE_SERVICE_V1_ID);

        // Optional services.
        c->hooks    = Query<CabbirdHookServiceV1>(host, CABBIRD_HOOK_SERVICE_V1_ID);
        c->player   = Query<CabbirdUnityPlayerServiceV1>(host, CABBIRD_UNITY_PLAYER_SERVICE_V1_ID);
        c->teleport = Query<CabbirdUnityPlayerTeleportServiceV1>(host, CABBIRD_UNITY_PLAYER_TELEPORT_SERVICE_V1_ID);
        c->motion   = Query<CabbirdUnityPlayerMotionServiceV1>(host, CABBIRD_UNITY_PLAYER_MOTION_SERVICE_V1_ID);
        if (c->player && !c->player->snapshot) c->player = nullptr;
        if (c->teleport && (!c->teleport->teleport || !c->teleport->state)) c->teleport = nullptr;
        if (c->motion && (!c->motion->acquire || !c->motion->update || !c->motion->release)) c->motion = nullptr;
        if (c->hooks && (!c->hooks->create || !c->hooks->release || !c->hooks->begin_callback || !c->hooks->end_callback)) {
            c->hooks = nullptr;
        }

        if (!c->core || !c->core->log || !c->core->read_memory ||
            !c->ui || !c->ui->checkbox || !c->ui->slider_float || !c->ui->input_double ||
            !c->ui->button || !c->ui->text || !c->ui->separator || !c->ui->frame_state ||
            !c->windows || !c->windows->register_window || !c->windows->release_window ||
            !c->windows->state || !c->windows->begin || !c->windows->end ||
            !c->input || !c->input->snapshot || !c->input->register_hotkey || !c->input->release_hotkey ||
            !c->il2cpp || !c->il2cpp->with_runtime ||
            !c->metadata || !c->metadata->resolve_method || !c->metadata->resolve_instance_field ||
            !c->lifecycle || !c->lifecycle->register_stop_cleanup || !c->lifecycle->unregister_stop_cleanup) {
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        }

        const auto cfgStatus = c->config.Bind(host, kSettingsId, SettingsSchema());
        if (!Succeeded(cfgStatus)) return cfgStatus;
        try {
            if (const auto saved = c->config.Read()) {
                Settings loaded;
                loaded.distance    = saved->value("distance", 0.0);
                loaded.fov         = saved->value("fov", 0.0);
                loaded.freeCamera  = saved->value("freeCamera", 0);
                loaded.lodFollow   = saved->value("lodFollow", 1);
                loaded.speed       = saved->value("speed", kDefaultSpeed);
                loaded.toggleKey   = saved->value("toggleKey", kDefaultToggleKey);
                loaded.teleportKey = saved->value("teleportKey", kDefaultTeleportKey);
                if (SettingsValid(loaded)) {
                    c->settings = loaded;
                } else {
                    LogOnceText(*c, 1U << 3, c->text.Text("log.settings",
                                "Camera Tools: saved settings are invalid; defaults retained."));
                }
            }
        } catch (...) {
            LogOnceText(*c, 1U << 3, c->text.Text("log.settings",
                        "Camera Tools: saved settings are invalid; defaults retained."));
        }
        SyncAtomicSettings(*c);

        const auto cleanupStatus =
            c->lifecycle->register_stop_cleanup(c->lifecycle->user, Cleanup, c.get(), &c->cleanup);
        if (!Succeeded(cleanupStatus)) return cleanupStatus;

        *result = c.release();
        return Ok();
    } catch (...) {
        return Status(CABBIRD_STATUS_V1_FAILED);
    }
}

CabbirdStatusV1 CABBIRD_CALL Start(void* raw) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        const std::string title = c.text.Text("window.title", "Camera Tools");
        CabbirdWindowSpecV1 spec{sizeof(spec)};
        spec.id = StringView("camera-tools.main");
        spec.title = StringView(title);
        spec.initial_width = 440.f;
        spec.initial_height = 340.f;
        spec.minimum_width = 300.f;
        spec.minimum_height = 200.f;
        spec.default_open = 1;
        const auto status = c.windows->register_window(c.windows->user, &spec, &c.window);
        if (!Succeeded(status)) return status;

        {
            std::scoped_lock lock(c.settingsMutex);
            const bool toggleOk = Succeeded(BindSingleHotkey(
                c, c.toggleHotkey, "camera-tools.toggle", c.settings.toggleKey, OnToggleHotkey));
            const bool tpOk = Succeeded(BindSingleHotkey(
                c, c.teleportHotkey, "camera-tools.teleport", c.settings.teleportKey, OnTeleportHotkey));
            c.hotkeyError = !(toggleOk && tpOk);
        }

        g_activeContext.store(&c, std::memory_order_release);
        c.running.store(true, std::memory_order_release);
        return Ok();
    } catch (...) {
        return Status(CABBIRD_STATUS_V1_FAILED);
    }
}

CabbirdStatusV1 CABBIRD_CALL Stop(void* raw, std::uint32_t) noexcept {
    auto& c = *static_cast<Context*>(raw);
    try {
        c.running.store(false, std::memory_order_release);
        bool released = ReleaseMotionHold(c);
        ReleaseAllHooks(c);
        Context* expected = &c;
        g_activeContext.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);

        {
            std::scoped_lock lock(c.settingsMutex);
            c.captureTarget = CaptureTarget::None;
            const auto st1 = BindSingleHotkey(c, c.toggleHotkey, "camera-tools.toggle", 0, OnToggleHotkey);
            const auto st2 = BindSingleHotkey(c, c.teleportHotkey, "camera-tools.teleport", 0, OnTeleportHotkey);
            released = released && Succeeded(st1) && Succeeded(st2);
        }

        const bool saved = c.config.Stop();

        if (c.window.id != 0) {
            const auto st = c.windows->release_window(c.windows->user, c.window);
            released = released && (Succeeded(st) || st.code == CABBIRD_STATUS_V1_NOT_FOUND);
            c.window = {};
        }
        if (c.cleanup.id != 0) {
            const auto st = c.lifecycle->unregister_stop_cleanup(c.lifecycle->user, c.cleanup);
            released = released && (Succeeded(st) || st.code == CABBIRD_STATUS_V1_NOT_FOUND);
            c.cleanup = {};
        }

        return (saved && released) ? Ok() : Status(CABBIRD_STATUS_V1_FAILED);
    } catch (...) {
        return Status(CABBIRD_STATUS_V1_FAILED);
    }
}

void CABBIRD_CALL Unload(void* raw) noexcept {
    try {
        auto* c = static_cast<Context*>(raw);
        if (c) {
            Context* expected = c;
            g_activeContext.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
            delete c;
        }
    } catch (...) {}
}

}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(CabbirdPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor) ||
        descriptor->api_major != CABBIRD_PLUGIN_API_V1_MAJOR) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    descriptor->id        = StringView("cabbird.camera-tools");
    descriptor->name      = StringView("Camera Tools");
    descriptor->author    = StringView("Cabbird");
    descriptor->version   = StringView("1.0.0");
    descriptor->on_load   = Load;
    descriptor->on_start  = Start;
    descriptor->on_stop   = Stop;
    descriptor->on_unload = Unload;
    descriptor->on_update = Update;
    descriptor->on_draw   = Draw;
    return Ok();
}

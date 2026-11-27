#include "cabbird/plugin_support/spatial.hpp"
#include "cabbird/sdk/services/game_lifecycle.h"
#include "cabbird/sdk/services/interop.h"
#include "cabbird/sdk/services/plugin_state.h"
#include "cabbird/sdk/services/unity.h"
#include "cabbird/sdk/services/unity_metadata.h"

#include "joint_limits.hpp"
#include "mmd_camera.hpp"
#include "mmd_morph_map.hpp"
#include "mmd_retarget.hpp"
#include "orbit_camera.hpp"
#include "pose_document.hpp"
#include "pose_history.hpp"
#include "pose_math.hpp"
#include "pose_mirror.hpp"
#include "rotation_gizmo.hpp"
#include "skeleton_filter.hpp"
#include "unity_pose_runtime.hpp"

#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using namespace betterpose;
using cabbird::plugins::ConfigStore;
using cabbird::plugins::Json;
using cabbird::plugins::NumberSchema;
using cabbird::plugins::ObjectSchema;
using cabbird::plugins::Query;
using cabbird::plugins::Range;
using cabbird::plugins::Status;
using cabbird::plugins::Toggle;
using cabbird::plugins::ToggleSchema;
using cabbird::sdk::Ok;
using cabbird::sdk::StringView;
using cabbird::sdk::Succeeded;
using Object = CabbirdIl2CppObjectV1;
using Runtime = cabbird::sdk::il2cpp::Context;

constexpr const char* kSettingsId = "better-pose-settings";
constexpr float kWheelDeltaUnit = 120.f;
constexpr float kJointPickRadiusMinPx = 10.f;
constexpr float kJointStackTiePx = 3.f;
constexpr float kJointDragThresholdPx = 4.f;
constexpr float kGizmoDragThresholdPx = 1.5f;
constexpr size_t kTargetsPerPage = 6;
constexpr size_t kBonesPerPage = 12;

// Append-only UI table field check per docs/api-reference/ui-services.md.
#define HAS_UI_FIELD(uiPtr, fieldName) \
    ((uiPtr) != nullptr && \
     (uiPtr)->struct_size >= offsetof(CabbirdUiServiceV1, fieldName) + sizeof((uiPtr)->fieldName) && \
     (uiPtr)->fieldName != nullptr)

struct Settings {
    int showSkeleton{1};
    int bodyOnly{1};
    int showFaceBones{0};
    int showGizmo{1};
    int showTooltip{1};
    int enableLimbIk{1};
    int lockLegPlane{1};
    int enableJointLimits{1};
    float jointRadius{6.f};
    int pauseAnim{0};
    float animSpeed{1.f};
    float orbitFov{45.f};
    std::string poseFilePath{"pose.json"};
    std::string skeletonFilePath{"skeleton.json"};

    // Persisted MMD import, playback, and camera settings.
    std::string mmdMotionPath{};
    std::string mmdCameraPath{};
    int mmdReferenceMode{0};
    int mmdIkMode{0};
    int mmdLoop{1};
    float mmdSpeed{1.f};
    int mmdHipOnPelvis{1};
    int mmdFeetAnchor{0};
    int mmdTargetLegIk{1};
    int mmdApplyRoot{1};
    int mmdLockPlanar{0};
    int mmdPlayExpressions{1};
    float mmdAmpMaster{1.f};
    int mmdCamFollow{1};
    int mmdCamAutoCut{1};
    int mmdCamMirror{0};
    float mmdCamDistScale{1.f};
    float mmdCamFovOffset{0.f};
    float mmdCamYawOffset{0.f};
    float mmdCamOffsetY{0.f};

    bool operator==(const Settings&) const = default;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Settings,
    showSkeleton, bodyOnly, showFaceBones, showGizmo, showTooltip,
    enableLimbIk, lockLegPlane, enableJointLimits, jointRadius,
    pauseAnim, animSpeed, orbitFov, poseFilePath, skeletonFilePath,
    mmdMotionPath, mmdCameraPath, mmdReferenceMode, mmdIkMode, mmdLoop,
    mmdSpeed, mmdHipOnPelvis, mmdFeetAnchor, mmdTargetLegIk, mmdApplyRoot,
    mmdLockPlanar, mmdPlayExpressions, mmdAmpMaster, mmdCamFollow,
    mmdCamAutoCut, mmdCamMirror, mmdCamDistScale, mmdCamFovOffset,
    mmdCamYawOffset, mmdCamOffsetY)

Json SettingsSchema() {
    Json schema = ObjectSchema({
        {"showSkeleton", ToggleSchema()},
        {"bodyOnly", ToggleSchema()},
        {"showFaceBones", ToggleSchema()},
        {"showGizmo", ToggleSchema()},
        {"showTooltip", ToggleSchema()},
        {"enableLimbIk", ToggleSchema()},
        {"lockLegPlane", ToggleSchema()},
        {"enableJointLimits", ToggleSchema()},
        {"jointRadius", NumberSchema(3.0, 14.0)},
        {"pauseAnim", ToggleSchema()},
        {"animSpeed", NumberSchema(0.0, 3.0)},
        {"orbitFov", NumberSchema(15.0, 110.0)},
        {"poseFilePath", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
        {"skeletonFilePath", {{"type", "string"}, {"minLength", 1}, {"maxLength", 260}}},
        {"mmdMotionPath", {{"type", "string"}, {"maxLength", 520}}},
        {"mmdCameraPath", {{"type", "string"}, {"maxLength", 520}}},
        {"mmdReferenceMode", {{"type", "integer"}, {"minimum", 0}, {"maximum", 2}}},
        {"mmdIkMode", {{"type", "integer"}, {"minimum", 0}, {"maximum", 2}}},
        {"mmdLoop", ToggleSchema()},
        {"mmdSpeed", NumberSchema(0.05, 3.0)},
        {"mmdHipOnPelvis", ToggleSchema()},
        {"mmdFeetAnchor", ToggleSchema()},
        {"mmdTargetLegIk", ToggleSchema()},
        {"mmdApplyRoot", ToggleSchema()},
        {"mmdLockPlanar", ToggleSchema()},
        {"mmdPlayExpressions", ToggleSchema()},
        {"mmdAmpMaster", NumberSchema(0.0, 2.0)},
        {"mmdCamFollow", ToggleSchema()},
        {"mmdCamAutoCut", ToggleSchema()},
        {"mmdCamMirror", ToggleSchema()},
        {"mmdCamDistScale", NumberSchema(0.1, 5.0)},
        {"mmdCamFovOffset", NumberSchema(-60.0, 60.0)},
        {"mmdCamYawOffset", NumberSchema(-180.0, 180.0)},
        {"mmdCamOffsetY", NumberSchema(-3.0, 3.0)}
    });
    // Keep only legacy keys required so pre-MMD saved configs upgrade cleanly without reset.
    schema["required"] = Json::array({
        "showSkeleton", "bodyOnly", "showFaceBones", "showGizmo", "showTooltip",
        "enableLimbIk", "lockLegPlane", "enableJointLimits", "jointRadius",
        "pauseAnim", "animSpeed", "orbitFov", "poseFilePath", "skeletonFilePath"
    });
    return schema;
}

bool ValidSettings(const Settings& s) {
    return Toggle(s.showSkeleton) && Toggle(s.bodyOnly) && Toggle(s.showFaceBones) &&
           Toggle(s.showGizmo) && Toggle(s.showTooltip) && Toggle(s.enableLimbIk) &&
           Toggle(s.lockLegPlane) && Toggle(s.enableJointLimits) &&
           Range(s.jointRadius, 3.0, 14.0) && Toggle(s.pauseAnim) &&
           Range(s.animSpeed, 0.0, 3.0) && Range(s.orbitFov, 15.0, 110.0) &&
           !s.poseFilePath.empty() && s.poseFilePath.size() <= 260 &&
           !s.skeletonFilePath.empty() && s.skeletonFilePath.size() <= 260 &&
           s.mmdMotionPath.size() <= 520 && s.mmdCameraPath.size() <= 520 &&
           s.mmdReferenceMode >= 0 && s.mmdReferenceMode <= 2 &&
           s.mmdIkMode >= 0 && s.mmdIkMode <= 2 &&
           Toggle(s.mmdLoop) && Range(s.mmdSpeed, 0.05, 3.0) &&
           Toggle(s.mmdHipOnPelvis) && Toggle(s.mmdFeetAnchor) && Toggle(s.mmdTargetLegIk) &&
           Toggle(s.mmdApplyRoot) && Toggle(s.mmdLockPlanar) && Toggle(s.mmdPlayExpressions) &&
           Range(s.mmdAmpMaster, 0.0, 2.0) &&
           Toggle(s.mmdCamFollow) && Toggle(s.mmdCamAutoCut) && Toggle(s.mmdCamMirror) &&
           Range(s.mmdCamDistScale, 0.1, 5.0) && Range(s.mmdCamFovOffset, -60.0, 60.0) &&
           Range(s.mmdCamYawOffset, -180.0, 180.0) && Range(s.mmdCamOffsetY, -3.0, 3.0);
}

uint64_t NowMs() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool KeyDown(const CabbirdInputSnapshotV1& input, unsigned vk) noexcept {
    return vk < 256 && (input.keys[vk / 8U] & (1U << (vk % 8U))) != 0;
}
bool MouseDown(const CabbirdInputSnapshotV1& input, unsigned buttonIndex) noexcept {
    return (input.mouse_buttons & (1U << buttonIndex)) != 0;
}

// Commands queued from UI / Render thread and consumed on the Game thread.
struct PendingCommands {
    bool refreshTargets{};
    bool scanSceneRigs{};
    bool selectLocalPlayer{};
    bool hasSelectTarget{};
    uint64_t selectTargetId{};
    bool reloadBones{};
    bool resetAll{};
    bool refreshAnim{};
    bool toggleBindBase{};
    bool wantBindBase{};

    bool hasBoneEdit{};
    std::vector<std::pair<int, EulerAngles>> boneEdits;

    bool hasBulkOffsets{};
    std::vector<EulerAngles> bulkOffsets;
    bool hasRootOffsetEdit{};
    Vec3 rootOffsetEdit{};

    bool hasMirror{};
    MirrorMode mirrorMode{MirrorMode::Flip};

    bool hasOrbitToggle{};
    bool wantOrbitEnabled{};
    bool orbitResetView{};
    bool orbitFocusSelected{};
    int orbitFocusBone{-1};
    bool hasOrbitDelta{};
    float orbitDx{};
    float orbitDy{};
    float panDx{};
    float panDy{};
    float zoomNotches{};
    bool hasOrbitFov{};
    float orbitFov{45.f};

    bool hasPauseToggle{};
    bool wantPause{};
    bool hasSpeedEdit{};
    float wantSpeed{1.f};

    // MMD motion & camera playback commands.
    bool mmdTogglePlay{};
    bool mmdSetPlaying{};
    bool mmdWantPlaying{};
    bool mmdStop{};
    bool mmdUnload{};
    bool mmdHasSeek{};
    double mmdSeekFrame{};
    bool mmdSyncSettings{};
    bool mmdCamHasEnable{};
    bool mmdCamWantEnable{};
    bool mmdCamClear{};
    bool mmdCamReanchor{};
};

// Active Game-thread skeleton bone entry (owns GC handle).
struct GameBone {
    CabbirdIl2CppGCHandleV1 transformHandle{};
    Object* framePtr{};
    std::string name;
    int parent{-1};
    Vec3 capturedLocalPos{};
    Quat capturedLocalRot{Quat::Identity()};
    Quat bindLocalRot{Quat::Identity()};
    bool hasBindLocalRot{};
    Vec3 restWorldPos{};
    Quat restWorldRot{Quat::Identity()};
    EulerAngles offsetEuler{};
    bool isBodyBone{true};
    bool isFaceBone{false};
    JointLimit limit{};
    IkChain ikChain{};
};

struct SavedSmrState {
    CabbirdIl2CppGCHandleV1 smrHandle{};
    Object* framePtr{};
    bool origUpdateWhenOffscreen{};
    bool modified{};
};

struct GameBlendShape {
    int smrIndex{-1};
    int shapeIndex{-1};
    std::string name;
    float originalWeight{};
    bool modified{};
};

enum class DragMode : int {
    None = 0,
    GizmoRing,
    RootArrow,
    JointFk,
    JointIk,
    JointTwist,
    OrbitRotate,
    OrbitPan
};

struct DragState {
    DragMode mode{DragMode::None};
    bool dragMoved{};
    int boneIndex{-1};
    int parentBoneIndex{-1};
    int gizmoAxis{kGizmoAxisNone};
    Vec2 startMouse{};
    Vec2 lastMouse{};
    GizmoDragTracker gizmoDrag{};
    RootArrowPick rootArrowPick{};
    Vec3 dragPlanePoint{};
    Vec3 dragPlaneNormal{0.0, 0.0, 1.0};
    double depthOffsetMeters{};
    std::vector<EulerAngles> startOffsets;
    Vec3 startRootOffset{};
    std::vector<EulerAngles> currentOffsets;
    Vec3 currentRootOffset{};
    Quat startBoneWorldRot{Quat::Identity()};
    Quat startParentWorldRot{Quat::Identity()};
    Vec3 startBoneWorldPos{};
    Vec3 startParentWorldPos{};
    IkChain ikChain{};
    Vec3 startUpperPos{};
    Vec3 startMidPos{};
    Vec3 startEndPos{};
    Quat startUpperWorldRot{Quat::Identity()};
    Quat startLowerWorldRot{Quat::Identity()};
    Vec3 startUpperToMidDir{};
    Vec3 startMidToEndDir{};
    Vec3 startPoleHint{};
};

enum class WorkerJobKind : int {
    ExportPose = 0,
    ImportPose,
    ExportSkeleton,
    BrowseMmdMotion,
    LoadMmdMotion,
    BrowseMmdCamera,
    LoadMmdCamera
};

struct WorkerIoJob {
    WorkerJobKind kind{WorkerJobKind::ExportPose};
    std::string path;
    std::string jsonPayload;
    int mmdReferenceMode{0};
    mmd::RetargetOptions mmdOptions{};
};

struct PendingMmdMotionResult {
    std::shared_ptr<mmd::LoadedMotion> motion;
    std::vector<mmd::MorphBinding> morphBindings;
    uint64_t builtForTargetId{};
    size_t builtForBoneCount{};
};

struct PendingMmdCameraResult {
    std::vector<mmd::VmdCameraKey> keys;
    std::string displayName;
};

struct MmdPlaybackState {
    std::shared_ptr<const mmd::LoadedMotion> motion;
    std::vector<mmd::MorphBinding> morphBindings;
    std::vector<float> morphWeightsBuf;
    std::vector<uint8_t> morphTouchedBuf;
    bool playing{};
    double currentFrame{};
    double camOnlyFrame{};
    std::chrono::steady_clock::time_point lastTick{};
    bool hasLastTick{};
    bool loop{true};
    float speed{1.f};
    bool playExpressions{true};
    mmd::MotionAmplitude amplitude{};
    mmd::MotionTravel travel{};

    std::vector<mmd::VmdCameraKey> cameraKeys;
    std::string cameraDisplayName;
    mmd::MmdCameraSettings camSettings{};
    mmd::MmdCameraAnchor camAnchor{};
    Vec3 lastAuthoredRootChar{};
    Vec3 lastAppliedRootChar{};
};

struct Context {
    explicit Context(const CabbirdHostApiV1* host) : text(host) {}

    cabbird::sdk::Localizer text;
    ConfigStore config;
    std::string pluginDir;
    std::string writableStateDir;

    const CabbirdCoreServiceV1* core{};
    const CabbirdUiServiceV1* ui{};
    const CabbirdWindowServiceV1* windows{};
    const CabbirdInputServiceV1* input{};
    const CabbirdUnityPlayerServiceV1* player{};
    const CabbirdUnityEntitiesServiceV1* entities{};
    const CabbirdIl2CppServiceV1* il2cpp{};
    const CabbirdUnityMetadataServiceV1* metadata{};
    const CabbirdGameLifecycleServiceV1* lifecycle{};
    const CabbirdHookServiceV1* hook{};
    const CabbirdSchedulerServiceV1* scheduler{};

    CabbirdGenerationHandleV1 window{};
    CabbirdGenerationHandleV1 cleanup{};
    CabbirdGenerationHandleV1 afterLateHook{};
    std::uintptr_t afterLateOriginal{};

    std::atomic_bool running{};
    std::atomic_bool stopping{};
    std::atomic_bool inDetour{};
    std::atomic<DWORD> gameThreadId{};
    std::atomic<uint32_t> reportedLogMask{};

    std::mutex settingsMutex;
    Settings settings;

    std::mutex commandMutex;
    PendingCommands pending;

    // Lock-free Game -> Render snapshot channel (never blocks on_draw).
    std::atomic<std::shared_ptr<const RenderSnapshot>> published;
    uint64_t snapshotRevision{};

    std::mutex noticeMutex;
    std::string noticeText;

    std::mutex ioMutex;
    std::vector<WorkerIoJob> ioQueue;

    // Cross-thread MMD Worker <-> Game <-> Render channels.
    std::mutex mmdMutex;
    uint64_t targetSkelTargetId{};
    mmd::TargetSkeletonInput targetSkelForMmd;
    std::vector<mmd::MorphTargetSlot> morphTargetSlotsForMmd;
    std::optional<PendingMmdMotionResult> pendingMmdMotion;
    std::optional<PendingMmdCameraResult> pendingMmdCamera;
    std::optional<std::string> pickedMotionPath;
    std::optional<std::string> pickedCameraPath;
    std::atomic_bool mmdLoading{};
    std::atomic_bool fileDialogOpen{};

    // Game-domain state (touched ONLY on Game thread).
    UnityBindings unity;
    std::chrono::steady_clock::time_point nextBindAttempt{};
    std::chrono::steady_clock::time_point nextAutoScanAttempt{};
    bool settingsSyncedToGame{};
    bool postLateHookAttempted{};
    uint64_t postLateDetourHits{};

    std::vector<TargetEntry> targets;
    uint64_t activeTargetId{};
    TargetKind activeTargetKind{TargetKind::LocalPlayer};
    std::string activeTargetName;
    CabbirdIl2CppGCHandleV1 activeGoHandle{};
    CabbirdIl2CppGCHandleV1 activePlayableAnimHandle{};
    CabbirdIl2CppGCHandleV1 activeAnimatorHandle{};
    std::vector<SavedSmrState> activeSmrs;
    std::vector<GameBlendShape> blendShapes;
    std::vector<GameBone> bones;
    int rootBoneIndex{-1};
    Vec3 rootOffset{};
    Vec3 capturedRootLocalPos{};
    Quat rootParentWorldRot{Quat::Identity()};
    Quat goToRootParentRot{Quat::Identity()};
    Vec3 rootForward{0.0, 0.0, 1.0};
    Vec3 rootRight{1.0, 0.0, 0.0};
    bool useBindBase{};
    bool pauseAnim{};
    float animSpeed{1.f};
    bool animModified{};
    float savedPlayableSpeed{1.f};
    bool hasSavedPlayableSpeed{};
    float savedAnimatorSpeed{1.f};
    bool hasSavedAnimatorSpeed{};

    MmdPlaybackState mmd;

    Object* frameCameraPtr{};
    Object* frameCameraTfPtr{};
    Object* frameRootParentTfPtr{};
    bool frameTargetAlive{};

    bool orbitEnabled{};
    OrbitCameraState orbit{};
    OrbitCameraState savedOrbitInitial{};
    bool hasSavedOrbitInitial{};
    float lastViewportH{1080.f};
    CabbirdIl2CppGCHandleV1 disabledBrainHandle{};
    bool disabledBrainActive{};
    float savedCameraFov{45.f};
    bool hasSavedCameraFov{};
    CabbirdIl2CppGCHandleV1 modifiedCameraHandle{};

    // Render-thread UI & interaction state (touched ONLY in on_draw).
    RenderSnapshot renderSnap;
    std::vector<uint32_t> boneDescendantCount;
    int selectedBone{-1};
    int hoveredBone{-1};
    int hoveredGizmoAxis{kGizmoAxisNone};
    int hoveredRootAxis{kGizmoAxisNone};
    BoneCategoryFilter categoryFilter{BoneCategoryFilter::All};
    size_t targetPage{};
    size_t bonePage{};
    char boneSearchBuf[128]{};
    char posePathBuf[260]{};
    char skeletonPathBuf[260]{};
    char mmdMotionPathBuf[520]{};
    char mmdCameraPathBuf[520]{};
    bool pathBufsInitialized{};
    bool showResetConfirm{};
    DragState drag;
    PoseHistory history;
    uint64_t historyTargetId{};
    size_t historyBoneCount{};
    bool prevLeftDown{};
    bool prevRightDown{};
    bool prevMiddleDown{};
    bool prevCtrlZ{};
    bool prevCtrlY{};
    bool prevSpaceDown{};
    uint64_t lastInputSeq{};
    std::vector<CabbirdUiScreenPrimitiveV1> drawPrims;
    std::vector<GizmoScreenSegment> gizmoSegs;
};

std::atomic<Context*> g_activeDetourContext{nullptr};

void LogOnceText(Context& c, uint32_t bit, std::string_view message) noexcept {
    try {
        if ((c.reportedLogMask.fetch_or(bit) & bit) == 0 && c.core && c.core->log) {
            c.core->log(c.core->user, CABBIRD_CORE_LOG_LEVEL_V1_WARNING, StringView(message));
        }
    } catch (...) {}
}

void SetNotice(Context& c, std::string msg) noexcept {
    try {
        std::scoped_lock lock(c.noticeMutex);
        c.noticeText = std::move(msg);
    } catch (...) {}
}

void QueueSaveSettings(Context& c) {
    Settings copy;
    {
        std::scoped_lock lock(c.settingsMutex);
        copy = c.settings;
    }
    if (ValidSettings(copy) && !c.config.Queue(Json(copy))) {
        LogOnceText(c, 1U << 0U, c.text.Text("log.save-failed", "BetterPose: settings could not be queued for saving."));
    }
}

void UiTextWrapped(const CabbirdUiServiceV1* ui, std::string_view text) noexcept {
    if (HAS_UI_FIELD(ui, text_wrapped)) {
        ui->text_wrapped(ui->user, StringView(text));
    } else if (ui && ui->text) {
        ui->text(ui->user, StringView(text));
    }
}

bool UiButtonSelectable(const CabbirdUiServiceV1* ui, std::string_view label, float width,
                        bool selected) noexcept {
    if (HAS_UI_FIELD(ui, button_selected)) {
        return ui->button_selected(ui->user, StringView(label), width, 0.f, selected ? 1 : 0, 1) != 0;
    }
    return ui && ui->button && ui->button(ui->user, StringView(label), width, 0.f) != 0;
}

// ---------- Game-domain Helpers (inside with_runtime) ----------

void FreeHandle(const CabbirdIl2CppApiV1& api, CabbirdIl2CppGCHandleV1& h) noexcept {
    if (h && api.il2cpp_gchandle_free) {
        api.il2cpp_gchandle_free(h);
        h = nullptr;
    }
}

bool IsClassOrSubclass(const Runtime& rt, CabbirdIl2CppClassV1* cls, CabbirdIl2CppClassV1* base) noexcept {
    if (!cls || !base) return false;
    const auto& api = rt.Functions();
    if (api.il2cpp_class_is_assignable_from) {
        return api.il2cpp_class_is_assignable_from(base, cls);
    }
    for (; cls; cls = api.il2cpp_class_get_parent(cls)) {
        if (cls == base) return true;
    }
    return false;
}

void ReleaseCameraTakeoverIfIdle(Context& c, const Runtime& rt) {
    if (c.orbitEnabled || c.mmd.camSettings.enabled) return;
    const auto& api = rt.Functions();
    if (c.disabledBrainActive && c.disabledBrainHandle && c.unity.behaviourSetEnabled) {
        Object* brain = api.il2cpp_gchandle_get_target(c.disabledBrainHandle);
        if (brain && c.unity.IsUnityObjectAlive(rt, brain)) {
            bool enable = true;
            void* args[]{&enable};
            Object* ret = nullptr;
            rt.Invoke(c.unity.behaviourSetEnabled, brain, args, &ret);
        }
    }
    FreeHandle(api, c.disabledBrainHandle);
    c.disabledBrainActive = false;

    if (c.hasSavedCameraFov && c.modifiedCameraHandle && c.unity.camSetFov) {
        Object* cam = api.il2cpp_gchandle_get_target(c.modifiedCameraHandle);
        if (cam && c.unity.IsUnityObjectAlive(rt, cam)) {
            float fov = c.savedCameraFov;
            void* args[]{&fov};
            Object* ret = nullptr;
            rt.Invoke(c.unity.camSetFov, cam, args, &ret);
        }
    }
    FreeHandle(api, c.modifiedCameraHandle);
    c.hasSavedCameraFov = false;
}

void EnsureCameraTakeover(Context& c, const Runtime& rt, Object* cam) {
    if (!cam || !c.unity.IsUnityObjectAlive(rt, cam)) return;
    const auto& api = rt.Functions();
    // Always disable CinemachineBrain while Orbit Camera or MMD Camera owns MainCamera, because
    // Cinemachine can update in LateUpdate/onPreCull after RunAfterLateUpdate and overwrite our writes.
    if (!c.disabledBrainActive && c.unity.cinemachineBrainTypeHandle && c.unity.goGetComponent1 &&
        c.unity.behaviourGetEnabled && c.unity.behaviourSetEnabled) {
        Object* camGo = nullptr;
        if (rt.Invoke(c.unity.compGetGameObject, cam, nullptr, &camGo) && camGo) {
            Object* brainType = api.il2cpp_gchandle_get_target(c.unity.cinemachineBrainTypeHandle);
            Object* brain = nullptr;
            void* args[]{brainType};
            if (brainType && rt.Invoke(c.unity.goGetComponent1, camGo, args, &brain) && brain &&
                c.unity.IsUnityObjectAlive(rt, brain)) {
                bool wasEnabled = false;
                if (UnityBindings::InvokeValue(rt, c.unity.behaviourGetEnabled, brain, nullptr, wasEnabled) && wasEnabled) {
                    bool disable = false;
                    void* setArgs[]{&disable};
                    Object* ret = nullptr;
                    if (rt.Invoke(c.unity.behaviourSetEnabled, brain, setArgs, &ret)) {
                        c.disabledBrainHandle = api.il2cpp_gchandle_new(brain, false);
                        c.disabledBrainActive = true;
                    }
                }
            }
        }
    }
    if (!c.hasSavedCameraFov && c.unity.camGetFov) {
        float fov = 45.f;
        if (UnityBindings::InvokeValue(rt, c.unity.camGetFov, cam, nullptr, fov) && std::isfinite(fov)) {
            c.savedCameraFov = fov;
            c.hasSavedCameraFov = true;
            FreeHandle(api, c.modifiedCameraHandle);
            c.modifiedCameraHandle = api.il2cpp_gchandle_new(cam, false);
        }
    }
}

void RestoreOrbitCamera(Context& c, const Runtime& rt) {
    c.hasSavedOrbitInitial = false;
    c.orbit.initialized = false;
    c.orbitEnabled = false;
    ReleaseCameraTakeoverIfIdle(c, rt);
}

void RestoreBlendShapes(Context& c, const Runtime& rt) {
    if (!c.unity.smrSetBlendShapeWeight || c.blendShapes.empty()) return;
    const auto& api = rt.Functions();
    for (auto& bs : c.blendShapes) {
        if (!bs.modified) continue;
        if (bs.smrIndex < 0 || static_cast<size_t>(bs.smrIndex) >= c.activeSmrs.size()) continue;
        Object* smr = c.activeSmrs[static_cast<size_t>(bs.smrIndex)].smrHandle
                          ? api.il2cpp_gchandle_get_target(c.activeSmrs[static_cast<size_t>(bs.smrIndex)].smrHandle)
                          : nullptr;
        if (smr && c.unity.IsUnityObjectAlive(rt, smr)) {
            int32_t idx = bs.shapeIndex;
            float w = bs.originalWeight;
            void* args[]{&idx, &w};
            Object* ret = nullptr;
            rt.Invoke(c.unity.smrSetBlendShapeWeight, smr, args, &ret);
        }
        bs.modified = false;
    }
}

void RestoreActiveAnimationAndBones(Context& c, const Runtime& rt, bool restoreBones) {
    const auto& api = rt.Functions();

    if (restoreBones) {
        RestoreBlendShapes(c, rt);
        for (size_t i = 0; i < c.bones.size(); ++i) {
            auto& b = c.bones[i];
            if (!b.transformHandle) continue;
            Object* tf = api.il2cpp_gchandle_get_target(b.transformHandle);
            if (!tf || !c.unity.IsUnityObjectAlive(rt, tf)) continue;
            UnityQuat q = ToUnity(b.capturedLocalRot);
            void* qArgs[]{&q};
            Object* ret = nullptr;
            rt.Invoke(c.unity.tfSetLocalRotation, tf, qArgs, &ret);
            if (static_cast<int>(i) == c.rootBoneIndex) {
                UnityVec3 p = ToUnity(c.capturedRootLocalPos);
                void* pArgs[]{&p};
                rt.Invoke(c.unity.tfSetLocalPosition, tf, pArgs, &ret);
            }
        }
    }

    if (c.hasSavedPlayableSpeed && c.activePlayableAnimHandle && c.unity.playableAnimSetSpeed) {
        Object* pa = api.il2cpp_gchandle_get_target(c.activePlayableAnimHandle);
        if (pa && c.unity.IsUnityObjectAlive(rt, pa)) {
            float spd = c.savedPlayableSpeed;
            void* args[]{&spd};
            Object* ret = nullptr;
            rt.Invoke(c.unity.playableAnimSetSpeed, pa, args, &ret);
        }
    }
    c.hasSavedPlayableSpeed = false;

    if (c.hasSavedAnimatorSpeed && c.activeAnimatorHandle && c.unity.animSetSpeed) {
        Object* an = api.il2cpp_gchandle_get_target(c.activeAnimatorHandle);
        if (an && c.unity.IsUnityObjectAlive(rt, an)) {
            float spd = c.savedAnimatorSpeed;
            void* args[]{&spd};
            Object* ret = nullptr;
            rt.Invoke(c.unity.animSetSpeed, an, args, &ret);
        }
    }
    c.hasSavedAnimatorSpeed = false;
    c.animModified = false;

    if (c.unity.smrSetUpdateWhenOffscreen) {
        for (auto& s : c.activeSmrs) {
            if (!s.modified || !s.smrHandle) continue;
            Object* smr = api.il2cpp_gchandle_get_target(s.smrHandle);
            if (smr && c.unity.IsUnityObjectAlive(rt, smr)) {
                bool orig = s.origUpdateWhenOffscreen;
                void* args[]{&orig};
                Object* ret = nullptr;
                rt.Invoke(c.unity.smrSetUpdateWhenOffscreen, smr, args, &ret);
            }
            s.modified = false;
        }
    }
}

void ClearActiveSkeleton(Context& c, const Runtime& rt, bool restoreState) {
    const auto& api = rt.Functions();
    if (restoreState) {
        RestoreActiveAnimationAndBones(c, rt, true);
    }
    c.mmd.motion.reset();
    c.mmd.morphBindings.clear();
    c.mmd.playing = false;
    c.mmd.currentFrame = 0.0;
    c.mmd.camAnchor.valid = false;
    c.blendShapes.clear();
    for (auto& b : c.bones) FreeHandle(api, b.transformHandle);
    c.bones.clear();
    for (auto& s : c.activeSmrs) FreeHandle(api, s.smrHandle);
    c.activeSmrs.clear();
    FreeHandle(api, c.activeGoHandle);
    FreeHandle(api, c.activePlayableAnimHandle);
    FreeHandle(api, c.activeAnimatorHandle);
    c.rootBoneIndex = -1;
    c.rootOffset = {};
    c.goToRootParentRot = Quat::Identity();
    c.frameTargetAlive = false;
    c.frameRootParentTfPtr = nullptr;
    {
        std::scoped_lock lock(c.mmdMutex);
        c.targetSkelTargetId = 0;
        c.targetSkelForMmd = {};
        c.morphTargetSlotsForMmd.clear();
    }
}

void ClearTargetList(Context& c, const CabbirdIl2CppApiV1& api) noexcept {
    for (auto& t : c.targets) {
        FreeHandle(api, t.gameObjectHandle);
        FreeHandle(api, t.playableAnimHandle);
        FreeHandle(api, t.animatorHandle);
    }
    c.targets.clear();
}

std::vector<Object*> GetChildSkinnedMeshes(const Context& c, const Runtime& rt, Object* go) {
    std::vector<Object*> result;
    if (!go || !c.unity.skinnedMeshTypeHandle) return result;
    Object* smrType = rt.Functions().il2cpp_gchandle_get_target(c.unity.skinnedMeshTypeHandle);
    if (!smrType) return result;

    Object* arr = nullptr;
    if (c.unity.goGetComponentsInChildren2) {
        bool includeInactive = true;
        void* args[]{smrType, &includeInactive};
        if (!rt.Invoke(c.unity.goGetComponentsInChildren2, go, args, &arr)) arr = nullptr;
    } else if (c.unity.goGetComponentsInChildren1) {
        void* args[]{smrType};
        if (!rt.Invoke(c.unity.goGetComponentsInChildren1, go, args, &arr)) arr = nullptr;
    }
    if (!arr) return result;

    const uint32_t len = rt.Functions().il2cpp_array_length(arr);
    const uint32_t capped = std::min(len, 64u);
    result.reserve(capped);
    for (uint32_t i = 0; i < capped; ++i) {
        Object* elem = nullptr;
        if (rt.ArrayReferenceAt(arr, i, &elem) && elem && c.unity.IsUnityObjectAlive(rt, elem)) {
            result.push_back(elem);
        }
    }
    return result;
}

Object* FindChildAnimator(const Context& c, const Runtime& rt, Object* go) {
    if (!go || !c.unity.animatorTypeHandle) return nullptr;
    Object* animType = rt.Functions().il2cpp_gchandle_get_target(c.unity.animatorTypeHandle);
    if (!animType) return nullptr;
    Object* arr = nullptr;
    if (c.unity.goGetComponentsInChildren2) {
        bool includeInactive = true;
        void* args[]{animType, &includeInactive};
        if (!rt.Invoke(c.unity.goGetComponentsInChildren2, go, args, &arr)) arr = nullptr;
    } else if (c.unity.goGetComponentsInChildren1) {
        void* args[]{animType};
        if (!rt.Invoke(c.unity.goGetComponentsInChildren1, go, args, &arr)) arr = nullptr;
    }
    if (!arr) return nullptr;
    const uint32_t len = rt.Functions().il2cpp_array_length(arr);
    for (uint32_t i = 0; i < std::min(len, 16u); ++i) {
        Object* elem = nullptr;
        if (rt.ArrayReferenceAt(arr, i, &elem) && elem && c.unity.IsUnityObjectAlive(rt, elem)) {
            return elem;
        }
    }
    return nullptr;
}

int CountSkeletonBonesQuick(const Context& c, const Runtime& rt, const std::vector<Object*>& smrs) {
    std::unordered_set<std::uintptr_t> seen;
    for (Object* smr : smrs) {
        Object* bonesArr = nullptr;
        if (!rt.Invoke(c.unity.smrGetBones, smr, nullptr, &bonesArr) || !bonesArr) continue;
        const uint32_t len = std::min(rt.Functions().il2cpp_array_length(bonesArr), 1024u);
        for (uint32_t i = 0; i < len; ++i) {
            Object* tf = nullptr;
            if (rt.ArrayReferenceAt(bonesArr, i, &tf) && tf) {
                seen.insert(reinterpret_cast<std::uintptr_t>(tf));
            }
        }
    }
    return static_cast<int>(seen.size());
}

void RefreshTargetList(Context& c, const Runtime& rt, bool includeSceneScan) {
    const auto& api = rt.Functions();
    ClearTargetList(c, api);
    std::unordered_set<std::uintptr_t> seenGo;

    auto addCandidate = [&](uint64_t id, TargetKind kind, std::string name, double dist,
                            Object* go, Object* playableAnim, Object* animator) {
        if (!go || !c.unity.IsUnityObjectAlive(rt, go)) return;
        const auto goKey = reinterpret_cast<std::uintptr_t>(go);
        if (!seenGo.insert(goKey).second) return;

        const auto smrs = GetChildSkinnedMeshes(c, rt, go);
        if (smrs.empty()) return;
        const int boneCount = CountSkeletonBonesQuick(c, rt, smrs);
        if (boneCount <= 0) return;

        if (!animator && playableAnim && c.unity.playableAnimGetAnimator) {
            Object* paAnim = nullptr;
            if (rt.Invoke(c.unity.playableAnimGetAnimator, playableAnim, nullptr, &paAnim) && paAnim &&
                c.unity.IsUnityObjectAlive(rt, paAnim)) {
                animator = paAnim;
            }
        }
        if (!animator) animator = FindChildAnimator(c, rt, go);
        if (name.empty()) name = c.unity.GetObjectName(rt, go);
        if (name.empty()) name = "Character";

        TargetEntry entry;
        entry.id = id ? id : static_cast<uint64_t>(goKey);
        entry.kind = kind;
        entry.rawName = std::move(name);
        entry.distanceMeters = dist;
        entry.hasPlayableAnimator = (playableAnim != nullptr);
        entry.boneCount = boneCount;
        entry.gameObjectHandle = api.il2cpp_gchandle_new(go, false);
        if (playableAnim) entry.playableAnimHandle = api.il2cpp_gchandle_new(playableAnim, false);
        if (animator) entry.animatorHandle = api.il2cpp_gchandle_new(animator, false);
        c.targets.push_back(std::move(entry));
    };

    auto tryAddBigWorldData = [&](std::uintptr_t dataAddr, TargetKind kind, std::string label, double dist) {
        if (!dataAddr || !c.unity.baseDataClass || !c.unity.baseDataGetTransform) return;
        // Validate memory readability through cabbird.core->read_memory before touching managed header.
        if (c.core && c.core->read_memory) {
            std::uintptr_t klassProbe = 0;
            if (!Succeeded(c.core->read_memory(c.core->user, dataAddr,
                    {reinterpret_cast<uint8_t*>(&klassProbe), sizeof(klassProbe)})) || !klassProbe) {
                return;
            }
        }
        auto* dataObj = reinterpret_cast<Object*>(dataAddr);
        CabbirdIl2CppClassV1* cls = seh::GetObjectClass(api, dataObj);
        if (!IsClassOrSubclass(rt, cls, c.unity.baseDataClass)) return;

        Object* relTf = nullptr;
        if (!rt.Invoke(c.unity.baseDataGetTransform, dataObj, nullptr, &relTf) || !relTf) return;

        Object* go = nullptr;
        if (c.unity.relTfGetGameObject) {
            rt.Invoke(c.unity.relTfGetGameObject, relTf, nullptr, &go);
        }
        if (!go && c.unity.relTfGetRawTransform) {
            Object* rawTf = nullptr;
            if (rt.Invoke(c.unity.relTfGetRawTransform, relTf, nullptr, &rawTf) && rawTf) {
                rt.Invoke(c.unity.compGetGameObject, rawTf, nullptr, &go);
            }
        }
        if (!go) return;

        Object* pa = nullptr;
        if (c.unity.baseDataGetAnimator) {
            rt.Invoke(c.unity.baseDataGetAnimator, dataObj, nullptr, &pa);
            if (pa && !c.unity.IsUnityObjectAlive(rt, pa)) pa = nullptr;
        }
        if (label.empty() && c.unity.baseDataGetConfigName) {
            Object* cfgStr = nullptr;
            if (rt.Invoke(c.unity.baseDataGetConfigName, dataObj, nullptr, &cfgStr) && cfgStr) {
                label = rt.ReadString(cfgStr).value_or(std::string{});
            }
        }
        addCandidate(static_cast<uint64_t>(dataAddr), kind, std::move(label), dist, go, pa, nullptr);
    };

    // 1. Local player first (via cabbird.unity.player).
    CabbirdUnityPlayerSnapshotV1 playerSnap{sizeof(playerSnap)};
    uint64_t localPlayerId = 0;
    if (c.player && c.player->snapshot && Succeeded(c.player->snapshot(c.player->user, &playerSnap)) &&
        (playerSnap.flags & CABBIRD_UNITY_PLAYER_V1_VALID) && playerSnap.entity_id) {
        localPlayerId = playerSnap.entity_id;
        tryAddBigWorldData(static_cast<std::uintptr_t>(playerSnap.entity_id),
                           TargetKind::LocalPlayer, {}, 0.0);
    }

    // 2. Live BigWorld entities (renew label lease so friendly names populate).
    if (c.entities && c.entities->entity_count && c.entities->entity_at) {
        if (c.entities->set_want_labels) {
            static_cast<void>(c.entities->set_want_labels(c.entities->user, 1));
        }
        const uint32_t total = std::min(c.entities->entity_count(c.entities->user), 256u);
        for (uint32_t i = 0; i < total && c.targets.size() < 64; ++i) {
            CabbirdUnityEntityV1 ent{sizeof(ent)};
            if (!Succeeded(c.entities->entity_at(c.entities->user, i, &ent))) continue;
            if (!(ent.flags & CABBIRD_UNITY_ENTITY_V1_VALID) || (ent.flags & CABBIRD_UNITY_ENTITY_V1_STALE)) continue;
            const uint64_t addr = ent.entity_data ? ent.entity_data : ent.entity_id;
            if (!addr) continue;

            TargetKind kind = TargetKind::WorldEntity;
            if ((ent.flags & CABBIRD_UNITY_ENTITY_V1_LOCAL_PLAYER) || addr == localPlayerId) {
                kind = TargetKind::LocalPlayer;
            } else if (ent.kind == CABBIRD_UNITY_ENTITY_V1_KIND_PLAYER) {
                kind = TargetKind::Player;
            } else if (ent.kind == CABBIRD_UNITY_ENTITY_V1_KIND_NPC) {
                kind = TargetKind::Npc;
            } else if (ent.kind == CABBIRD_UNITY_ENTITY_V1_KIND_MONSTER) {
                kind = TargetKind::Monster;
            }
            std::string label;
            if (ent.label && ent.label_size) label.assign(ent.label, ent.label_size);
            tryAddBigWorldData(static_cast<std::uintptr_t>(addr), kind, std::move(label), ent.distance_meters);
        }
    }

    // 3. Scene rig discovery (Animator first, then SkinnedMeshRenderer fallback for non-Animator showcases).
    auto findObjects = [&](CabbirdIl2CppGCHandleV1 typeHandle) -> Object* {
        if (!typeHandle) return nullptr;
        Object* typeObj = api.il2cpp_gchandle_get_target(typeHandle);
        if (!typeObj) return nullptr;
        Object* arr = nullptr;
        if (c.unity.objFindObjectsByType2) {
            bool includeInactive = false;
            void* args[]{typeObj, &includeInactive};
            if (rt.Invoke(c.unity.objFindObjectsByType2, nullptr, args, &arr) && arr) return arr;
        }
        if (c.unity.objFindObjectsOfType1) {
            void* args[]{typeObj};
            if (rt.Invoke(c.unity.objFindObjectsOfType1, nullptr, args, &arr) && arr) return arr;
        }
        return nullptr;
    };

    if (includeSceneScan || c.targets.empty()) {
        if (Object* animArr = findObjects(c.unity.animatorTypeHandle)) {
            const uint32_t len = std::min(api.il2cpp_array_length(animArr), 128u);
            for (uint32_t i = 0; i < len && c.targets.size() < 64; ++i) {
                Object* anim = nullptr;
                if (!rt.ArrayReferenceAt(animArr, i, &anim) || !anim || !c.unity.IsUnityObjectAlive(rt, anim)) continue;
                Object* go = nullptr;
                if (!rt.Invoke(c.unity.compGetGameObject, anim, nullptr, &go) || !go) continue;
                bool active = true;
                if (c.unity.goGetActiveInHierarchy) {
                    UnityBindings::InvokeValue(rt, c.unity.goGetActiveInHierarchy, go, nullptr, active);
                }
                if (!active) continue;
                addCandidate(0, TargetKind::SceneRig, c.unity.GetObjectName(rt, go), 0.0, go, nullptr, anim);
            }
        }
        if (c.targets.empty()) {
            if (Object* smrArr = findObjects(c.unity.skinnedMeshTypeHandle)) {
                const uint32_t len = std::min(api.il2cpp_array_length(smrArr), 128u);
                for (uint32_t i = 0; i < len && c.targets.size() < 64; ++i) {
                    Object* smr = nullptr;
                    if (!rt.ArrayReferenceAt(smrArr, i, &smr) || !smr || !c.unity.IsUnityObjectAlive(rt, smr)) continue;
                    Object* tf = nullptr;
                    if (!rt.Invoke(c.unity.compGetTransform, smr, nullptr, &tf) || !tf) continue;
                    // Climb up to the parent container owning the character's SMRs (up to 2 levels).
                    Object* parentTf = nullptr;
                    if (rt.Invoke(c.unity.tfGetParent, tf, nullptr, &parentTf) && parentTf &&
                        c.unity.IsUnityObjectAlive(rt, parentTf)) {
                        tf = parentTf;
                    }
                    Object* go = nullptr;
                    if (!rt.Invoke(c.unity.compGetGameObject, tf, nullptr, &go) || !go) continue;
                    bool active = true;
                    if (c.unity.goGetActiveInHierarchy) {
                        UnityBindings::InvokeValue(rt, c.unity.goGetActiveInHierarchy, go, nullptr, active);
                    }
                    if (!active) continue;
                    addCandidate(0, TargetKind::SceneRig, c.unity.GetObjectName(rt, go), 0.0, go, nullptr, nullptr);
                }
            }
        }
    }

    std::stable_sort(c.targets.begin(), c.targets.end(), [](const TargetEntry& a, const TargetEntry& b) {
        if ((a.kind == TargetKind::LocalPlayer) != (b.kind == TargetKind::LocalPlayer)) {
            return a.kind == TargetKind::LocalPlayer;
        }
        return a.distanceMeters < b.distanceMeters;
    });
}

bool LoadSkeletonFromTarget(Context& c, const Runtime& rt, const TargetEntry& target) {
    const auto& api = rt.Functions();
    Object* go = target.gameObjectHandle ? api.il2cpp_gchandle_get_target(target.gameObjectHandle) : nullptr;
    if (!go || !c.unity.IsUnityObjectAlive(rt, go)) return false;

    const auto smrs = GetChildSkinnedMeshes(c, rt, go);
    if (smrs.empty()) return false;

    Object* goTransform = nullptr;
    if (c.unity.goGetTransform) {
        rt.Invoke(c.unity.goGetTransform, go, nullptr, &goTransform);
    }
    const auto goTfKey = reinterpret_cast<std::uintptr_t>(goTransform);

    auto readLocalToWorld = [&](Object* tf) -> Mat4 {
        if (!tf || !c.unity.IsUnityObjectAlive(rt, tf)) return Mat4::Identity();
        if (c.unity.tfGetLocalToWorld) {
            UnityMat4 raw{};
            if (UnityBindings::InvokeValue(rt, c.unity.tfGetLocalToWorld, tf, nullptr, raw)) {
                const Mat4 m = Mat4::FromUnityColumnMajor(raw.m);
                if (m.IsFinite()) return m;
            }
        }
        UnityVec3 p{};
        UnityQuat r{};
        UnityBindings::InvokeValue(rt, c.unity.tfGetPosition, tf, nullptr, p);
        UnityBindings::InvokeValue(rt, c.unity.tfGetRotation, tf, nullptr, r);
        return Mat4::FromTrs(FromUnity(p), FromUnity(r));
    };

    Mat4 goWorldToLocal = Mat4::Identity();
    if (goTransform && c.unity.IsUnityObjectAlive(rt, goTransform)) {
        bool okW2L = false;
        if (c.unity.tfGetWorldToLocal) {
            UnityMat4 raw{};
            if (UnityBindings::InvokeValue(rt, c.unity.tfGetWorldToLocal, goTransform, nullptr, raw)) {
                const Mat4 m = Mat4::FromUnityColumnMajor(raw.m);
                if (m.IsFinite()) {
                    goWorldToLocal = m;
                    okW2L = true;
                }
            }
        }
        if (!okW2L) {
            Mat4 inv;
            if (readLocalToWorld(goTransform).Inverse(inv)) goWorldToLocal = inv;
        }
    }

    std::vector<Object*> rawBones;
    std::unordered_map<std::uintptr_t, size_t> ptrToRaw;
    struct RawSmrBindData {
        Object* smrTf{};
        Mat4 smrInGo{};
        std::vector<std::pair<std::uintptr_t, Mat4>> entries;
    };
    std::vector<RawSmrBindData> rawSmrBinds;
    std::unordered_set<std::uintptr_t> smrAncestors;
    if (goTfKey) smrAncestors.insert(goTfKey);

    auto registerBone = [&](Object* tf) -> size_t {
        const auto key = reinterpret_cast<std::uintptr_t>(tf);
        if (const auto it = ptrToRaw.find(key); it != ptrToRaw.end()) return it->second;
        const size_t idx = rawBones.size();
        rawBones.push_back(tf);
        ptrToRaw.emplace(key, idx);
        return idx;
    };

    Object* preferredRootBone = nullptr;
    size_t maxSmrBones = 0;

    for (Object* smr : smrs) {
        Object* smrTf = nullptr;
        if (rt.Invoke(c.unity.compGetTransform, smr, nullptr, &smrTf) && smrTf &&
            c.unity.IsUnityObjectAlive(rt, smrTf)) {
            Object* cur = smrTf;
            for (int depth = 0; cur && depth < 16; ++depth) {
                const auto k = reinterpret_cast<std::uintptr_t>(cur);
                smrAncestors.insert(k);
                if (k == goTfKey) break;
                Object* par = nullptr;
                if (!rt.Invoke(c.unity.tfGetParent, cur, nullptr, &par) || !par ||
                    !c.unity.IsUnityObjectAlive(rt, par)) {
                    break;
                }
                cur = par;
            }
        }

        Object* bonesArr = nullptr;
        if (!rt.Invoke(c.unity.smrGetBones, smr, nullptr, &bonesArr) || !bonesArr) continue;
        const uint32_t boneLen = std::min(api.il2cpp_array_length(bonesArr), 1024u);
        if (boneLen == 0) continue;

        if (boneLen > maxSmrBones && c.unity.smrGetRootBone) {
            Object* rb = nullptr;
            if (rt.Invoke(c.unity.smrGetRootBone, smr, nullptr, &rb) && rb && c.unity.IsUnityObjectAlive(rt, rb)) {
                preferredRootBone = rb;
                maxSmrBones = boneLen;
            }
        }

        Object* bindArr = nullptr;
        uint32_t bindLen = 0;
        const size_t elemBase = rt.ArrayElementBase();
        if (c.unity.smrGetSharedMesh && c.unity.meshGetBindposes && elemBase > 0) {
            Object* mesh = nullptr;
            if (rt.Invoke(c.unity.smrGetSharedMesh, smr, nullptr, &mesh) && mesh && c.unity.IsUnityObjectAlive(rt, mesh)) {
                if (rt.Invoke(c.unity.meshGetBindposes, mesh, nullptr, &bindArr) && bindArr) {
                    bindLen = api.il2cpp_array_length(bindArr);
                }
            }
        }

        RawSmrBindData smrData;
        smrData.smrTf = smrTf;
        smrData.smrInGo = Mat4::Multiply(goWorldToLocal, readLocalToWorld(smrTf));

        for (uint32_t i = 0; i < boneLen; ++i) {
            Object* tf = nullptr;
            if (!rt.ArrayReferenceAt(bonesArr, i, &tf) || !tf || !c.unity.IsUnityObjectAlive(rt, tf)) continue;
            registerBone(tf);
            if (bindArr && i < bindLen) {
                UnityMat4 rawM{};
                const auto addr = reinterpret_cast<std::uintptr_t>(bindArr) + elemBase +
                                  static_cast<size_t>(i) * sizeof(UnityMat4);
                if (seh::ReadMat4(addr, rawM)) {
                    const Mat4 m = Mat4::FromUnityColumnMajor(rawM.m);
                    if (m.IsFinite()) {
                        smrData.entries.emplace_back(reinterpret_cast<std::uintptr_t>(tf), m);
                    }
                }
            }
        }
        if (!smrData.entries.empty()) rawSmrBinds.push_back(std::move(smrData));
    }

    if (preferredRootBone) registerBone(preferredRootBone);
    if (rawBones.empty()) return false;

    const size_t initialCount = rawBones.size();
    for (size_t i = 0; i < initialCount; ++i) {
        std::vector<Object*> chain;
        Object* cur = rawBones[i];
        int depth = 0;
        bool reachedExisting = false;
        while (cur && depth++ < 32) {
            Object* parent = nullptr;
            if (!rt.Invoke(c.unity.tfGetParent, cur, nullptr, &parent) || !parent || !c.unity.IsUnityObjectAlive(rt, parent)) {
                break;
            }
            const auto pKey = reinterpret_cast<std::uintptr_t>(parent);
            if (pKey == goTfKey) break;
            if (ptrToRaw.find(pKey) != ptrToRaw.end()) {
                reachedExisting = true;
                break;
            }
            chain.push_back(parent);
            cur = parent;
        }
        if (reachedExisting) {
            for (Object* bridge : chain) registerBone(bridge);
        }
    }

    // Also include unskinned skeleton-branch root ancestors (e.g. Bip001/Root above Pelvis) that are
    // strictly below the SMR/Model container so animated root containers are straightened in Bind Pose.
    if (preferredRootBone) {
        Object* cur = preferredRootBone;
        for (int hops = 0; cur && hops < 4; ++hops) {
            Object* parent = nullptr;
            if (!rt.Invoke(c.unity.tfGetParent, cur, nullptr, &parent) || !parent ||
                !c.unity.IsUnityObjectAlive(rt, parent)) {
                break;
            }
            const auto pKey = reinterpret_cast<std::uintptr_t>(parent);
            if (smrAncestors.count(pKey) != 0) break;
            registerBone(parent);
            cur = parent;
        }
    }

    const size_t count = rawBones.size();
    std::vector<int> rawParent(count, -1);
    for (size_t i = 0; i < count; ++i) {
        Object* parent = nullptr;
        if (rt.Invoke(c.unity.tfGetParent, rawBones[i], nullptr, &parent) && parent) {
            const auto it = ptrToRaw.find(reinterpret_cast<std::uintptr_t>(parent));
            if (it != ptrToRaw.end() && it->second != i) {
                rawParent[i] = static_cast<int>(it->second);
            }
        }
    }

    std::vector<size_t> order;
    order.reserve(count);
    std::vector<uint8_t> state(count, 0);
    auto topoVisit = [&](auto&& self, size_t idx) -> void {
        if (state[idx] != 0) return;
        state[idx] = 1;
        const int p = rawParent[idx];
        if (p >= 0 && static_cast<size_t>(p) < count && state[static_cast<size_t>(p)] == 0) {
            self(self, static_cast<size_t>(p));
        }
        state[idx] = 2;
        order.push_back(idx);
    };
    for (size_t i = 0; i < count; ++i) topoVisit(topoVisit, i);

    std::vector<int> rawToSorted(count, -1);
    for (size_t s = 0; s < order.size(); ++s) rawToSorted[order[s]] = static_cast<int>(s);

    ClearActiveSkeleton(c, rt, true);

    c.activeTargetId = target.id;
    c.activeTargetKind = target.kind;
    c.activeTargetName = target.rawName;
    c.activeGoHandle = api.il2cpp_gchandle_new(go, false);
    if (target.playableAnimHandle) {
        if (Object* pa = api.il2cpp_gchandle_get_target(target.playableAnimHandle)) {
            if (c.unity.IsUnityObjectAlive(rt, pa)) {
                c.activePlayableAnimHandle = api.il2cpp_gchandle_new(pa, false);
            }
        }
    }
    if (target.animatorHandle) {
        if (Object* an = api.il2cpp_gchandle_get_target(target.animatorHandle)) {
            if (c.unity.IsUnityObjectAlive(rt, an)) {
                c.activeAnimatorHandle = api.il2cpp_gchandle_new(an, false);
            }
        }
    }

    for (Object* smr : smrs) {
        SavedSmrState st;
        st.smrHandle = api.il2cpp_gchandle_new(smr, false);
        st.framePtr = smr;
        if (c.unity.smrGetUpdateWhenOffscreen && c.unity.smrSetUpdateWhenOffscreen) {
            bool orig = false;
            if (UnityBindings::InvokeValue(rt, c.unity.smrGetUpdateWhenOffscreen, smr, nullptr, orig)) {
                st.origUpdateWhenOffscreen = orig;
                if (!orig) {
                    bool enable = true;
                    void* args[]{&enable};
                    Object* ret = nullptr;
                    if (rt.Invoke(c.unity.smrSetUpdateWhenOffscreen, smr, args, &ret)) {
                        st.modified = true;
                    }
                }
            }
        }
        const int smrIdx = static_cast<int>(c.activeSmrs.size());
        c.activeSmrs.push_back(st);

        // Discover facial/body BlendShapes on this SkinnedMeshRenderer for MMD morph playback.
        if (c.unity.smrGetSharedMesh && c.unity.meshGetBlendShapeCount && c.unity.meshGetBlendShapeName &&
            c.unity.smrGetBlendShapeWeight && c.unity.smrSetBlendShapeWeight) {
            Object* mesh = nullptr;
            if (rt.Invoke(c.unity.smrGetSharedMesh, smr, nullptr, &mesh) && mesh &&
                c.unity.IsUnityObjectAlive(rt, mesh)) {
                int32_t bsCount = 0;
                if (UnityBindings::InvokeValue(rt, c.unity.meshGetBlendShapeCount, mesh, nullptr, bsCount) &&
                    bsCount > 0) {
                    if (!c.unity.nativeBlendVerified && !c.unity.nativeBlendRejected) {
                        c.unity.VerifyBlendShapeNativeAbi(rt, smr);
                    }
                    const int32_t capped = std::min<int32_t>(bsCount, 512);
                    for (int32_t si = 0; si < capped; ++si) {
                        int32_t idxArg = si;
                        void* nameArgs[]{&idxArg};
                        Object* nameObj = nullptr;
                        if (!rt.Invoke(c.unity.meshGetBlendShapeName, mesh, nameArgs, &nameObj) || !nameObj) {
                            continue;
                        }
                        std::string bsName = rt.ReadString(nameObj).value_or(std::string{});
                        if (bsName.empty()) continue;
                        float initWeight = 0.f;
                        UnityBindings::InvokeValue(rt, c.unity.smrGetBlendShapeWeight, smr, nameArgs, initWeight);
                        if (!std::isfinite(initWeight)) initWeight = 0.f;
                        c.blendShapes.push_back({smrIdx, si, std::move(bsName), initWeight, false});
                    }
                }
            }
        }
    }

    c.bones.resize(count);
    std::vector<int> parents(count, -1);
    std::vector<std::string> names(count);
    std::vector<Vec3> capturedLocalPos(count);
    std::vector<Vec3> capturedLocalScale(count, Vec3{1.0, 1.0, 1.0});
    std::vector<Vec3> restWorldPos(count);
    std::vector<Quat> restWorldRot(count, Quat::Identity());
    std::vector<Mat4> rootParentInGo(count, Mat4::Identity());

    int preferredRootSorted = -1;
    int firstRootSorted = -1;

    for (size_t i = 0; i < count; ++i) {
        Object* tf = rawBones[order[i]];
        auto& b = c.bones[i];
        b.transformHandle = api.il2cpp_gchandle_new(tf, false);
        b.framePtr = tf;
        b.name = c.unity.GetObjectName(rt, tf);
        if (b.name.empty()) b.name = "Bone_" + std::to_string(i);
        const int rp = rawParent[order[i]];
        b.parent = (rp >= 0) ? rawToSorted[static_cast<size_t>(rp)] : -1;

        UnityVec3 lp{}, wp{}, ls{1.f, 1.f, 1.f};
        UnityQuat lr{}, wr{};
        if (UnityBindings::InvokeValue(rt, c.unity.tfGetLocalPosition, tf, nullptr, lp)) b.capturedLocalPos = FromUnity(lp);
        if (UnityBindings::InvokeValue(rt, c.unity.tfGetLocalRotation, tf, nullptr, lr)) b.capturedLocalRot = FromUnity(lr);
        if (UnityBindings::InvokeValue(rt, c.unity.tfGetPosition, tf, nullptr, wp)) b.restWorldPos = FromUnity(wp);
        if (UnityBindings::InvokeValue(rt, c.unity.tfGetRotation, tf, nullptr, wr)) b.restWorldRot = FromUnity(wr);
        if (c.unity.tfGetLocalScale && UnityBindings::InvokeValue(rt, c.unity.tfGetLocalScale, tf, nullptr, ls)) {
            const Vec3 s = FromUnity(ls);
            if (s.IsFinite() && s.x > 1e-5 && s.y > 1e-5 && s.z > 1e-5) capturedLocalScale[i] = s;
        }

        if (b.parent < 0) {
            Object* extParent = nullptr;
            if (rt.Invoke(c.unity.tfGetParent, tf, nullptr, &extParent) && extParent &&
                c.unity.IsUnityObjectAlive(rt, extParent)) {
                rootParentInGo[i] = Mat4::Multiply(goWorldToLocal, readLocalToWorld(extParent));
            }
        }

        b.isFaceBone = IsFaceBoneName(b.name);
        b.isBodyBone = IsBodyBoneName(b.name, false);

        parents[i] = b.parent;
        names[i] = b.name;
        capturedLocalPos[i] = b.capturedLocalPos;
        restWorldPos[i] = b.restWorldPos;
        restWorldRot[i] = b.restWorldRot;

        if (tf == preferredRootBone) preferredRootSorted = static_cast<int>(i);
        if (b.parent < 0 && firstRootSorted < 0) firstRootSorted = static_cast<int>(i);
    }

    // Prefer top-most skeleton root (so moving/resetting root owns the whole skeleton cleanly).
    int topRootSorted = preferredRootSorted;
    if (topRootSorted >= 0) {
        while (c.bones[static_cast<size_t>(topRootSorted)].parent >= 0) {
            topRootSorted = c.bones[static_cast<size_t>(topRootSorted)].parent;
        }
    }
    c.rootBoneIndex = (topRootSorted >= 0) ? topRootSorted : (firstRootSorted >= 0 ? firstRootSorted : 0);
    c.capturedRootLocalPos = c.bones[static_cast<size_t>(c.rootBoneIndex)].capturedLocalPos;
    c.rootOffset = {};

    Quat charWorldRot = Quat::Identity();
    if (goTransform && c.unity.IsUnityObjectAlive(rt, goTransform)) {
        UnityQuat r{};
        if (UnityBindings::InvokeValue(rt, c.unity.tfGetRotation, goTransform, nullptr, r)) {
            charWorldRot = FromUnity(r);
        }
    }
    c.rootForward = charWorldRot.Rotate({0.0, 0.0, 1.0}).Normalized({0.0, 0.0, 1.0});
    c.rootRight = charWorldRot.Rotate({1.0, 0.0, 0.0}).Normalized({1.0, 0.0, 0.0});

    c.goToRootParentRot = Quat::Identity();
    if (c.rootBoneIndex >= 0 && static_cast<size_t>(c.rootBoneIndex) < count) {
        Object* rootTf = c.bones[static_cast<size_t>(c.rootBoneIndex)].framePtr;
        if (rootTf) {
            Object* rootParentTf = nullptr;
            if (rt.Invoke(c.unity.tfGetParent, rootTf, nullptr, &rootParentTf) && rootParentTf &&
                c.unity.IsUnityObjectAlive(rt, rootParentTf)) {
                UnityQuat pr{};
                if (UnityBindings::InvokeValue(rt, c.unity.tfGetRotation, rootParentTf, nullptr, pr)) {
                    c.rootParentWorldRot = FromUnity(pr);
                    c.goToRootParentRot = (charWorldRot.Inverse() * c.rootParentWorldRot).Normalized();
                }
            }
        }
    }

    // Build per-SMR bindpose sets indexed by sorted bone index and compute unified bind transforms.
    std::vector<SmrBindPoseSet> smrSets;
    smrSets.reserve(rawSmrBinds.size());
    for (const auto& rs : rawSmrBinds) {
        SmrBindPoseSet set;
        set.smrInGo = rs.smrInGo;
        // Detect if this SMR transform is parented under an animated skeleton bone.
        Object* cur = rs.smrTf;
        for (int depth = 0; cur && depth < 16; ++depth) {
            const auto k = reinterpret_cast<std::uintptr_t>(cur);
            if (ptrToRaw.find(k) != ptrToRaw.end()) {
                set.smrUnderBone = true;
                break;
            }
            if (k == goTfKey) break;
            Object* par = nullptr;
            if (!rt.Invoke(c.unity.tfGetParent, cur, nullptr, &par) || !par ||
                !c.unity.IsUnityObjectAlive(rt, par)) {
                break;
            }
            cur = par;
        }
        set.entries.reserve(rs.entries.size());
        for (const auto& [tfKey, mat] : rs.entries) {
            const auto it = ptrToRaw.find(tfKey);
            if (it == ptrToRaw.end()) continue;
            const int sortedIdx = rawToSorted[it->second];
            if (sortedIdx >= 0 && static_cast<size_t>(sortedIdx) < count) {
                set.entries.emplace_back(sortedIdx, mat);
            }
        }
        if (!set.entries.empty()) smrSets.push_back(std::move(set));
    }

    std::vector<Quat> bindLocalRots;
    std::vector<bool> bindRotValid;
    std::vector<Vec3> bindLocalPos;
    std::vector<bool> bindPosValid;
    ComputeBindLocalTransforms(parents, capturedLocalPos, capturedLocalScale, rootParentInGo,
                               smrSets, bindLocalRots, bindRotValid, bindLocalPos, bindPosValid);
    for (size_t i = 0; i < count; ++i) {
        c.bones[i].bindLocalRot = bindLocalRots[i];
        c.bones[i].hasBindLocalRot = bindRotValid[i];
        c.bones[i].limit = BuildJointLimit(static_cast<int>(i), names, parents,
                                           restWorldPos, restWorldRot, c.rootForward, c.rootRight);
        c.bones[i].ikChain = ResolveIkChain(static_cast<int>(i), names, parents);
    }

    // Publish an immutable target skeleton + BlendShape snapshot for Worker-thread MMD retargeting.
    {
        mmd::TargetSkeletonInput skel;
        skel.targetName = c.activeTargetName;
        skel.rootBoneIndex = c.rootBoneIndex;
        skel.goToRootParentRot = c.goToRootParentRot;
        skel.bones.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto& b = c.bones[i];
            skel.bones.push_back({
                b.name, b.parent,
                bindPosValid[i] ? bindLocalPos[i] : b.capturedLocalPos,
                b.hasBindLocalRot ? b.bindLocalRot : b.capturedLocalRot,
                b.hasBindLocalRot,
                capturedLocalScale[i]
            });
        }
        std::vector<mmd::MorphTargetSlot> slots;
        slots.reserve(c.blendShapes.size());
        for (const auto& bs : c.blendShapes) {
            slots.push_back({bs.smrIndex, bs.shapeIndex, bs.name});
        }
        std::scoped_lock lock(c.mmdMutex);
        c.targetSkelTargetId = c.activeTargetId;
        c.targetSkelForMmd = std::move(skel);
        c.morphTargetSlotsForMmd = std::move(slots);
    }

    c.frameTargetAlive = true;
    return true;
}

bool HasActivePoseEdits(const Context& c) noexcept {
    if (c.mmd.motion != nullptr) return true;
    if (c.pauseAnim || c.useBindBase) return true;
    if (c.rootOffset.LengthSq() > 1e-10) return true;
    for (const auto& b : c.bones) {
        if (!b.offsetEuler.IsNearZero()) return true;
    }
    return false;
}

void SyncMmdSettingsToGame(Context& c) {
    Settings copy;
    {
        std::scoped_lock lock(c.settingsMutex);
        copy = c.settings;
    }
    c.mmd.loop = (copy.mmdLoop != 0);
    c.mmd.speed = std::clamp(copy.mmdSpeed, 0.05f, 3.f);
    c.mmd.playExpressions = (copy.mmdPlayExpressions != 0);
    c.mmd.amplitude.master = std::clamp(copy.mmdAmpMaster, 0.f, 2.f);
    c.mmd.travel.applyRootMotion = (copy.mmdApplyRoot != 0);
    c.mmd.travel.lockPlanar = (copy.mmdLockPlanar != 0);
    c.mmd.camSettings.followCharacter = (copy.mmdCamFollow != 0);
    c.mmd.camSettings.autoCut = (copy.mmdCamAutoCut != 0);
    c.mmd.camSettings.mirror = (copy.mmdCamMirror != 0);
    c.mmd.camSettings.distanceScale = std::clamp(copy.mmdCamDistScale, 0.1f, 5.f);
    c.mmd.camSettings.fovOffsetDeg = std::clamp(copy.mmdCamFovOffset, -60.f, 60.f);
    c.mmd.camSettings.yawOffsetDeg = std::clamp(copy.mmdCamYawOffset, -180.f, 180.f);
    c.mmd.camSettings.offsetY = std::clamp(copy.mmdCamOffsetY, -3.f, 3.f);
}

void CaptureMmdCameraAnchor(Context& c, const Runtime& rt) {
    const auto& api = rt.Functions();
    Object* anchorTf = nullptr;
    Object* go = c.activeGoHandle ? api.il2cpp_gchandle_get_target(c.activeGoHandle) : nullptr;
    if (go && c.unity.IsUnityObjectAlive(rt, go) && c.unity.goGetTransform) {
        if (!rt.Invoke(c.unity.goGetTransform, go, nullptr, &anchorTf) || !anchorTf ||
            !c.unity.IsUnityObjectAlive(rt, anchorTf)) {
            anchorTf = nullptr;
        }
    }
    if (!anchorTf && c.rootBoneIndex >= 0 && static_cast<size_t>(c.rootBoneIndex) < c.bones.size()) {
        const auto& rb = c.bones[static_cast<size_t>(c.rootBoneIndex)];
        if (rb.transformHandle) {
            Object* rtf = api.il2cpp_gchandle_get_target(rb.transformHandle);
            if (rtf && c.unity.IsUnityObjectAlive(rt, rtf)) {
                anchorTf = rtf;
            }
        }
    }
    if (!anchorTf) return;

    UnityVec3 goPos{};
    UnityQuat goRot{};
    if (!UnityBindings::InvokeValue(rt, c.unity.tfGetPosition, anchorTf, nullptr, goPos) ||
        !UnityBindings::InvokeValue(rt, c.unity.tfGetRotation, anchorTf, nullptr, goRot)) {
        return;
    }
    const Vec3 fwd = FromUnity(goRot).Rotate({0.0, 0.0, 1.0});
    c.mmd.camAnchor.feetWorldPos = FromUnity(goPos);
    c.mmd.camAnchor.charYawRad = std::atan2(fwd.x, fwd.z);
    c.mmd.camAnchor.unitMeters = c.mmd.motion ? c.mmd.motion->unitMetersPerMmdUnit : 0.08;
    c.mmd.camAnchor.valid = c.mmd.camAnchor.feetWorldPos.IsFinite() && std::isfinite(c.mmd.camAnchor.charYawRad);
}

void AdvanceMmdPlayback(Context& c) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (!c.mmd.hasLastTick) {
        c.mmd.lastTick = now;
        c.mmd.hasLastTick = true;
        return;
    }
    const double dt = std::clamp(std::chrono::duration<double>(now - c.mmd.lastTick).count(), 0.0, 0.1);
    c.mmd.lastTick = now;
    if (!c.mmd.playing) return;

    const double speed = static_cast<double>(std::clamp(c.mmd.speed, 0.05f, 3.f));
    if (c.mmd.motion) {
        const double maxF = (c.mmd.motion->frameCount > 1)
                                ? static_cast<double>(c.mmd.motion->frameCount - 1)
                                : 0.0;
        if (maxF > 0.0) {
            c.mmd.currentFrame += dt * c.mmd.motion->fps * speed;
            if (c.mmd.currentFrame >= maxF) {
                if (c.mmd.loop) {
                    c.mmd.currentFrame = std::fmod(c.mmd.currentFrame, maxF);
                } else {
                    c.mmd.currentFrame = maxF;
                    c.mmd.playing = false;
                }
            }
        } else {
            c.mmd.currentFrame = 0.0;
        }
    } else if (!c.mmd.cameraKeys.empty()) {
        const double maxF = static_cast<double>(c.mmd.cameraKeys.back().frame);
        if (maxF > 0.0) {
            c.mmd.camOnlyFrame += dt * 30.0 * speed;
            if (c.mmd.camOnlyFrame >= maxF) {
                if (c.mmd.loop) c.mmd.camOnlyFrame = std::fmod(c.mmd.camOnlyFrame, maxF);
                else {
                    c.mmd.camOnlyFrame = maxF;
                    c.mmd.playing = false;
                }
            }
        }
    }
}

void EvaluateMmdMorphsNoAlloc(Context& c, double vmdFrame) noexcept {
    const size_t nSlots = c.blendShapes.size();
    if (!c.mmd.motion || !c.mmd.playExpressions || c.mmd.morphBindings.empty() || nSlots == 0) return;
    if (c.mmd.morphWeightsBuf.size() != nSlots) c.mmd.morphWeightsBuf.assign(nSlots, 0.f);
    else std::fill(c.mmd.morphWeightsBuf.begin(), c.mmd.morphWeightsBuf.end(), 0.f);
    if (c.mmd.morphTouchedBuf.size() != nSlots) c.mmd.morphTouchedBuf.assign(nSlots, static_cast<uint8_t>(0));
    else std::fill(c.mmd.morphTouchedBuf.begin(), c.mmd.morphTouchedBuf.end(), static_cast<uint8_t>(0));

    const auto& tracks = c.mmd.motion->morphTracks;
    for (const auto& b : c.mmd.morphBindings) {
        if (b.trackIndex >= tracks.size()) continue;
        const float w01 = std::clamp(mmd::SampleMorphTrack(tracks[b.trackIndex], vmdFrame) * b.scale, 0.f, 1.f);
        for (size_t i = 0; i < nSlots; ++i) {
            if (c.blendShapes[i].smrIndex == b.smrIndex && c.blendShapes[i].shapeIndex == b.shapeIndex) {
                c.mmd.morphWeightsBuf[i] = std::min(100.f, c.mmd.morphWeightsBuf[i] + w01 * 100.f);
                c.mmd.morphTouchedBuf[i] = 1U;
            }
        }
    }
}

void FillMmdUiSnapshot(const Context& c, MmdUiSnapshot& out) {
    out.busyLoading = c.mmdLoading.load(std::memory_order_acquire);
    out.fileDialogBusy = c.fileDialogOpen.load(std::memory_order_acquire);
    out.totalBlendShapes = static_cast<uint32_t>(c.blendShapes.size());
    if (c.mmd.motion) {
        out.motionLoaded = true;
        out.playing = c.mmd.playing;
        out.currentFrame = c.mmd.currentFrame;
        out.frameCount = c.mmd.motion->frameCount;
        out.fps = c.mmd.motion->fps;
        out.mappedBoneCount = c.mmd.motion->mappedBoneCount;
        out.matchedMorphCount = static_cast<uint32_t>(c.mmd.morphBindings.size());
        out.recommendedReference = c.mmd.motion->report.recommendedReference;
        out.hasFootIk = c.mmd.motion->report.hasFootIk;
        out.hasLegD = c.mmd.motion->report.hasLegD;
        out.motionName = c.mmd.motion->displayName;
    } else if (!c.mmd.cameraKeys.empty()) {
        out.playing = c.mmd.playing;
        out.currentFrame = c.mmd.camOnlyFrame;
        out.frameCount = c.mmd.cameraKeys.back().frame + 1;
        out.fps = 30.0;
    }
    out.cameraLoaded = !c.mmd.cameraKeys.empty();
    out.cameraEnabled = c.mmd.camSettings.enabled && out.cameraLoaded;
    out.cameraKeyCount = static_cast<uint32_t>(c.mmd.cameraKeys.size());
    out.cameraName = c.mmd.cameraDisplayName;
}

void PublishStatusSnapshot(Context& c, std::string detail) {
    auto snap = std::make_shared<RenderSnapshot>();
    snap->revision = ++c.snapshotRevision;
    snap->bound = c.unity.bound;
    snap->statusDetail = std::move(detail);
    FillMmdUiSnapshot(c, snap->mmd);
    c.published.store(std::move(snap), std::memory_order_release);
}

void ApplyPoseAndPublishNative(Context& c) {
    if (!c.unity.nativeDirectVerified) return;

    AdvanceMmdPlayback(c);

    if (c.frameTargetAlive && !c.bones.empty() && HasActivePoseEdits(c)) {
        const auto setLocalRot = reinterpret_cast<NativeTransformQuatFn>(c.unity.nativeSetLocalRot.address);
        const auto getLocalRot = reinterpret_cast<NativeTransformQuatFn>(c.unity.nativeGetLocalRot.address);
        const auto setLocalPos = reinterpret_cast<NativeTransformVec3Fn>(c.unity.nativeSetLocalPos.address);
        const auto getLocalPos = reinterpret_cast<NativeTransformVec3Fn>(c.unity.nativeGetLocalPos.address);

        if (c.mmd.motion) {
            const auto& mot = *c.mmd.motion;
            const double frame = c.mmd.currentFrame;

            for (const auto& track : mot.boneTracks) {
                const int idx = track.targetBoneIndex;
                if (idx < 0 || static_cast<size_t>(idx) >= c.bones.size()) continue;
                auto& b = c.bones[static_cast<size_t>(idx)];
                if (!c.unity.IsUnityObjectAliveFast(b.framePtr)) continue;
                Quat q = mmd::SampleMappedBoneFrame(track, frame, c.mmd.amplitude);
                if (!b.offsetEuler.IsNearZero()) {
                    q = (q * Quat::FromUnityEuler(b.offsetEuler)).Normalized();
                }
                UnityQuat uq = ToUnity(q);
                seh::CallTransformQuat(setLocalRot, b.framePtr, &uq, c.unity.nativeSetLocalRot.method);
            }
            for (int idx : mot.rigidHoldBones) {
                if (idx < 0 || static_cast<size_t>(idx) >= c.bones.size()) continue;
                auto& b = c.bones[static_cast<size_t>(idx)];
                if (!b.hasBindLocalRot || !c.unity.IsUnityObjectAliveFast(b.framePtr)) continue;
                Quat q = b.bindLocalRot;
                if (!b.offsetEuler.IsNearZero()) {
                    q = (q * Quat::FromUnityEuler(b.offsetEuler)).Normalized();
                }
                UnityQuat uq = ToUnity(q);
                seh::CallTransformQuat(setLocalRot, b.framePtr, &uq, c.unity.nativeSetLocalRot.method);
            }
            for (const auto& offTrack : mot.boneOffsetTracks) {
                const int idx = offTrack.targetBoneIndex;
                if (idx < 0 || static_cast<size_t>(idx) >= c.bones.size()) continue;
                auto& b = c.bones[static_cast<size_t>(idx)];
                if (!c.unity.IsUnityObjectAliveFast(b.framePtr)) continue;
                UnityVec3 up = ToUnity(mmd::SampleVec3Track(offTrack.frames, frame));
                seh::CallTransformVec3(setLocalPos, b.framePtr, &up, c.unity.nativeSetLocalPos.method);
            }
            const int rIdx = (mot.rootBoneIndex >= 0 && static_cast<size_t>(mot.rootBoneIndex) < c.bones.size())
                                 ? mot.rootBoneIndex
                                 : c.rootBoneIndex;
            if (rIdx >= 0 && static_cast<size_t>(rIdx) < c.bones.size()) {
                auto& rb = c.bones[static_cast<size_t>(rIdx)];
                if (c.unity.IsUnityObjectAliveFast(rb.framePtr)) {
                    const Vec3 rawChar = mmd::SampleVec3Track(mot.rootTranslationsChar, frame);
                    c.mmd.lastAuthoredRootChar = rawChar;
                    const Vec3 appliedLocal = mmd::FilterRootTravel(rawChar, c.mmd.travel, mot.charToRootParentRot);
                    c.mmd.lastAppliedRootChar = mot.charToRootParentRot.Conjugate().Rotate(appliedLocal);
                    const Vec3 basePos = (rIdx == c.rootBoneIndex) ? c.capturedRootLocalPos : rb.capturedLocalPos;
                    UnityVec3 up = ToUnity(basePos + appliedLocal + c.rootOffset);
                    seh::CallTransformVec3(setLocalPos, rb.framePtr, &up, c.unity.nativeSetLocalPos.method);
                }
            }
            if (c.mmd.playExpressions && c.unity.nativeBlendVerified && !c.mmd.morphBindings.empty()) {
                const double vmdFrame = (mot.fps > 1e-3) ? (frame * 30.0 / mot.fps) : frame;
                EvaluateMmdMorphsNoAlloc(c, vmdFrame);
                const auto setBlend = reinterpret_cast<NativeSmrSetBlendFn>(c.unity.nativeSmrSetBlend.address);
                for (size_t i = 0; i < c.blendShapes.size(); ++i) {
                    if (!c.mmd.morphTouchedBuf[i]) continue;
                    auto& bs = c.blendShapes[i];
                    if (bs.smrIndex < 0 || static_cast<size_t>(bs.smrIndex) >= c.activeSmrs.size()) continue;
                    Object* smr = c.activeSmrs[static_cast<size_t>(bs.smrIndex)].framePtr;
                    if (!c.unity.IsUnityObjectAliveFast(smr)) continue;
                    if (seh::CallSmrSetBlend(setBlend, smr, bs.shapeIndex, c.mmd.morphWeightsBuf[i],
                                             c.unity.nativeSmrSetBlend.method)) {
                        bs.modified = true;
                    }
                }
            }
        } else {
            const bool fullOverride = c.pauseAnim || c.useBindBase;
            for (size_t i = 0; i < c.bones.size(); ++i) {
                auto& b = c.bones[i];
                if (!c.unity.IsUnityObjectAliveFast(b.framePtr)) continue;

                if (fullOverride) {
                    const Quat baseQ = (c.useBindBase && b.hasBindLocalRot) ? b.bindLocalRot : b.capturedLocalRot;
                    UnityQuat q = ToUnity((baseQ * Quat::FromUnityEuler(b.offsetEuler)).Normalized());
                    seh::CallTransformQuat(setLocalRot, b.framePtr, &q, c.unity.nativeSetLocalRot.method);
                } else if (!b.offsetEuler.IsNearZero()) {
                    UnityQuat cur{};
                    if (seh::CallTransformQuat(getLocalRot, b.framePtr, &cur, c.unity.nativeGetLocalRot.method)) {
                        UnityQuat q = ToUnity((FromUnity(cur) * Quat::FromUnityEuler(b.offsetEuler)).Normalized());
                        seh::CallTransformQuat(setLocalRot, b.framePtr, &q, c.unity.nativeSetLocalRot.method);
                    }
                }

                if (static_cast<int>(i) == c.rootBoneIndex && (fullOverride || c.rootOffset.LengthSq() > 1e-10)) {
                    if (fullOverride) {
                        UnityVec3 p = ToUnity(c.capturedRootLocalPos + c.rootOffset);
                        seh::CallTransformVec3(setLocalPos, b.framePtr, &p, c.unity.nativeSetLocalPos.method);
                    } else {
                        UnityVec3 cur{};
                        if (seh::CallTransformVec3(getLocalPos, b.framePtr, &cur, c.unity.nativeGetLocalPos.method)) {
                            UnityVec3 p = ToUnity(FromUnity(cur) + c.rootOffset);
                            seh::CallTransformVec3(setLocalPos, b.framePtr, &p, c.unity.nativeSetLocalPos.method);
                        }
                    }
                }
            }
        }
    }

    if (c.mmd.camSettings.enabled && !c.mmd.cameraKeys.empty() && c.mmd.camAnchor.valid &&
        c.unity.IsUnityObjectAliveFast(c.frameCameraPtr) &&
        c.unity.IsUnityObjectAliveFast(c.frameCameraTfPtr)) {
        const double camFrame = (c.mmd.motion && c.mmd.motion->fps > 1e-3)
                                    ? (c.mmd.currentFrame * 30.0 / c.mmd.motion->fps)
                                    : c.mmd.camOnlyFrame;
        const auto camPose = mmd::EvaluateMmdCameraUnity(
            c.mmd.cameraKeys, camFrame, c.mmd.camAnchor, c.mmd.camSettings,
            c.mmd.lastAuthoredRootChar, c.mmd.lastAppliedRootChar);
        if (camPose.valid) {
            UnityVec3 pos = ToUnity(camPose.worldPos);
            UnityQuat rot = ToUnity(camPose.worldRot);
            seh::CallTransformVec3(reinterpret_cast<NativeTransformVec3Fn>(c.unity.nativeSetPos.address),
                                   c.frameCameraTfPtr, &pos, c.unity.nativeSetPos.method);
            seh::CallTransformQuat(reinterpret_cast<NativeTransformQuatFn>(c.unity.nativeSetRot.address),
                                   c.frameCameraTfPtr, &rot, c.unity.nativeSetRot.method);
            seh::CallCameraSetFov(reinterpret_cast<NativeCameraSetFovFn>(c.unity.nativeCamSetFov.address),
                                  c.frameCameraPtr, camPose.verticalFovDeg, c.unity.nativeCamSetFov.method);
        }
    } else if (c.orbitEnabled && c.orbit.initialized &&
               c.unity.IsUnityObjectAliveFast(c.frameCameraPtr) &&
               c.unity.IsUnityObjectAliveFast(c.frameCameraTfPtr)) {
        UnityVec3 pos = ToUnity(c.orbit.Position());
        UnityQuat rot = ToUnity(c.orbit.Rotation());
        seh::CallTransformVec3(reinterpret_cast<NativeTransformVec3Fn>(c.unity.nativeSetPos.address),
                               c.frameCameraTfPtr, &pos, c.unity.nativeSetPos.method);
        seh::CallTransformQuat(reinterpret_cast<NativeTransformQuatFn>(c.unity.nativeSetRot.address),
                               c.frameCameraTfPtr, &rot, c.unity.nativeSetRot.method);
        seh::CallCameraSetFov(reinterpret_cast<NativeCameraSetFovFn>(c.unity.nativeCamSetFov.address),
                              c.frameCameraPtr, c.orbit.fovDeg, c.unity.nativeCamSetFov.method);
    }

    CameraProjection cam{};
    if (c.unity.ReadCameraProjectionNative(c.frameCameraPtr, c.frameCameraTfPtr, cam) && cam.viewportH > 1.f) {
        c.lastViewportH = cam.viewportH;
    }

    if (c.frameRootParentTfPtr && c.unity.IsUnityObjectAliveFast(c.frameRootParentTfPtr)) {
        UnityQuat pr{};
        if (seh::CallTransformQuat(reinterpret_cast<NativeTransformQuatFn>(c.unity.nativeGetRot.address),
                                   c.frameRootParentTfPtr, &pr, c.unity.nativeGetRot.method)) {
            c.rootParentWorldRot = FromUnity(pr);
        }
    } else {
        c.rootParentWorldRot = Quat::Identity();
    }

    auto snap = std::make_shared<RenderSnapshot>();
    snap->revision = ++c.snapshotRevision;
    snap->bound = c.unity.bound;
    snap->postLateHookActive = (c.afterLateHook.id != 0 && c.unity.nativeDirectVerified);
    snap->targetAlive = c.frameTargetAlive && !c.bones.empty();
    snap->useBindBase = c.useBindBase;
    snap->activeTargetId = c.activeTargetId;
    snap->activeTargetName = c.activeTargetName;
    snap->activeTargetKind = c.activeTargetKind;
    snap->rootBoneIndex = c.rootBoneIndex;
    snap->rootOffset = c.rootOffset;
    snap->rootParentWorldRot = c.rootParentWorldRot;
    snap->rootForward = c.rootForward;
    snap->rootRight = c.rootRight;
    snap->camera = cam;
    snap->orbit = c.orbit;
    snap->orbitActive = c.orbitEnabled;
    FillMmdUiSnapshot(c, snap->mmd);

    const auto getPos = reinterpret_cast<NativeTransformVec3Fn>(c.unity.nativeGetPos.address);
    const auto getRot = reinterpret_cast<NativeTransformQuatFn>(c.unity.nativeGetRot.address);
    snap->bones.reserve(c.bones.size());
    for (const auto& b : c.bones) {
        BoneRenderData rd;
        rd.name = b.name;
        rd.parent = b.parent;
        rd.restWorldPos = b.restWorldPos;
        rd.restWorldRot = b.restWorldRot;
        rd.restLocalRot = (c.useBindBase && b.hasBindLocalRot) ? b.bindLocalRot : b.capturedLocalRot;
        rd.offsetEuler = b.offsetEuler;
        rd.isBodyBone = b.isBodyBone;
        rd.isFaceBone = b.isFaceBone;
        rd.limit = b.limit;
        rd.ikChain = b.ikChain;
        if (c.unity.IsUnityObjectAliveFast(b.framePtr)) {
            UnityVec3 wp{};
            UnityQuat wr{};
            if (seh::CallTransformVec3(getPos, b.framePtr, &wp, c.unity.nativeGetPos.method)) rd.worldPos = FromUnity(wp);
            if (seh::CallTransformQuat(getRot, b.framePtr, &wr, c.unity.nativeGetRot.method)) rd.worldRot = FromUnity(wr);
        }
        snap->bones.push_back(std::move(rd));
    }

    snap->targets.reserve(c.targets.size());
    for (const auto& t : c.targets) {
        TargetEntry uiCopy;
        uiCopy.id = t.id;
        uiCopy.kind = t.kind;
        uiCopy.rawName = t.rawName;
        uiCopy.distanceMeters = t.distanceMeters;
        uiCopy.hasPlayableAnimator = t.hasPlayableAnimator;
        uiCopy.boneCount = t.boneCount;
        snap->targets.push_back(std::move(uiCopy));
    }

    c.published.store(std::move(snap), std::memory_order_release);
}

void ApplyPoseAndPublishReflection(Context& c, const Runtime& rt, Object* cam) {
    const auto& api = rt.Functions();
    AdvanceMmdPlayback(c);

    if (c.frameTargetAlive && !c.bones.empty() && HasActivePoseEdits(c)) {
        if (c.mmd.motion) {
            const auto& mot = *c.mmd.motion;
            const double frame = c.mmd.currentFrame;
            Object* ret = nullptr;

            for (const auto& track : mot.boneTracks) {
                const int idx = track.targetBoneIndex;
                if (idx < 0 || static_cast<size_t>(idx) >= c.bones.size()) continue;
                auto& b = c.bones[static_cast<size_t>(idx)];
                if (!b.transformHandle) continue;
                Object* tf = api.il2cpp_gchandle_get_target(b.transformHandle);
                if (!tf || !c.unity.IsUnityObjectAlive(rt, tf)) continue;
                Quat q = mmd::SampleMappedBoneFrame(track, frame, c.mmd.amplitude);
                if (!b.offsetEuler.IsNearZero()) {
                    q = (q * Quat::FromUnityEuler(b.offsetEuler)).Normalized();
                }
                UnityQuat uq = ToUnity(q);
                void* args[]{&uq};
                rt.Invoke(c.unity.tfSetLocalRotation, tf, args, &ret);
            }
            for (int idx : mot.rigidHoldBones) {
                if (idx < 0 || static_cast<size_t>(idx) >= c.bones.size()) continue;
                auto& b = c.bones[static_cast<size_t>(idx)];
                if (!b.hasBindLocalRot || !b.transformHandle) continue;
                Object* tf = api.il2cpp_gchandle_get_target(b.transformHandle);
                if (!tf || !c.unity.IsUnityObjectAlive(rt, tf)) continue;
                Quat q = b.bindLocalRot;
                if (!b.offsetEuler.IsNearZero()) {
                    q = (q * Quat::FromUnityEuler(b.offsetEuler)).Normalized();
                }
                UnityQuat uq = ToUnity(q);
                void* args[]{&uq};
                rt.Invoke(c.unity.tfSetLocalRotation, tf, args, &ret);
            }
            for (const auto& offTrack : mot.boneOffsetTracks) {
                const int idx = offTrack.targetBoneIndex;
                if (idx < 0 || static_cast<size_t>(idx) >= c.bones.size()) continue;
                auto& b = c.bones[static_cast<size_t>(idx)];
                if (!b.transformHandle) continue;
                Object* tf = api.il2cpp_gchandle_get_target(b.transformHandle);
                if (!tf || !c.unity.IsUnityObjectAlive(rt, tf)) continue;
                UnityVec3 up = ToUnity(mmd::SampleVec3Track(offTrack.frames, frame));
                void* args[]{&up};
                rt.Invoke(c.unity.tfSetLocalPosition, tf, args, &ret);
            }
            const int rIdx = (mot.rootBoneIndex >= 0 && static_cast<size_t>(mot.rootBoneIndex) < c.bones.size())
                                 ? mot.rootBoneIndex
                                 : c.rootBoneIndex;
            if (rIdx >= 0 && static_cast<size_t>(rIdx) < c.bones.size()) {
                auto& rb = c.bones[static_cast<size_t>(rIdx)];
                Object* tf = rb.transformHandle ? api.il2cpp_gchandle_get_target(rb.transformHandle) : nullptr;
                if (tf && c.unity.IsUnityObjectAlive(rt, tf)) {
                    const Vec3 rawChar = mmd::SampleVec3Track(mot.rootTranslationsChar, frame);
                    c.mmd.lastAuthoredRootChar = rawChar;
                    const Vec3 appliedLocal = mmd::FilterRootTravel(rawChar, c.mmd.travel, mot.charToRootParentRot);
                    c.mmd.lastAppliedRootChar = mot.charToRootParentRot.Conjugate().Rotate(appliedLocal);
                    const Vec3 basePos = (rIdx == c.rootBoneIndex) ? c.capturedRootLocalPos : rb.capturedLocalPos;
                    UnityVec3 up = ToUnity(basePos + appliedLocal + c.rootOffset);
                    void* args[]{&up};
                    rt.Invoke(c.unity.tfSetLocalPosition, tf, args, &ret);
                }
            }
            if (c.mmd.playExpressions && c.unity.smrSetBlendShapeWeight && !c.mmd.morphBindings.empty()) {
                const double vmdFrame = (mot.fps > 1e-3) ? (frame * 30.0 / mot.fps) : frame;
                EvaluateMmdMorphsNoAlloc(c, vmdFrame);
                for (size_t i = 0; i < c.blendShapes.size(); ++i) {
                    if (!c.mmd.morphTouchedBuf[i]) continue;
                    auto& bs = c.blendShapes[i];
                    if (bs.smrIndex < 0 || static_cast<size_t>(bs.smrIndex) >= c.activeSmrs.size()) continue;
                    Object* smr = c.activeSmrs[static_cast<size_t>(bs.smrIndex)].smrHandle
                                      ? api.il2cpp_gchandle_get_target(c.activeSmrs[static_cast<size_t>(bs.smrIndex)].smrHandle)
                                      : nullptr;
                    if (!smr || !c.unity.IsUnityObjectAlive(rt, smr)) continue;
                    int32_t sIdx = bs.shapeIndex;
                    float w = c.mmd.morphWeightsBuf[i];
                    void* args[]{&sIdx, &w};
                    if (rt.Invoke(c.unity.smrSetBlendShapeWeight, smr, args, &ret)) {
                        bs.modified = true;
                    }
                }
            }
        } else {
            const bool fullOverride = c.pauseAnim || c.useBindBase;
            for (size_t i = 0; i < c.bones.size(); ++i) {
                auto& b = c.bones[i];
                if (!b.transformHandle) continue;
                Object* tf = api.il2cpp_gchandle_get_target(b.transformHandle);
                if (!tf || !c.unity.IsUnityObjectAlive(rt, tf)) continue;

                Object* ret = nullptr;
                if (fullOverride) {
                    const Quat baseQ = (c.useBindBase && b.hasBindLocalRot) ? b.bindLocalRot : b.capturedLocalRot;
                    UnityQuat q = ToUnity((baseQ * Quat::FromUnityEuler(b.offsetEuler)).Normalized());
                    void* args[]{&q};
                    rt.Invoke(c.unity.tfSetLocalRotation, tf, args, &ret);
                } else if (!b.offsetEuler.IsNearZero()) {
                    UnityQuat cur{};
                    if (UnityBindings::InvokeValue(rt, c.unity.tfGetLocalRotation, tf, nullptr, cur)) {
                        UnityQuat q = ToUnity((FromUnity(cur) * Quat::FromUnityEuler(b.offsetEuler)).Normalized());
                        void* args[]{&q};
                        rt.Invoke(c.unity.tfSetLocalRotation, tf, args, &ret);
                    }
                }
                if (static_cast<int>(i) == c.rootBoneIndex && (fullOverride || c.rootOffset.LengthSq() > 1e-10)) {
                    if (fullOverride) {
                        UnityVec3 p = ToUnity(c.capturedRootLocalPos + c.rootOffset);
                        void* args[]{&p};
                        rt.Invoke(c.unity.tfSetLocalPosition, tf, args, &ret);
                    } else {
                        UnityVec3 cur{};
                        if (UnityBindings::InvokeValue(rt, c.unity.tfGetLocalPosition, tf, nullptr, cur)) {
                            UnityVec3 p = ToUnity(FromUnity(cur) + c.rootOffset);
                            void* args[]{&p};
                            rt.Invoke(c.unity.tfSetLocalPosition, tf, args, &ret);
                        }
                    }
                }
            }
        }
    }

    if (c.mmd.camSettings.enabled && !c.mmd.cameraKeys.empty() && c.mmd.camAnchor.valid &&
        cam && c.unity.IsUnityObjectAlive(rt, cam)) {
        const double camFrame = (c.mmd.motion && c.mmd.motion->fps > 1e-3)
                                    ? (c.mmd.currentFrame * 30.0 / c.mmd.motion->fps)
                                    : c.mmd.camOnlyFrame;
        const auto camPose = mmd::EvaluateMmdCameraUnity(
            c.mmd.cameraKeys, camFrame, c.mmd.camAnchor, c.mmd.camSettings,
            c.mmd.lastAuthoredRootChar, c.mmd.lastAppliedRootChar);
        if (camPose.valid) {
            Object* camTf = nullptr;
            if (rt.Invoke(c.unity.compGetTransform, cam, nullptr, &camTf) && camTf &&
                c.unity.IsUnityObjectAlive(rt, camTf)) {
                UnityVec3 pos = ToUnity(camPose.worldPos);
                UnityQuat rot = ToUnity(camPose.worldRot);
                void* pArgs[]{&pos};
                void* rArgs[]{&rot};
                Object* ret = nullptr;
                rt.Invoke(c.unity.tfSetPosition, camTf, pArgs, &ret);
                rt.Invoke(c.unity.tfSetRotation, camTf, rArgs, &ret);
            }
            if (c.unity.camSetFov) {
                float fov = camPose.verticalFovDeg;
                void* fArgs[]{&fov};
                Object* ret = nullptr;
                rt.Invoke(c.unity.camSetFov, cam, fArgs, &ret);
            }
        }
    } else if (c.orbitEnabled && c.orbit.initialized && cam && c.unity.IsUnityObjectAlive(rt, cam)) {
        Object* camTf = nullptr;
        if (rt.Invoke(c.unity.compGetTransform, cam, nullptr, &camTf) && camTf && c.unity.IsUnityObjectAlive(rt, camTf)) {
            UnityVec3 pos = ToUnity(c.orbit.Position());
            UnityQuat rot = ToUnity(c.orbit.Rotation());
            void* pArgs[]{&pos};
            void* rArgs[]{&rot};
            Object* ret = nullptr;
            rt.Invoke(c.unity.tfSetPosition, camTf, pArgs, &ret);
            rt.Invoke(c.unity.tfSetRotation, camTf, rArgs, &ret);
        }
        if (c.unity.camSetFov) {
            float fov = c.orbit.fovDeg;
            void* fArgs[]{&fov};
            Object* ret = nullptr;
            rt.Invoke(c.unity.camSetFov, cam, fArgs, &ret);
        }
    }

    CameraProjection camProj{};
    if (c.unity.ReadCameraProjectionReflection(rt, cam, camProj) && camProj.viewportH > 1.f) {
        c.lastViewportH = camProj.viewportH;
    }

    auto snap = std::make_shared<RenderSnapshot>();
    snap->revision = ++c.snapshotRevision;
    snap->bound = c.unity.bound;
    snap->postLateHookActive = false;
    snap->targetAlive = c.frameTargetAlive && !c.bones.empty();
    snap->useBindBase = c.useBindBase;
    snap->activeTargetId = c.activeTargetId;
    snap->activeTargetName = c.activeTargetName;
    snap->activeTargetKind = c.activeTargetKind;
    snap->rootBoneIndex = c.rootBoneIndex;
    snap->rootOffset = c.rootOffset;
    snap->rootParentWorldRot = c.rootParentWorldRot;
    snap->rootForward = c.rootForward;
    snap->rootRight = c.rootRight;
    snap->camera = camProj;
    snap->orbit = c.orbit;
    snap->orbitActive = c.orbitEnabled;
    FillMmdUiSnapshot(c, snap->mmd);

    snap->bones.reserve(c.bones.size());
    for (const auto& b : c.bones) {
        BoneRenderData rd;
        rd.name = b.name;
        rd.parent = b.parent;
        rd.restWorldPos = b.restWorldPos;
        rd.restWorldRot = b.restWorldRot;
        rd.restLocalRot = (c.useBindBase && b.hasBindLocalRot) ? b.bindLocalRot : b.capturedLocalRot;
        rd.offsetEuler = b.offsetEuler;
        rd.isBodyBone = b.isBodyBone;
        rd.isFaceBone = b.isFaceBone;
        rd.limit = b.limit;
        rd.ikChain = b.ikChain;
        if (b.transformHandle) {
            Object* tf = api.il2cpp_gchandle_get_target(b.transformHandle);
            if (tf && c.unity.IsUnityObjectAlive(rt, tf)) {
                UnityVec3 wp{};
                UnityQuat wr{};
                if (UnityBindings::InvokeValue(rt, c.unity.tfGetPosition, tf, nullptr, wp)) rd.worldPos = FromUnity(wp);
                if (UnityBindings::InvokeValue(rt, c.unity.tfGetRotation, tf, nullptr, wr)) rd.worldRot = FromUnity(wr);
            }
        }
        snap->bones.push_back(std::move(rd));
    }

    snap->targets.reserve(c.targets.size());
    for (const auto& t : c.targets) {
        TargetEntry uiCopy;
        uiCopy.id = t.id;
        uiCopy.kind = t.kind;
        uiCopy.rawName = t.rawName;
        uiCopy.distanceMeters = t.distanceMeters;
        uiCopy.hasPlayableAnimator = t.hasPlayableAnimator;
        uiCopy.boneCount = t.boneCount;
        snap->targets.push_back(std::move(uiCopy));
    }

    c.published.store(std::move(snap), std::memory_order_release);
}

void CABBIRD_CALL DetourAfterLateUpdate(const CabbirdMethodInfoV1* methodInfo) {
    Context* c = g_activeDetourContext.load(std::memory_order_acquire);
    if (!c || !c->hook) return;

    CabbirdGenerationHandleV1 lease{};
    if (!Succeeded(c->hook->begin_callback(c->hook->user, c->afterLateHook, &lease)) || !lease.id) {
        return;
    }
    struct LeaseGuard {
        const CabbirdHookServiceV1* hook;
        CabbirdGenerationHandleV1 lease;
        ~LeaseGuard() {
            if (hook && hook->end_callback && lease.id) {
                static_cast<void>(hook->end_callback(hook->user, lease));
            }
        }
    } guard{c->hook, lease};

    const auto orig = reinterpret_cast<NativeAfterLateFn>(c->afterLateOriginal);
    if (orig) orig(methodInfo);

    if (!c->running.load(std::memory_order_acquire) || c->stopping.load(std::memory_order_acquire)) return;
    if (GetCurrentThreadId() != c->gameThreadId.load(std::memory_order_relaxed)) return;
    if (c->inDetour.exchange(true, std::memory_order_acq_rel)) return;
    struct ReentryGuard {
        std::atomic_bool& flag;
        ~ReentryGuard() { flag.store(false, std::memory_order_release); }
    } reentry{c->inDetour};

    try {
        PendingCommands fast;
        {
            std::scoped_lock lock(c->commandMutex);
            if (c->pending.hasBoneEdit) {
                fast.hasBoneEdit = true;
                fast.boneEdits.swap(c->pending.boneEdits);
                c->pending.hasBoneEdit = false;
            }
            if (c->pending.hasRootOffsetEdit) {
                fast.hasRootOffsetEdit = true;
                fast.rootOffsetEdit = c->pending.rootOffsetEdit;
                c->pending.hasRootOffsetEdit = false;
            }
            if (c->pending.hasOrbitDelta) {
                fast.hasOrbitDelta = true;
                fast.orbitDx = c->pending.orbitDx;
                fast.orbitDy = c->pending.orbitDy;
                fast.panDx = c->pending.panDx;
                fast.panDy = c->pending.panDy;
                fast.zoomNotches = c->pending.zoomNotches;
                c->pending.orbitDx = c->pending.orbitDy = 0.f;
                c->pending.panDx = c->pending.panDy = 0.f;
                c->pending.zoomNotches = 0.f;
                c->pending.hasOrbitDelta = false;
            }
            if (c->pending.hasOrbitFov) {
                fast.hasOrbitFov = true;
                fast.orbitFov = c->pending.orbitFov;
                c->pending.hasOrbitFov = false;
            }
            if (c->pending.mmdHasSeek) {
                fast.mmdHasSeek = true;
                fast.mmdSeekFrame = c->pending.mmdSeekFrame;
                c->pending.mmdHasSeek = false;
            }
        }
        if (fast.hasBoneEdit) {
            for (const auto& [idx, euler] : fast.boneEdits) {
                if (idx >= 0 && static_cast<size_t>(idx) < c->bones.size()) {
                    c->bones[static_cast<size_t>(idx)].offsetEuler = euler;
                }
            }
        }
        if (fast.hasRootOffsetEdit) {
            c->rootOffset = fast.rootOffsetEdit;
        }
        if (fast.hasOrbitDelta && c->orbitEnabled && c->orbit.initialized) {
            if (fast.orbitDx != 0.f || fast.orbitDy != 0.f) c->orbit.Orbit(fast.orbitDx, fast.orbitDy);
            if (fast.panDx != 0.f || fast.panDy != 0.f) c->orbit.Pan(fast.panDx, fast.panDy, c->lastViewportH);
            if (fast.zoomNotches != 0.f) c->orbit.Zoom(fast.zoomNotches);
        }
        if (fast.hasOrbitFov) {
            c->orbit.fovDeg = std::clamp(fast.orbitFov, OrbitCameraState::kMinFovDeg, OrbitCameraState::kMaxFovDeg);
        }
        if (fast.mmdHasSeek) {
            if (c->mmd.motion) {
                const double maxF = c->mmd.motion->frameCount > 1 ? static_cast<double>(c->mmd.motion->frameCount - 1) : 0.0;
                c->mmd.currentFrame = std::clamp(fast.mmdSeekFrame, 0.0, maxF);
            } else if (!c->mmd.cameraKeys.empty()) {
                c->mmd.camOnlyFrame = std::clamp(fast.mmdSeekFrame, 0.0, static_cast<double>(c->mmd.cameraKeys.back().frame));
            }
        }

        ApplyPoseAndPublishNative(*c);
        ++c->postLateDetourHits;
    } catch (...) {
        LogOnceText(*c, 1U << 3U, c->text.Text("log.detour-failed", "BetterPose: post-LateUpdate pose pass failed."));
    }
}

void TryInstallAfterLateHook(Context& c) {
    if (c.postLateHookAttempted || c.afterLateHook.id || !c.hook || !c.hook->create ||
        !c.unity.afterLateUpdateAddress || !c.unity.nativeDirectVerified) {
        return;
    }
    c.postLateHookAttempted = true;
    g_activeDetourContext.store(&c, std::memory_order_release);
    CabbirdHookRequestV1 req{sizeof(req), CABBIRD_HOOK_V1_FUNCTION,
                             c.unity.afterLateUpdateAddress,
                             reinterpret_cast<void*>(&DetourAfterLateUpdate),
                             StringView("cabbird.better-pose.after-late")};
    const auto st = c.hook->create(c.hook->user, &req, &c.afterLateOriginal, &c.afterLateHook);
    if (!Succeeded(st)) {
        c.afterLateHook = {};
        c.afterLateOriginal = 0;
        LogOnceText(c, 1U << 4U, c.text.Text("log.hook-fallback",
                    "BetterPose: post-LateUpdate hook unavailable; falling back to game tick."));
    }
}

void RemoveAfterLateHook(Context& c) noexcept {
    if (!c.afterLateHook.id || !c.hook || !c.hook->release) return;
    static_cast<void>(c.hook->release(c.hook->user, c.afterLateHook));
    c.afterLateHook = {};
    c.afterLateOriginal = 0;
    g_activeDetourContext.store(nullptr, std::memory_order_release);
}

bool ComputeCharacterOrbitCenter(const Context& c, const Runtime& rt, Vec3& outCenter) {
    const auto& api = rt.Functions();
    if (c.bones.empty()) return false;

    auto accumulate = [&](bool bodyOnly) -> bool {
        Vec3 minP{1e18, 1e18, 1e18};
        Vec3 maxP{-1e18, -1e18, -1e18};
        int count = 0;
        for (const auto& b : c.bones) {
            if (bodyOnly && !b.isBodyBone) continue;
            if (!b.transformHandle) continue;
            Object* tf = api.il2cpp_gchandle_get_target(b.transformHandle);
            UnityVec3 wp{};
            if (!tf || !c.unity.IsUnityObjectAlive(rt, tf) ||
                !UnityBindings::InvokeValue(rt, c.unity.tfGetPosition, tf, nullptr, wp)) {
                continue;
            }
            const Vec3 p = FromUnity(wp);
            if (!p.IsFinite()) continue;
            minP.x = std::min(minP.x, p.x); maxP.x = std::max(maxP.x, p.x);
            minP.y = std::min(minP.y, p.y); maxP.y = std::max(maxP.y, p.y);
            minP.z = std::min(minP.z, p.z); maxP.z = std::max(maxP.z, p.z);
            ++count;
        }
        if (count == 0) return false;
        outCenter = (minP + maxP) * 0.5;
        return outCenter.IsFinite();
    };

    if (accumulate(true)) return true;
    if (accumulate(false)) return true;
    return false;
}

void UpdateOrbitCameraSetup(Context& c, const Runtime& rt, Object* cam, bool wantEnabled) {
    if (!wantEnabled) {
        if (c.orbitEnabled) RestoreOrbitCamera(c, rt);
        return;
    }
    if (!cam || !c.unity.IsUnityObjectAlive(rt, cam)) return;

    // Orbit Camera and MMD Camera are mutually exclusive.
    c.mmd.camSettings.enabled = false;
    EnsureCameraTakeover(c, rt, cam);

    if (!c.orbit.initialized) {
        CameraProjection curCam{};
        if (c.unity.ReadCameraProjectionReflection(rt, cam, curCam)) {
            if (curCam.viewportH > 1.f) c.lastViewportH = curCam.viewportH;
            double orbitDist = 2.5;
            Vec3 charCenter{};
            if (ComputeCharacterOrbitCenter(c, rt, charCenter)) {
                const double range = (charCenter - curCam.cameraPos).Length();
                if (range > 0.35 && range < 25.0) {
                    orbitDist = range;
                }
            }
            const float liveFov = (std::isfinite(curCam.verticalFovDeg) &&
                                   curCam.verticalFovDeg >= OrbitCameraState::kMinFovDeg &&
                                   curCam.verticalFovDeg <= OrbitCameraState::kMaxFovDeg)
                                      ? curCam.verticalFovDeg
                                      : (c.hasSavedCameraFov ? c.savedCameraFov : 45.f);
            // Keep the exact current camera position, orientation, and FOV so turning Orbit Camera on never jumps.
            c.orbit.InitFromView(curCam.cameraPos, curCam.cameraRot, orbitDist, liveFov);
            c.savedOrbitInitial = c.orbit;
            c.hasSavedOrbitInitial = true;
            {
                std::scoped_lock lock(c.settingsMutex);
                c.settings.orbitFov = c.orbit.fovDeg;
            }
        }
    }
    c.orbitEnabled = true;
}

void ApplyAnimationSpeedControl(Context& c, const Runtime& rt) {
    const auto& api = rt.Functions();
    const bool wantModify = c.pauseAnim || std::fabs(c.animSpeed - 1.f) > 1e-3f;
    const float desiredSpeed = c.pauseAnim ? 0.f : c.animSpeed;

    if (wantModify) {
        if (c.activePlayableAnimHandle && c.unity.playableAnimSetSpeed) {
            Object* pa = api.il2cpp_gchandle_get_target(c.activePlayableAnimHandle);
            if (pa && c.unity.IsUnityObjectAlive(rt, pa)) {
                if (!c.hasSavedPlayableSpeed && c.unity.playableAnimGetSpeed) {
                    float orig = 1.f;
                    if (UnityBindings::InvokeValue(rt, c.unity.playableAnimGetSpeed, pa, nullptr, orig)) {
                        c.savedPlayableSpeed = orig;
                        c.hasSavedPlayableSpeed = true;
                    }
                }
                float spd = desiredSpeed;
                void* args[]{&spd};
                Object* ret = nullptr;
                rt.Invoke(c.unity.playableAnimSetSpeed, pa, args, &ret);
                c.animModified = true;
            }
        }
        if (c.activeAnimatorHandle && c.unity.animSetSpeed) {
            Object* an = api.il2cpp_gchandle_get_target(c.activeAnimatorHandle);
            if (an && c.unity.IsUnityObjectAlive(rt, an)) {
                if (!c.hasSavedAnimatorSpeed && c.unity.animGetSpeed) {
                    float orig = 1.f;
                    if (UnityBindings::InvokeValue(rt, c.unity.animGetSpeed, an, nullptr, orig)) {
                        c.savedAnimatorSpeed = orig;
                        c.hasSavedAnimatorSpeed = true;
                    }
                }
                float spd = desiredSpeed;
                void* args[]{&spd};
                Object* ret = nullptr;
                rt.Invoke(c.unity.animSetSpeed, an, args, &ret);
                c.animModified = true;
            }
        }
    } else if (c.animModified) {
        RestoreActiveAnimationAndBones(c, rt, false);
    }
}

CabbirdStatusV1 CABBIRD_CALL GameUpdateCallback(void* raw, const CabbirdIl2CppApiV1* api) noexcept {
    try {
        if (!api || api->struct_size < sizeof(*api) || api->api_version != 1) {
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        }
        auto& c = *static_cast<Context*>(raw);
        if (!c.running.load(std::memory_order_acquire)) return Ok();

        c.gameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
        Runtime rt(*api);

        if (!c.unity.bound) {
            const auto now = std::chrono::steady_clock::now();
            if (now < c.nextBindAttempt) return Ok();
            c.nextBindAttempt = now + std::chrono::seconds(2);
            if (!c.unity.Bind(rt, c.metadata)) {
                const std::string_view args[]{c.unity.bindError};
                LogOnceText(c, 1U << 7U, c.text.Format("log.bind-failed",
                            "BetterPose: Unity metadata binding failed ({0}).", args));
                PublishStatusSnapshot(c, c.unity.bindError);
                return Ok();
            }
        }

        Object* cam = c.unity.GetMainCamera(rt);
        if (cam && !c.unity.nativeDirectVerified && !c.unity.nativeDirectRejected) {
            if (c.unity.VerifyNativeDirectAbi(rt, cam)) {
                TryInstallAfterLateHook(c);
            }
        }

        if (!c.settingsSyncedToGame) {
            c.settingsSyncedToGame = true;
            std::scoped_lock lock(c.settingsMutex);
            c.pauseAnim = (c.settings.pauseAnim != 0);
            c.animSpeed = c.settings.animSpeed;
            c.orbit.fovDeg = c.settings.orbitFov;
        }
        SyncMmdSettingsToGame(c);

        // Install completed Worker-thread MMD motion / camera loads on the Game thread.
        {
            std::optional<PendingMmdMotionResult> incomingMotion;
            std::optional<PendingMmdCameraResult> incomingCamera;
            {
                std::scoped_lock lock(c.mmdMutex);
                if (c.pendingMmdMotion.has_value()) {
                    incomingMotion = std::move(c.pendingMmdMotion);
                    c.pendingMmdMotion.reset();
                }
                if (c.pendingMmdCamera.has_value()) {
                    incomingCamera = std::move(c.pendingMmdCamera);
                    c.pendingMmdCamera.reset();
                }
            }
            if (incomingMotion && incomingMotion->motion &&
                incomingMotion->builtForTargetId == c.activeTargetId &&
                incomingMotion->builtForBoneCount == c.bones.size()) {
                RestoreBlendShapes(c, rt);
                c.mmd.motion = std::move(incomingMotion->motion);
                c.mmd.morphBindings = std::move(incomingMotion->morphBindings);
                c.mmd.morphWeightsBuf.assign(c.blendShapes.size(), 0.f);
                c.mmd.morphTouchedBuf.assign(c.blendShapes.size(), 0U);
                c.mmd.currentFrame = 0.0;
                c.mmd.playing = false;
                c.mmd.hasLastTick = false;
                c.mmd.lastAuthoredRootChar = {};
                c.mmd.lastAppliedRootChar = {};
                CaptureMmdCameraAnchor(c, rt);
                if (!c.mmd.motion->embeddedCameraKeys.empty() && c.mmd.cameraKeys.empty()) {
                    c.mmd.cameraKeys = c.mmd.motion->embeddedCameraKeys;
                    c.mmd.cameraDisplayName = c.mmd.motion->displayName;
                }
            }
            if (incomingCamera && !incomingCamera->keys.empty()) {
                c.mmd.cameraKeys = std::move(incomingCamera->keys);
                c.mmd.cameraDisplayName = std::move(incomingCamera->displayName);
                c.mmd.camOnlyFrame = 0.0;
                CaptureMmdCameraAnchor(c, rt);
                if (cam && c.unity.IsUnityObjectAlive(rt, cam)) {
                    if (c.orbitEnabled) RestoreOrbitCamera(c, rt);
                    c.mmd.camSettings.enabled = true;
                    EnsureCameraTakeover(c, rt, cam);
                }
            }
        }

        // Drain commands from UI / Render thread.
        PendingCommands cmd;
        {
            std::scoped_lock lock(c.commandMutex);
            cmd = std::move(c.pending);
            c.pending = {};
        }

        // Auto-retry target discovery while no active target is bound (handles first-tick entity warmup).
        const auto now = std::chrono::steady_clock::now();
        const bool autoScanDue = !c.frameTargetAlive && now >= c.nextAutoScanAttempt;
        if (autoScanDue) {
            c.nextAutoScanAttempt = now + std::chrono::milliseconds(1500);
        }

        if (cmd.refreshTargets || cmd.scanSceneRigs || autoScanDue) {
            RefreshTargetList(c, rt, cmd.scanSceneRigs);
            if (!c.frameTargetAlive && !c.targets.empty()) {
                LoadSkeletonFromTarget(c, rt, c.targets.front());
            }
        }
        if (cmd.selectLocalPlayer) {
            RefreshTargetList(c, rt, false);
            for (const auto& t : c.targets) {
                if (t.kind == TargetKind::LocalPlayer) {
                    LoadSkeletonFromTarget(c, rt, t);
                    break;
                }
            }
        }
        if (cmd.hasSelectTarget) {
            for (const auto& t : c.targets) {
                if (t.id == cmd.selectTargetId) {
                    LoadSkeletonFromTarget(c, rt, t);
                    break;
                }
            }
        }
        if (cmd.reloadBones) {
            bool reloaded = false;
            for (const auto& t : c.targets) {
                if (t.id == c.activeTargetId) {
                    reloaded = LoadSkeletonFromTarget(c, rt, t);
                    break;
                }
            }
            if (!reloaded) {
                RefreshTargetList(c, rt, false);
                if (!c.targets.empty()) LoadSkeletonFromTarget(c, rt, c.targets.front());
            }
        }
        if (cmd.toggleBindBase) {
            c.useBindBase = cmd.wantBindBase;
        }
        if (cmd.hasPauseToggle) {
            const bool wasPaused = c.pauseAnim;
            c.pauseAnim = cmd.wantPause;
            if (c.pauseAnim && !wasPaused && c.frameTargetAlive && !c.useBindBase && !c.mmd.motion) {
                for (size_t i = 0; i < c.bones.size(); ++i) {
                    Object* tf = api->il2cpp_gchandle_get_target(c.bones[i].transformHandle);
                    if (!tf || !c.unity.IsUnityObjectAlive(rt, tf)) continue;
                    UnityQuat lr{};
                    if (UnityBindings::InvokeValue(rt, c.unity.tfGetLocalRotation, tf, nullptr, lr)) {
                        c.bones[i].capturedLocalRot = FromUnity(lr);
                    }
                    if (static_cast<int>(i) == c.rootBoneIndex) {
                        UnityVec3 lp{};
                        if (UnityBindings::InvokeValue(rt, c.unity.tfGetLocalPosition, tf, nullptr, lp)) {
                            c.capturedRootLocalPos = FromUnity(lp);
                        }
                    }
                }
            }
        }
        if (cmd.hasSpeedEdit) {
            c.animSpeed = std::clamp(cmd.wantSpeed, 0.f, 3.f);
        }
        if (cmd.resetAll) {
            for (auto& b : c.bones) b.offsetEuler = {};
            c.rootOffset = {};
            c.useBindBase = false;
        }
        if (cmd.refreshAnim) {
            for (auto& b : c.bones) b.offsetEuler = {};
            c.rootOffset = {};
            c.useBindBase = false;
            c.pauseAnim = false;
            c.animSpeed = 1.f;
            {
                std::scoped_lock lock(c.settingsMutex);
                c.settings.pauseAnim = 0;
                c.settings.animSpeed = 1.f;
            }
            QueueSaveSettings(c);
            RestoreActiveAnimationAndBones(c, rt, true);
            if (c.activePlayableAnimHandle && c.unity.playableAnimRebind) {
                Object* pa = api->il2cpp_gchandle_get_target(c.activePlayableAnimHandle);
                if (pa && c.unity.IsUnityObjectAlive(rt, pa)) {
                    Object* ret = nullptr;
                    rt.Invoke(c.unity.playableAnimRebind, pa, nullptr, &ret);
                }
            }
        }
        if (cmd.hasBulkOffsets && cmd.bulkOffsets.size() == c.bones.size()) {
            for (size_t i = 0; i < c.bones.size(); ++i) c.bones[i].offsetEuler = cmd.bulkOffsets[i];
        }
        if (cmd.hasBoneEdit) {
            for (const auto& [idx, euler] : cmd.boneEdits) {
                if (idx >= 0 && static_cast<size_t>(idx) < c.bones.size()) {
                    c.bones[static_cast<size_t>(idx)].offsetEuler = euler;
                }
            }
        }
        if (cmd.hasRootOffsetEdit) {
            c.rootOffset = cmd.rootOffsetEdit;
        }
        if (cmd.hasMirror && !c.bones.empty()) {
            std::vector<MirrorBoneInput> inputs;
            inputs.reserve(c.bones.size());
            for (const auto& b : c.bones) {
                inputs.push_back({
                    b.name, b.parent,
                    (c.useBindBase && b.hasBindLocalRot) ? b.bindLocalRot : b.capturedLocalRot,
                    b.offsetEuler
                });
            }
            std::vector<EulerAngles> mirrored;
            const int changed = ApplyPoseMirror(inputs, c.rootBoneIndex, cmd.mirrorMode, mirrored, c.rootOffset);
            for (size_t i = 0; i < c.bones.size(); ++i) c.bones[i].offsetEuler = mirrored[i];
            const std::string countStr = std::to_string(changed);
            const std::string_view args[]{countStr};
            SetNotice(c, c.text.Format("notice.mirrored", "Mirrored {0} bones.", args));
        }

        if (cmd.hasOrbitToggle) {
            UpdateOrbitCameraSetup(c, rt, cam, cmd.wantOrbitEnabled);
        } else if (c.orbitEnabled) {
            UpdateOrbitCameraSetup(c, rt, cam, true);
        }
        if (c.orbitEnabled && c.orbit.initialized) {
            if (cmd.hasOrbitDelta) {
                if (cmd.orbitDx != 0.f || cmd.orbitDy != 0.f) c.orbit.Orbit(cmd.orbitDx, cmd.orbitDy);
                if (cmd.panDx != 0.f || cmd.panDy != 0.f) c.orbit.Pan(cmd.panDx, cmd.panDy, c.lastViewportH);
                if (cmd.zoomNotches != 0.f) c.orbit.Zoom(cmd.zoomNotches);
            }
            if (cmd.hasOrbitFov) {
                c.orbit.fovDeg = std::clamp(cmd.orbitFov, OrbitCameraState::kMinFovDeg, OrbitCameraState::kMaxFovDeg);
            }
            if (cmd.orbitFocusSelected && cmd.orbitFocusBone >= 0 &&
                static_cast<size_t>(cmd.orbitFocusBone) < c.bones.size()) {
                Object* tf = api->il2cpp_gchandle_get_target(c.bones[static_cast<size_t>(cmd.orbitFocusBone)].transformHandle);
                UnityVec3 wp{};
                if (tf && c.unity.IsUnityObjectAlive(rt, tf) &&
                    UnityBindings::InvokeValue(rt, c.unity.tfGetPosition, tf, nullptr, wp)) {
                    c.orbit.FocusOn(FromUnity(wp), 1.8);
                }
            }
            if (cmd.orbitResetView) {
                if (c.hasSavedOrbitInitial) {
                    c.orbit = c.savedOrbitInitial;
                    std::scoped_lock lock(c.settingsMutex);
                    c.settings.orbitFov = c.orbit.fovDeg;
                } else {
                    c.orbit.initialized = false;
                    UpdateOrbitCameraSetup(c, rt, cam, true);
                }
            }
        }

        if (cmd.mmdSyncSettings) {
            const bool prevExpr = c.mmd.playExpressions;
            SyncMmdSettingsToGame(c);
            if (prevExpr && !c.mmd.playExpressions) {
                RestoreBlendShapes(c, rt);
            }
        }
        if (cmd.mmdTogglePlay) {
            if (c.mmd.motion || !c.mmd.cameraKeys.empty()) {
                c.mmd.playing = !c.mmd.playing;
                c.mmd.hasLastTick = false;
                if (c.mmd.playing && c.mmd.motion && c.mmd.motion->frameCount > 1 &&
                    c.mmd.currentFrame >= static_cast<double>(c.mmd.motion->frameCount - 1) - 1e-3) {
                    c.mmd.currentFrame = 0.0;
                }
            }
        }
        if (cmd.mmdSetPlaying) {
            if (c.mmd.motion || !c.mmd.cameraKeys.empty()) {
                c.mmd.playing = cmd.mmdWantPlaying;
                c.mmd.hasLastTick = false;
                if (c.mmd.playing && c.mmd.motion && c.mmd.motion->frameCount > 1 &&
                    c.mmd.currentFrame >= static_cast<double>(c.mmd.motion->frameCount - 1) - 1e-3) {
                    c.mmd.currentFrame = 0.0;
                }
            }
        }
        if (cmd.mmdStop) {
            c.mmd.playing = false;
            c.mmd.currentFrame = 0.0;
            c.mmd.camOnlyFrame = 0.0;
            c.mmd.hasLastTick = false;
            c.mmd.lastAuthoredRootChar = {};
            c.mmd.lastAppliedRootChar = {};
            RestoreBlendShapes(c, rt);
        }
        if (cmd.mmdUnload) {
            RestoreBlendShapes(c, rt);
            if (c.mmd.motion) {
                Object* ret = nullptr;
                for (const auto& offTrack : c.mmd.motion->boneOffsetTracks) {
                    const int idx = offTrack.targetBoneIndex;
                    if (idx < 0 || static_cast<size_t>(idx) >= c.bones.size()) continue;
                    Object* tf = c.bones[static_cast<size_t>(idx)].transformHandle
                                     ? api->il2cpp_gchandle_get_target(c.bones[static_cast<size_t>(idx)].transformHandle)
                                     : nullptr;
                    if (tf && c.unity.IsUnityObjectAlive(rt, tf)) {
                        UnityVec3 p = ToUnity(c.bones[static_cast<size_t>(idx)].capturedLocalPos);
                        void* pArgs[]{&p};
                        rt.Invoke(c.unity.tfSetLocalPosition, tf, pArgs, &ret);
                    }
                }
                const int rIdx = c.mmd.motion->rootBoneIndex;
                if (rIdx >= 0 && rIdx != c.rootBoneIndex && static_cast<size_t>(rIdx) < c.bones.size()) {
                    Object* tf = c.bones[static_cast<size_t>(rIdx)].transformHandle
                                     ? api->il2cpp_gchandle_get_target(c.bones[static_cast<size_t>(rIdx)].transformHandle)
                                     : nullptr;
                    if (tf && c.unity.IsUnityObjectAlive(rt, tf)) {
                        UnityVec3 p = ToUnity(c.bones[static_cast<size_t>(rIdx)].capturedLocalPos);
                        void* pArgs[]{&p};
                        rt.Invoke(c.unity.tfSetLocalPosition, tf, pArgs, &ret);
                    }
                }
            }
            c.mmd.motion.reset();
            c.mmd.morphBindings.clear();
            c.mmd.playing = false;
            c.mmd.currentFrame = 0.0;
            c.mmd.lastAuthoredRootChar = {};
            c.mmd.lastAppliedRootChar = {};
            RestoreActiveAnimationAndBones(c, rt, true);
        }
        if (cmd.mmdHasSeek) {
            if (c.mmd.motion) {
                const double maxF = c.mmd.motion->frameCount > 1 ? static_cast<double>(c.mmd.motion->frameCount - 1) : 0.0;
                c.mmd.currentFrame = std::clamp(cmd.mmdSeekFrame, 0.0, maxF);
            } else if (!c.mmd.cameraKeys.empty()) {
                c.mmd.camOnlyFrame = std::clamp(cmd.mmdSeekFrame, 0.0, static_cast<double>(c.mmd.cameraKeys.back().frame));
            }
        }
        if (cmd.mmdCamClear) {
            c.mmd.camSettings.enabled = false;
            c.mmd.cameraKeys.clear();
            c.mmd.cameraDisplayName.clear();
            c.mmd.camAnchor.valid = false;
            ReleaseCameraTakeoverIfIdle(c, rt);
        }
        if (cmd.mmdCamHasEnable) {
            if (cmd.mmdCamWantEnable && !c.mmd.cameraKeys.empty() && cam && c.unity.IsUnityObjectAlive(rt, cam)) {
                if (c.orbitEnabled) RestoreOrbitCamera(c, rt);
                c.mmd.camSettings.enabled = true;
                EnsureCameraTakeover(c, rt, cam);
                CaptureMmdCameraAnchor(c, rt);
            } else {
                c.mmd.camSettings.enabled = false;
                ReleaseCameraTakeoverIfIdle(c, rt);
            }
        } else if (c.mmd.camSettings.enabled && cam && c.unity.IsUnityObjectAlive(rt, cam)) {
            EnsureCameraTakeover(c, rt, cam);
            if (!c.mmd.camAnchor.valid) CaptureMmdCameraAnchor(c, rt);
        }
        if (cmd.mmdCamReanchor) {
            CaptureMmdCameraAnchor(c, rt);
        }

        Object* activeGo = c.activeGoHandle ? api->il2cpp_gchandle_get_target(c.activeGoHandle) : nullptr;
        if (c.activeGoHandle && (!activeGo || !c.unity.IsUnityObjectAlive(rt, activeGo))) {
            ClearActiveSkeleton(c, rt, false);
        }
        c.frameTargetAlive = (activeGo != nullptr && !c.bones.empty());
        c.frameRootParentTfPtr = nullptr;

        if (c.frameTargetAlive) {
            ApplyAnimationSpeedControl(c, rt);
            for (auto& s : c.activeSmrs) {
                Object* smr = s.smrHandle ? api->il2cpp_gchandle_get_target(s.smrHandle) : nullptr;
                s.framePtr = (smr && c.unity.IsUnityObjectAlive(rt, smr)) ? smr : nullptr;
            }
            for (auto& b : c.bones) {
                Object* tf = b.transformHandle ? api->il2cpp_gchandle_get_target(b.transformHandle) : nullptr;
                b.framePtr = (tf && c.unity.IsUnityObjectAlive(rt, tf)) ? tf : nullptr;
            }
            // Reflection fallback for facial BlendShapes if native direct BlendShape ABI is unavailable.
            if (c.mmd.motion && c.mmd.playExpressions && !c.unity.nativeBlendVerified &&
                c.unity.smrSetBlendShapeWeight && !c.mmd.morphBindings.empty() &&
                c.afterLateHook.id != 0 && c.unity.nativeDirectVerified && c.postLateDetourHits > 0) {
                const double vmdFrame = (c.mmd.motion->fps > 1e-3)
                                            ? (c.mmd.currentFrame * 30.0 / c.mmd.motion->fps)
                                            : c.mmd.currentFrame;
                EvaluateMmdMorphsNoAlloc(c, vmdFrame);
                Object* ret = nullptr;
                for (size_t i = 0; i < c.blendShapes.size(); ++i) {
                    if (!c.mmd.morphTouchedBuf[i]) continue;
                    auto& bs = c.blendShapes[i];
                    if (bs.smrIndex < 0 || static_cast<size_t>(bs.smrIndex) >= c.activeSmrs.size()) continue;
                    Object* smr = c.activeSmrs[static_cast<size_t>(bs.smrIndex)].framePtr;
                    if (!smr) continue;
                    int32_t sIdx = bs.shapeIndex;
                    float w = c.mmd.morphWeightsBuf[i];
                    void* args[]{&sIdx, &w};
                    if (rt.Invoke(c.unity.smrSetBlendShapeWeight, smr, args, &ret)) {
                        bs.modified = true;
                    }
                }
            }
            if (c.rootBoneIndex >= 0 && static_cast<size_t>(c.rootBoneIndex) < c.bones.size()) {
                Object* rootTf = c.bones[static_cast<size_t>(c.rootBoneIndex)].framePtr;
                if (rootTf) {
                    Object* parentTf = nullptr;
                    if (rt.Invoke(c.unity.tfGetParent, rootTf, nullptr, &parentTf) && parentTf &&
                        c.unity.IsUnityObjectAlive(rt, parentTf)) {
                        c.frameRootParentTfPtr = parentTf;
                        UnityQuat pr{};
                        if (UnityBindings::InvokeValue(rt, c.unity.tfGetRotation, parentTf, nullptr, pr)) {
                            c.rootParentWorldRot = FromUnity(pr);
                        }
                    }
                }
            }
        }

        c.frameCameraPtr = (cam && c.unity.IsUnityObjectAlive(rt, cam)) ? cam : nullptr;
        c.frameCameraTfPtr = nullptr;
        if (c.frameCameraPtr) {
            Object* camTf = nullptr;
            if (rt.Invoke(c.unity.compGetTransform, c.frameCameraPtr, nullptr, &camTf) && camTf &&
                c.unity.IsUnityObjectAlive(rt, camTf)) {
                c.frameCameraTfPtr = camTf;
            }
        }

        if (!c.afterLateHook.id || !c.unity.nativeDirectVerified || c.postLateDetourHits == 0) {
            ApplyPoseAndPublishReflection(c, rt, cam);
        }
        return Ok();
    } catch (...) {
        return Status(CABBIRD_STATUS_V1_FAILED);
    }
}

CabbirdStatusV1 CABBIRD_CALL CleanupWithRuntime(void* raw, const CabbirdIl2CppApiV1* api) noexcept {
    try {
        if (!api || api->struct_size < sizeof(*api) || api->api_version != 1) return Ok();
        auto& c = *static_cast<Context*>(raw);
        Runtime rt(*api);
        c.mmd.camSettings.enabled = false;
        RestoreOrbitCamera(c, rt);
        ReleaseCameraTakeoverIfIdle(c, rt);
        ClearActiveSkeleton(c, rt, true);
        ClearTargetList(c, *api);
        c.unity.ReleaseTypeHandles(api);
        return Ok();
    } catch (...) {
        return Status(CABBIRD_STATUS_V1_FAILED);
    }
}

CabbirdStatusV1 CABBIRD_CALL StopCleanup(void* raw) noexcept {
    auto& c = *static_cast<Context*>(raw);
    c.stopping.store(true, std::memory_order_release);
    c.running.store(false, std::memory_order_release);
    RemoveAfterLateHook(c);
    if (c.il2cpp && c.il2cpp->with_runtime) {
        static_cast<void>(c.il2cpp->with_runtime(c.il2cpp->user, CleanupWithRuntime, &c));
    }
    return Ok();
}

// ---------- Worker Async File I/O & MMD Retargeting (NEVER runs on Game/Render threads) ----------

std::wstring Utf8ToWideStr(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string WideToUtf8Str(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::filesystem::path PathFromUtf8(std::string_view utf8) {
    const std::wstring w = Utf8ToWideStr(utf8);
    return w.empty() ? std::filesystem::path(utf8) : std::filesystem::path(w);
}

std::string PathToUtf8(const std::filesystem::path& p) {
    return WideToUtf8Str(p.native());
}

bool PathHasExtLower(const std::filesystem::path& p, std::string_view extLower) {
    return ToLowerAscii(PathToUtf8(p.extension())) == extLower;
}

std::optional<std::string> BrowseFileDialogWorker(std::string_view initialUtf8, bool cameraOnly) {
    struct ComGuard {
        HRESULT hr{};
        ComGuard() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
        ~ComGuard() {
            if (SUCCEEDED(hr)) CoUninitialize();
        }
        bool Ok() const noexcept { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
    } com;
    if (!com.Ok()) return std::nullopt;

    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    DWORD options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        static_cast<void>(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST));
    }
    const COMDLG_FILTERSPEC motionFilters[]{
        {L"MMD Motion / Pose (*.vmd;*.vpd)", L"*.vmd;*.vpd"},
        {L"VMD Motion (*.vmd)", L"*.vmd"},
        {L"VPD Pose (*.vpd)", L"*.vpd"},
        {L"All Files (*.*)", L"*.*"}
    };
    const COMDLG_FILTERSPEC camFilters[]{
        {L"MMD Camera VMD (*.vmd)", L"*.vmd"},
        {L"All Files (*.*)", L"*.*"}
    };
    if (cameraOnly) {
        static_cast<void>(dialog->SetFileTypes(static_cast<UINT>(std::size(camFilters)), camFilters));
    } else {
        static_cast<void>(dialog->SetFileTypes(static_cast<UINT>(std::size(motionFilters)), motionFilters));
    }
    static_cast<void>(dialog->SetFileTypeIndex(1));

    if (!initialUtf8.empty()) {
        std::filesystem::path cur = PathFromUtf8(initialUtf8);
        std::error_code ec;
        std::filesystem::path folder = std::filesystem::is_directory(cur, ec) ? cur : cur.parent_path();
        if (!folder.empty() && std::filesystem::is_directory(folder, ec)) {
            Microsoft::WRL::ComPtr<IShellItem> folderItem;
            if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&folderItem)))) {
                static_cast<void>(dialog->SetFolder(folderItem.Get()));
            }
        }
    }

    if (FAILED(dialog->Show(nullptr))) return std::nullopt;
    Microsoft::WRL::ComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(&selected))) return std::nullopt;
    PWSTR raw{};
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return std::nullopt;
    std::string utf8 = WideToUtf8Str(raw);
    CoTaskMemFree(raw);
    if (utf8.empty()) return std::nullopt;
    return utf8;
}

std::filesystem::path ResolveStateFilePath(const Context& c, std::string_view rawPath) {
    std::filesystem::path p = PathFromUtf8(rawPath);
    if (p.is_absolute()) return p;
    std::filesystem::path base = c.writableStateDir.empty() ? std::filesystem::current_path()
                                                            : PathFromUtf8(c.writableStateDir);
    return base / "poses" / p;
}

std::filesystem::path ResolveReferenceAssetPath(const Context& c, int refMode) {
    const char* file = mmd::ReferencePresetFilename(refMode);
    if (!c.pluginDir.empty()) {
        const auto candidate = PathFromUtf8(c.pluginDir) / "assets" / file;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return std::filesystem::current_path() / "assets" / file;
}

void ExecuteLoadMmdMotionWorker(Context& c, const std::string& pathUtf8,
                                int refMode, const mmd::RetargetOptions& opts) {
    struct LoadingGuard {
        std::atomic_bool& flag;
        ~LoadingGuard() { flag.store(false, std::memory_order_release); }
    } loadingGuard{c.mmdLoading};

    if (pathUtf8.empty()) return;
    const std::filesystem::path filePath = PathFromUtf8(pathUtf8);
    const std::string displayName = PathToUtf8(filePath.filename());

    mmd::TargetSkeletonInput targetSkel;
    std::vector<mmd::MorphTargetSlot> morphSlots;
    uint64_t targetId = 0;
    {
        std::scoped_lock lock(c.mmdMutex);
        targetId = c.targetSkelTargetId;
        targetSkel = c.targetSkelForMmd;
        morphSlots = c.morphTargetSlotsForMmd;
    }
    if (targetSkel.bones.empty()) {
        SetNotice(c, c.text.Text("notice.no-skeleton", "Load a target skeleton before importing."));
        return;
    }

    const std::filesystem::path refPath = ResolveReferenceAssetPath(c, refMode);
    mmd::ReferenceSkeleton refSkel;
    std::string err;
    if (!mmd::LoadReferenceSkeletonFile(refPath, refSkel, err)) {
        const std::string refName = PathToUtf8(refPath.filename());
        const std::string_view args[]{refName};
        SetNotice(c, c.text.Format("notice.mmd-no-ref", "Missing reference skeleton asset: {0}", args));
        return;
    }

    mmd::VmdDocument doc;
    const bool okParse = PathHasExtLower(filePath, ".vpd")
                             ? mmd::ParseVpdFile(filePath, doc, err)
                             : mmd::ParseVmdFile(filePath, doc, err);
    if (!okParse) {
        const std::string_view args[]{err};
        SetNotice(c, c.text.Format("notice.mmd-failed", "Failed to load motion: {0}", args));
        return;
    }

    auto built = std::make_shared<mmd::LoadedMotion>();
    if (!mmd::BuildUnityMotion(doc, refSkel, targetSkel, opts, *built, err)) {
        const std::string_view args[]{err};
        SetNotice(c, c.text.Format("notice.mmd-failed", "Failed to load motion: {0}", args));
        return;
    }
    built->sourcePath = pathUtf8;
    built->displayName = displayName;
    built->referenceMode = refMode;

    auto bindings = mmd::BuildMorphBindings(built->morphTracks, morphSlots);

    const std::string frameStr = std::to_string(built->frameCount);
    const std::string boneStr = std::to_string(built->mappedBoneCount);
    const size_t builtBoneCount = targetSkel.bones.size();
    {
        std::scoped_lock lock(c.mmdMutex);
        c.pendingMmdMotion = PendingMmdMotionResult{std::move(built), std::move(bindings), targetId, builtBoneCount};
    }

    const std::string_view args[]{displayName, frameStr, boneStr};
    SetNotice(c, c.text.Format("notice.mmd-loaded", "Loaded {0}: {1} frames, {2} bones mapped", args));
}

void ExecuteLoadMmdCameraWorker(Context& c, const std::string& pathUtf8) {
    if (pathUtf8.empty()) return;
    const std::filesystem::path filePath = PathFromUtf8(pathUtf8);
    const std::string displayName = PathToUtf8(filePath.filename());

    mmd::VmdDocument doc;
    std::string err;
    if (!mmd::ParseVmdFile(filePath, doc, err)) {
        const std::string_view args[]{err};
        SetNotice(c, c.text.Format("notice.mmd-failed", "Failed to load motion: {0}", args));
        return;
    }
    if (doc.cameraKeys.empty()) {
        const std::string_view args[]{displayName};
        SetNotice(c, c.text.Format("notice.mmd-cam-empty", "No camera keyframes found in {0}", args));
        return;
    }
    const std::string keyStr = std::to_string(doc.cameraKeys.size());
    {
        std::scoped_lock lock(c.mmdMutex);
        c.pendingMmdCamera = PendingMmdCameraResult{std::move(doc.cameraKeys), displayName};
    }
    const std::string_view args[]{displayName, keyStr};
    SetNotice(c, c.text.Format("notice.mmd-cam-loaded", "Loaded camera {0} ({1} keys)", args));
}

void CABBIRD_CALL RunWorkerIoTask(void* raw, CabbirdGenerationHandleV1) noexcept {
    auto& c = *static_cast<Context*>(raw);
    try {
        std::vector<WorkerIoJob> batch;
        {
            std::scoped_lock lock(c.ioMutex);
            batch.swap(c.ioQueue);
        }
        for (const auto& job : batch) {
            if (job.kind == WorkerJobKind::BrowseMmdMotion) {
                if (c.fileDialogOpen.exchange(true, std::memory_order_acq_rel)) continue;
                std::optional<std::string> picked;
                try {
                    picked = BrowseFileDialogWorker(job.path, false);
                } catch (...) {}
                c.fileDialogOpen.store(false, std::memory_order_release);
                if (picked && !picked->empty()) {
                    {
                        std::scoped_lock lock(c.mmdMutex);
                        c.pickedMotionPath = *picked;
                    }
                    {
                        std::scoped_lock lock(c.settingsMutex);
                        if (picked->size() <= 520) c.settings.mmdMotionPath = *picked;
                    }
                    QueueSaveSettings(c);
                    c.mmdLoading.store(true, std::memory_order_release);
                    ExecuteLoadMmdMotionWorker(c, *picked, job.mmdReferenceMode, job.mmdOptions);
                }
                continue;
            }
            if (job.kind == WorkerJobKind::LoadMmdMotion) {
                c.mmdLoading.store(true, std::memory_order_release);
                ExecuteLoadMmdMotionWorker(c, job.path, job.mmdReferenceMode, job.mmdOptions);
                continue;
            }
            if (job.kind == WorkerJobKind::BrowseMmdCamera) {
                if (c.fileDialogOpen.exchange(true, std::memory_order_acq_rel)) continue;
                std::optional<std::string> picked;
                try {
                    picked = BrowseFileDialogWorker(job.path, true);
                } catch (...) {}
                c.fileDialogOpen.store(false, std::memory_order_release);
                if (picked && !picked->empty()) {
                    {
                        std::scoped_lock lock(c.mmdMutex);
                        c.pickedCameraPath = *picked;
                    }
                    {
                        std::scoped_lock lock(c.settingsMutex);
                        if (picked->size() <= 520) c.settings.mmdCameraPath = *picked;
                    }
                    QueueSaveSettings(c);
                    ExecuteLoadMmdCameraWorker(c, *picked);
                }
                continue;
            }
            if (job.kind == WorkerJobKind::LoadMmdCamera) {
                ExecuteLoadMmdCameraWorker(c, job.path);
                continue;
            }

            const auto fullPath = ResolveStateFilePath(c, job.path);
            const std::string displayPath = PathToUtf8(fullPath.filename());
            if (job.kind == WorkerJobKind::ExportPose || job.kind == WorkerJobKind::ExportSkeleton) {
                std::error_code ec;
                if (fullPath.has_parent_path()) {
                    std::filesystem::create_directories(fullPath.parent_path(), ec);
                }
                std::ofstream out(fullPath, std::ios::binary | std::ios::trunc);
                if (!out.is_open()) {
                    const std::string_view args[]{displayPath};
                    SetNotice(c, c.text.Format("notice.export-failed", "Failed to write {0}.", args));
                    continue;
                }
                out.write(job.jsonPayload.data(), static_cast<std::streamsize>(job.jsonPayload.size()));
                out.flush();
                if (!out.good()) {
                    const std::string_view args[]{displayPath};
                    SetNotice(c, c.text.Format("notice.export-failed", "Failed to write {0}.", args));
                    continue;
                }
                const std::string_view args[]{displayPath};
                SetNotice(c, job.kind == WorkerJobKind::ExportSkeleton
                    ? c.text.Format("notice.skeleton-exported", "Exported skeleton to {0}.", args)
                    : c.text.Format("notice.pose-exported", "Exported pose to {0}.", args));
            } else if (job.kind == WorkerJobKind::ImportPose) {
                std::ifstream in(fullPath, std::ios::binary);
                if (!in.is_open()) {
                    const std::string_view args[]{displayPath};
                    SetNotice(c, c.text.Format("notice.import-not-found", "Could not open {0}.", args));
                    continue;
                }
                Json doc = Json::parse(in, nullptr, false);
                if (doc.is_discarded() || !doc.is_object()) {
                    const std::string_view args[]{displayPath};
                    SetNotice(c, c.text.Format("notice.import-invalid", "Invalid pose JSON in {0}.", args));
                    continue;
                }
                const auto latest = c.published.load(std::memory_order_acquire);
                std::vector<std::string> boneNames;
                if (latest) {
                    boneNames.reserve(latest->bones.size());
                    for (const auto& b : latest->bones) boneNames.push_back(b.name);
                }
                if (boneNames.empty()) {
                    SetNotice(c, c.text.Text("notice.no-skeleton", "Load a target skeleton before importing."));
                    continue;
                }
                std::vector<EulerAngles> importedOffsets;
                Vec3 importedRoot{};
                PoseImportStats stats{};
                if (!DeserializePoseJson(doc, boneNames, importedOffsets, importedRoot, stats)) {
                    const std::string_view args[]{displayPath};
                    SetNotice(c, c.text.Format("notice.import-invalid", "Invalid pose JSON in {0}.", args));
                    continue;
                }
                {
                    std::scoped_lock lock(c.commandMutex);
                    c.pending.hasBulkOffsets = true;
                    c.pending.bulkOffsets = std::move(importedOffsets);
                    c.pending.hasRootOffsetEdit = true;
                    c.pending.rootOffsetEdit = importedRoot;
                }
                const std::string sName = std::to_string(stats.matchedByName);
                const std::string sIdx = std::to_string(stats.matchedByIndex);
                const std::string sSkip = std::to_string(stats.skipped);
                const std::string_view args[]{sName, sIdx, sSkip};
                SetNotice(c, c.text.Format("notice.pose-imported",
                    "Imported pose: {0} by name, {1} by index, {2} skipped.", args));
            }
        }
    } catch (...) {
        c.mmdLoading.store(false, std::memory_order_release);
        c.fileDialogOpen.store(false, std::memory_order_release);
        SetNotice(c, c.text.Text("notice.io-error", "File operation failed."));
    }
}

void QueueWorkerIo(Context& c, WorkerIoJob job) {
    if (!c.scheduler || !c.scheduler->schedule) {
        SetNotice(c, c.text.Text("notice.io-error", "File operation failed."));
        return;
    }
    {
        std::scoped_lock lock(c.ioMutex);
        c.ioQueue.push_back(std::move(job));
    }
    CabbirdGenerationHandleV1 task{};
    static_cast<void>(c.scheduler->schedule(c.scheduler->user, 0, RunWorkerIoTask, &c, &task));
}

// ---------- Render-thread Pose Editing, Picking, Gizmo & Overlay Drawing ----------

PoseSnapshot CaptureLiveSnapshot(const RenderSnapshot& snap) {
    PoseSnapshot out;
    out.rootOffset = snap.rootOffset;
    out.boneOffsets.reserve(snap.bones.size());
    for (const auto& b : snap.bones) out.boneOffsets.push_back(b.offsetEuler);
    return out;
}

void QueueBoneOffsetEdit(Context& c, int boneIndex, const EulerAngles& euler) {
    if (boneIndex < 0 || static_cast<size_t>(boneIndex) >= c.renderSnap.bones.size()) return;
    EulerAngles wrapped{WrapAngle180(euler.pitch), WrapAngle180(euler.yaw), WrapAngle180(euler.roll)};
    c.renderSnap.bones[static_cast<size_t>(boneIndex)].offsetEuler = wrapped;
    if (c.drag.mode != DragMode::None && static_cast<size_t>(boneIndex) < c.drag.currentOffsets.size()) {
        c.drag.currentOffsets[static_cast<size_t>(boneIndex)] = wrapped;
    }
    {
        std::scoped_lock lock(c.commandMutex);
        c.pending.hasBoneEdit = true;
        c.pending.boneEdits.emplace_back(boneIndex, wrapped);
    }
    c.history.MarkDirty(NowMs());
}

void QueueRootOffsetEdit(Context& c, const Vec3& offset) {
    Vec3 clamped{
        std::clamp(offset.x, -10.0, 10.0),
        std::clamp(offset.y, -10.0, 10.0),
        std::clamp(offset.z, -10.0, 10.0)
    };
    c.renderSnap.rootOffset = clamped;
    if (c.drag.mode != DragMode::None) {
        c.drag.currentRootOffset = clamped;
    }
    {
        std::scoped_lock lock(c.commandMutex);
        c.pending.hasRootOffsetEdit = true;
        c.pending.rootOffsetEdit = clamped;
    }
    c.history.MarkDirty(NowMs());
}

void ApplySnapshotToTarget(Context& c, const PoseSnapshot& snap) {
    if (snap.boneOffsets.size() != c.renderSnap.bones.size()) return;
    for (size_t i = 0; i < snap.boneOffsets.size(); ++i) {
        c.renderSnap.bones[i].offsetEuler = snap.boneOffsets[i];
    }
    c.renderSnap.rootOffset = snap.rootOffset;
    std::scoped_lock lock(c.commandMutex);
    c.pending.hasBulkOffsets = true;
    c.pending.bulkOffsets = snap.boneOffsets;
    c.pending.hasRootOffsetEdit = true;
    c.pending.rootOffsetEdit = snap.rootOffset;
}

bool BoneVisibleInOverlay(const BoneRenderData& b, const Settings& s, int index, int selectedBone) noexcept {
    if (index == selectedBone) return true;
    if (!s.bodyOnly) return true;
    if (b.isBodyBone) return true;
    return s.showFaceBones != 0 && b.isFaceBone;
}

EulerAngles ComputeBoneOffsetFromWorldDelta(const RenderSnapshot& snap, int boneIdx,
                                            const Quat& startWorldRot, const EulerAngles& startOffset,
                                            const Quat& worldDelta, bool applyLimits) noexcept {
    const auto& b = snap.bones[static_cast<size_t>(boneIdx)];
    const Quat localStep = (startWorldRot.Inverse() * worldDelta * startWorldRot).Normalized();
    Quat newOffsetQ = (Quat::FromUnityEuler(startOffset) * localStep).Normalized();
    if (applyLimits && b.limit.kind != JointLimitKind::None) {
        newOffsetQ = ClampOffsetInBoneFrame(newOffsetQ, b.limit);
    }
    return newOffsetQ.ToUnityEuler();
}

void HandleViewportInteraction(Context& c, const Settings& s, const CabbirdInputSnapshotV1& in,
                               bool menuOwnsMouse, bool overUi, bool uiWantsKeyboard) {
    const bool leftDown = MouseDown(in, 0);
    const bool rightDown = MouseDown(in, 1);
    const bool middleDown = MouseDown(in, 2);
    const bool leftPressed = leftDown && !c.prevLeftDown;
    const bool rightPressed = rightDown && !c.prevRightDown;
    const bool middlePressed = middleDown && !c.prevMiddleDown;
    const bool leftReleased = !leftDown && c.prevLeftDown;
    const bool rightReleased = !rightDown && c.prevRightDown;
    const bool middleReleased = !middleDown && c.prevMiddleDown;

    float wheelNotches = 0.f;
    if (in.sequence != c.lastInputSeq && in.mouse_wheel != 0) {
        wheelNotches = static_cast<float>(in.mouse_wheel) / kWheelDeltaUnit;
    }
    c.lastInputSeq = in.sequence;

    const bool ctrlDown = (in.modifiers & CABBIRD_INPUT_MODIFIER_V1_CONTROL) != 0 ||
                          KeyDown(in, VK_CONTROL) || KeyDown(in, VK_LCONTROL) || KeyDown(in, VK_RCONTROL);
    const bool ctrlZ = ctrlDown && KeyDown(in, 'Z');
    const bool ctrlY = ctrlDown && KeyDown(in, 'Y');
    const bool spaceDown = KeyDown(in, VK_SPACE);
    if (menuOwnsMouse && !uiWantsKeyboard && c.drag.mode == DragMode::None) {
        if (spaceDown && !c.prevSpaceDown && (c.renderSnap.mmd.motionLoaded || c.renderSnap.mmd.cameraLoaded)) {
            std::scoped_lock lock(c.commandMutex);
            c.pending.mmdTogglePlay = true;
        }
        if (c.renderSnap.targetAlive) {
            if (ctrlZ && !c.prevCtrlZ) {
                PoseSnapshot restored;
                if (c.history.Undo(CaptureLiveSnapshot(c.renderSnap), restored)) {
                    ApplySnapshotToTarget(c, restored);
                }
            } else if (ctrlY && !c.prevCtrlY) {
                PoseSnapshot restored;
                if (c.history.Redo(restored)) {
                    ApplySnapshotToTarget(c, restored);
                }
            }
        }
    }
    c.prevCtrlZ = ctrlZ;
    c.prevCtrlY = ctrlY;
    c.prevSpaceDown = spaceDown;

    auto finishPoseDrag = [&]() {
        if (c.drag.dragMoved && c.drag.currentOffsets.size() == c.renderSnap.bones.size()) {
            c.history.CommitNow(PoseSnapshot{c.drag.currentOffsets, c.drag.currentRootOffset});
        }
        c.drag.mode = DragMode::None;
        c.drag.dragMoved = false;
    };

    // Scene picking and dragging are active ONLY while the overlay menu owns the cursor.
    if (!menuOwnsMouse) {
        if (c.drag.mode != DragMode::None) {
            if (c.drag.mode == DragMode::GizmoRing || c.drag.mode == DragMode::RootArrow ||
                c.drag.mode == DragMode::JointFk || c.drag.mode == DragMode::JointIk ||
                c.drag.mode == DragMode::JointTwist) {
                finishPoseDrag();
            } else {
                c.drag.mode = DragMode::None;
                c.drag.dragMoved = false;
            }
        }
        c.hoveredBone = -1;
        c.hoveredGizmoAxis = kGizmoAxisNone;
        c.hoveredRootAxis = kGizmoAxisNone;
        c.prevLeftDown = leftDown;
        c.prevRightDown = rightDown;
        c.prevMiddleDown = middleDown;
        return;
    }

    const Vec2 mouse{in.mouse_x, in.mouse_y};
    const auto& cam = c.renderSnap.camera;
    const bool canPickScene = c.renderSnap.targetAlive && cam.valid && s.showSkeleton != 0;

    auto cancelDrag = [&]() {
        if (c.drag.dragMoved &&
            (c.drag.mode == DragMode::GizmoRing || c.drag.mode == DragMode::RootArrow ||
             c.drag.mode == DragMode::JointFk || c.drag.mode == DragMode::JointIk ||
             c.drag.mode == DragMode::JointTwist)) {
            PoseSnapshot restore{c.drag.startOffsets, c.drag.startRootOffset};
            ApplySnapshotToTarget(c, restore);
        }
        c.drag.mode = DragMode::None;
        c.drag.dragMoved = false;
    };

    auto passedThreshold = [&](float thresholdPx, bool allowWheel) -> bool {
        if (c.drag.dragMoved) return true;
        const float dx = mouse.x - c.drag.startMouse.x;
        const float dy = mouse.y - c.drag.startMouse.y;
        if (dx * dx + dy * dy >= thresholdPx * thresholdPx || (allowWheel && wheelNotches != 0.f)) {
            c.drag.dragMoved = true;
            return true;
        }
        return false;
    };

    // An active scene drag continues uninterrupted even if the pointer sweeps over a UI panel.
    if (c.drag.mode != DragMode::None) {
        if (c.drag.mode == DragMode::OrbitRotate) {
            if (!rightDown || rightReleased) {
                c.drag.mode = DragMode::None;
            } else {
                const float dx = mouse.x - c.drag.lastMouse.x;
                const float dy = mouse.y - c.drag.lastMouse.y;
                if (dx != 0.f || dy != 0.f || wheelNotches != 0.f) {
                    std::scoped_lock lock(c.commandMutex);
                    c.pending.hasOrbitDelta = true;
                    c.pending.orbitDx += dx;
                    c.pending.orbitDy += dy;
                    c.pending.zoomNotches += wheelNotches;
                }
                c.drag.lastMouse = mouse;
            }
        } else if (c.drag.mode == DragMode::OrbitPan) {
            if (!middleDown || middleReleased) {
                c.drag.mode = DragMode::None;
            } else {
                const float dx = mouse.x - c.drag.lastMouse.x;
                const float dy = mouse.y - c.drag.lastMouse.y;
                if (dx != 0.f || dy != 0.f || wheelNotches != 0.f) {
                    std::scoped_lock lock(c.commandMutex);
                    c.pending.hasOrbitDelta = true;
                    c.pending.panDx += dx;
                    c.pending.panDy += dy;
                    c.pending.zoomNotches += wheelNotches;
                }
                c.drag.lastMouse = mouse;
            }
        } else if (c.drag.mode == DragMode::GizmoRing) {
            if (rightPressed) {
                cancelDrag();
            } else if (!leftDown || leftReleased) {
                finishPoseDrag();
            } else if (passedThreshold(kGizmoDragThresholdPx, false) &&
                       c.drag.boneIndex >= 0 && static_cast<size_t>(c.drag.boneIndex) < c.renderSnap.bones.size()) {
                const double angleRad = UpdateGizmoDragRadians(c.drag.gizmoDrag, mouse);
                const Vec3 localAxis = GizmoLocalAxis(c.drag.gizmoAxis);
                const Quat startQ = Quat::FromUnityEuler(c.drag.startOffsets[static_cast<size_t>(c.drag.boneIndex)]);
                Quat newQ = (startQ * Quat::FromAxisAngle(localAxis, angleRad)).Normalized();
                if (s.enableJointLimits != 0) {
                    newQ = ClampOffsetInBoneFrame(newQ, c.renderSnap.bones[static_cast<size_t>(c.drag.boneIndex)].limit);
                }
                QueueBoneOffsetEdit(c, c.drag.boneIndex, newQ.ToUnityEuler());
            }
        } else if (c.drag.mode == DragMode::RootArrow) {
            if (rightPressed) {
                cancelDrag();
            } else if (!leftDown || leftReleased) {
                finishPoseDrag();
            } else if (passedThreshold(kGizmoDragThresholdPx, false)) {
                const Vec2 totalDelta{mouse.x - c.drag.startMouse.x, mouse.y - c.drag.startMouse.y};
                const double meters = ComputeRootArrowDragMeters(totalDelta, c.drag.rootArrowPick);
                Vec3 next = c.drag.startRootOffset + GizmoLocalAxis(c.drag.gizmoAxis) * meters;
                QueueRootOffsetEdit(c, next);
            }
        } else if (c.drag.mode == DragMode::JointFk) {
            if (rightPressed) {
                cancelDrag();
            } else if (!leftDown || leftReleased) {
                finishPoseDrag();
            } else if (cam.valid && passedThreshold(kJointDragThresholdPx, true)) {
                if (wheelNotches != 0.f) {
                    c.drag.depthOffsetMeters += static_cast<double>(wheelNotches) * 0.05;
                }
                const Vec3 planePt = c.drag.dragPlanePoint + cam.Forward() * c.drag.depthOffsetMeters;
                Vec3 hit{};
                if (cam.CursorOnPlane(mouse.x, mouse.y, planePt, c.drag.dragPlaneNormal, hit)) {
                    if (c.drag.parentBoneIndex < 0) {
                        const Vec3 worldDisp = hit - c.drag.startBoneWorldPos;
                        const Vec3 localDisp = c.renderSnap.rootParentWorldRot.Inverse().Rotate(worldDisp);
                        QueueRootOffsetEdit(c, c.drag.startRootOffset + localDisp);
                    } else if (static_cast<size_t>(c.drag.parentBoneIndex) < c.renderSnap.bones.size()) {
                        const Vec3 fromDir = (c.drag.startBoneWorldPos - c.drag.startParentWorldPos).Normalized();
                        const Vec3 toDir = (hit - c.drag.startParentWorldPos).Normalized();
                        if (fromDir.LengthSq() > 0.5 && toDir.LengthSq() > 0.5) {
                            const Quat worldSwing = Quat::FromToRotation(fromDir, toDir);
                            const EulerAngles nextEuler = ComputeBoneOffsetFromWorldDelta(
                                c.renderSnap, c.drag.parentBoneIndex,
                                c.drag.startParentWorldRot,
                                c.drag.startOffsets[static_cast<size_t>(c.drag.parentBoneIndex)],
                                worldSwing, s.enableJointLimits != 0);
                            QueueBoneOffsetEdit(c, c.drag.parentBoneIndex, nextEuler);
                        }
                    }
                }
            }
        } else if (c.drag.mode == DragMode::JointIk) {
            if (rightPressed) {
                cancelDrag();
            } else if (!leftDown || leftReleased) {
                finishPoseDrag();
            } else if (cam.valid && c.drag.ikChain.IsValid() && passedThreshold(kJointDragThresholdPx, true)) {
                if (wheelNotches != 0.f) {
                    c.drag.depthOffsetMeters += static_cast<double>(wheelNotches) * 0.05;
                }
                const Vec3 planePt = c.drag.dragPlanePoint + cam.Forward() * c.drag.depthOffsetMeters;
                Vec3 targetPos{};
                if (cam.CursorOnPlane(mouse.x, mouse.y, planePt, c.drag.dragPlaneNormal, targetPos)) {
                    Vec3 polePos = c.drag.startUpperPos + c.drag.startPoleHint;
                    const bool isFoot = (c.drag.ikChain.kind == IkLimbKind::FootL ||
                                         c.drag.ikChain.kind == IkLimbKind::FootR);
                    if (isFoot && s.lockLegPlane != 0) {
                        polePos = c.drag.startUpperPos + c.renderSnap.rootForward;
                    }
                    Vec3 newMid{}, clampedEnd{};
                    if (SolveTwoBoneIk(c.drag.startUpperPos, c.drag.startMidPos, c.drag.startEndPos,
                                       targetPos, polePos, newMid, clampedEnd)) {
                        const Vec3 newUpperDir = (newMid - c.drag.startUpperPos).Normalized();
                        const Quat upperWorldDelta = Quat::FromToRotation(c.drag.startUpperToMidDir, newUpperDir);
                        const EulerAngles startUpperEuler = c.drag.startOffsets[static_cast<size_t>(c.drag.ikChain.upper)];
                        const EulerAngles upperEuler = ComputeBoneOffsetFromWorldDelta(
                            c.renderSnap, c.drag.ikChain.upper,
                            c.drag.startUpperWorldRot,
                            startUpperEuler,
                            upperWorldDelta, s.enableJointLimits != 0);

                        // Compute actual applied upper-bone world delta (respecting joint limits if clamped).
                        const Quat upperLocalStep = (Quat::FromUnityEuler(startUpperEuler).Inverse() *
                                                     Quat::FromUnityEuler(upperEuler)).Normalized();
                        const Quat appliedUpperWorld = (c.drag.startUpperWorldRot * upperLocalStep *
                                                        c.drag.startUpperWorldRot.Inverse()).Normalized();
                        const double upperLen = (c.drag.startMidPos - c.drag.startUpperPos).Length();
                        const Vec3 actualMid = c.drag.startUpperPos +
                                               appliedUpperWorld.Rotate(c.drag.startUpperToMidDir).Normalized() * upperLen;

                        const Vec3 carriedLowerDir = appliedUpperWorld.Rotate(c.drag.startMidToEndDir).Normalized();
                        const Vec3 desiredLowerDir = (clampedEnd - actualMid).Normalized();
                        const Quat lowerExtraWorld = Quat::FromToRotation(carriedLowerDir, desiredLowerDir);
                        const Quat lowerAfterUpperRot = (appliedUpperWorld * c.drag.startLowerWorldRot).Normalized();
                        const EulerAngles lowerEuler = ComputeBoneOffsetFromWorldDelta(
                            c.renderSnap, c.drag.ikChain.lower,
                            lowerAfterUpperRot,
                            c.drag.startOffsets[static_cast<size_t>(c.drag.ikChain.lower)],
                            lowerExtraWorld, s.enableJointLimits != 0);

                        QueueBoneOffsetEdit(c, c.drag.ikChain.upper, upperEuler);
                        QueueBoneOffsetEdit(c, c.drag.ikChain.lower, lowerEuler);
                    }
                }
            }
        } else if (c.drag.mode == DragMode::JointTwist) {
            if (leftPressed) {
                cancelDrag();
            } else if (!rightDown || rightReleased) {
                finishPoseDrag();
            } else if (passedThreshold(kJointDragThresholdPx, false) &&
                       c.drag.boneIndex >= 0 && static_cast<size_t>(c.drag.boneIndex) < c.renderSnap.bones.size()) {
                const double twistDeg = static_cast<double>(mouse.x - c.drag.startMouse.x) * 0.7;
                const auto& b = c.renderSnap.bones[static_cast<size_t>(c.drag.boneIndex)];
                const Vec3 twistAxis = b.limit.localBoneAxis.Normalized({0.0, 1.0, 0.0});
                const Quat startQ = Quat::FromUnityEuler(c.drag.startOffsets[static_cast<size_t>(c.drag.boneIndex)]);
                Quat newQ = (startQ * Quat::FromAxisAngle(twistAxis, twistDeg * kDegToRad)).Normalized();
                if (s.enableJointLimits != 0) {
                    newQ = ClampOffsetInBoneFrame(newQ, b.limit);
                }
                QueueBoneOffsetEdit(c, c.drag.boneIndex, newQ.ToUnityEuler());
            }
        }

        c.prevLeftDown = leftDown;
        c.prevRightDown = rightDown;
        c.prevMiddleDown = middleDown;
        return;
    }

    c.hoveredBone = -1;
    c.hoveredGizmoAxis = kGizmoAxisNone;
    c.hoveredRootAxis = kGizmoAxisNone;

    if (!overUi && canPickScene) {
        // 1. Pick visible joint dots FIRST so clicking/dragging ANY joint position (including the
        // currently selected joint center dot) is never hijacked by the 3 rotation rings or arrows.
        const float pickRad = std::max(kJointPickRadiusMinPx, s.jointRadius + 4.f);
        float nearestDistSq = pickRad * pickRad;
        bool anyBoneNear = false;
        std::vector<Vec2> cachedScreen(c.renderSnap.bones.size());
        std::vector<uint8_t> cachedValid(c.renderSnap.bones.size(), 0);
        for (size_t i = 0; i < c.renderSnap.bones.size(); ++i) {
            const auto& b = c.renderSnap.bones[i];
            if (!BoneVisibleInOverlay(b, s, static_cast<int>(i), c.selectedBone)) continue;
            if (!cam.WorldToScreen(b.worldPos, cachedScreen[i])) continue;
            cachedValid[i] = 1;
            const float dx = cachedScreen[i].x - mouse.x;
            const float dy = cachedScreen[i].y - mouse.y;
            const float dSq = dx * dx + dy * dy;
            if (dSq <= nearestDistSq) {
                nearestDistSq = dSq;
                anyBoneNear = true;
            }
        }
        if (anyBoneNear) {
            // Break ties among stacked bones within kJointStackTiePx by descendant count (then nearest).
            const float tieLimit = std::sqrt(nearestDistSq) + kJointStackTiePx;
            const float tieLimitSq = tieLimit * tieLimit;
            uint32_t bestWeight = 0;
            float bestDistSq = tieLimitSq + 1.f;
            for (size_t i = 0; i < c.renderSnap.bones.size(); ++i) {
                if (!cachedValid[i]) continue;
                const float dx = cachedScreen[i].x - mouse.x;
                const float dy = cachedScreen[i].y - mouse.y;
                const float dSq = dx * dx + dy * dy;
                if (dSq > tieLimitSq) continue;
                const uint32_t w = (i < c.boneDescendantCount.size()) ? c.boneDescendantCount[i] : 0U;
                if (c.hoveredBone < 0 || w > bestWeight || (w == bestWeight && dSq < bestDistSq)) {
                    c.hoveredBone = static_cast<int>(i);
                    bestWeight = w;
                    bestDistSq = dSq;
                }
            }
        }

        // 2. Only hit-test root move arrows and rotation rings when the cursor is NOT over a joint dot.
        RootArrowPick rootPick;
        GizmoPickResult gizmoPick;
        if (c.hoveredBone < 0 && s.showGizmo != 0 && c.selectedBone >= 0 &&
            static_cast<size_t>(c.selectedBone) < c.renderSnap.bones.size()) {
            const float centerExcl = std::max(kGizmoDefaultCenterExclusionPx, s.jointRadius + 10.f);
            const auto& sb = c.renderSnap.bones[static_cast<size_t>(c.selectedBone)];
            if (c.selectedBone == c.renderSnap.rootBoneIndex) {
                rootPick = PickRootMoveArrows(cam, sb.worldPos, c.renderSnap.rootParentWorldRot,
                                              mouse.x, mouse.y, centerExcl);
                c.hoveredRootAxis = rootPick.axis;
            }
            if (c.hoveredRootAxis == kGizmoAxisNone) {
                gizmoPick = PickRotationGizmo(cam, sb.worldPos, sb.worldRot,
                                              mouse.x, mouse.y, centerExcl);
                c.hoveredGizmoAxis = gizmoPick.axis;
            }
        }

        auto snapshotForDrag = [&]() {
            c.drag = {};
            c.drag.startMouse = mouse;
            c.drag.lastMouse = mouse;
            c.drag.startRootOffset = c.renderSnap.rootOffset;
            c.drag.currentRootOffset = c.renderSnap.rootOffset;
            c.drag.startOffsets.reserve(c.renderSnap.bones.size());
            for (const auto& b : c.renderSnap.bones) c.drag.startOffsets.push_back(b.offsetEuler);
            c.drag.currentOffsets = c.drag.startOffsets;
        };

        if (leftPressed) {
            if (c.hoveredBone >= 0) {
                c.selectedBone = c.hoveredBone;
                snapshotForDrag();
                c.drag.boneIndex = c.hoveredBone;
                const auto& b = c.renderSnap.bones[static_cast<size_t>(c.hoveredBone)];
                c.drag.dragPlanePoint = b.worldPos;
                c.drag.dragPlaneNormal = cam.Forward();
                c.drag.startBoneWorldPos = b.worldPos;
                c.drag.startBoneWorldRot = b.worldRot;

                if (s.enableLimbIk != 0 && b.ikChain.IsValid()) {
                    c.drag.mode = DragMode::JointIk;
                    c.drag.ikChain = b.ikChain;
                    const auto& up = c.renderSnap.bones[static_cast<size_t>(b.ikChain.upper)];
                    const auto& lo = c.renderSnap.bones[static_cast<size_t>(b.ikChain.lower)];
                    c.drag.startUpperPos = up.worldPos;
                    c.drag.startMidPos = lo.worldPos;
                    c.drag.startEndPos = b.worldPos;
                    c.drag.startUpperWorldRot = up.worldRot;
                    c.drag.startLowerWorldRot = lo.worldRot;
                    c.drag.startUpperToMidDir = (lo.worldPos - up.worldPos).Normalized();
                    c.drag.startMidToEndDir = (b.worldPos - lo.worldPos).Normalized();
                    const Vec3 chord = (b.worldPos - up.worldPos).Normalized();
                    const Vec3 relMid = lo.worldPos - up.worldPos;
                    Vec3 bend = relMid - chord * relMid.Dot(chord);
                    if (bend.LengthSq() > 1e-6) {
                        c.drag.startPoleHint = bend.Normalized();
                    } else {
                        const bool isFoot = (b.ikChain.kind == IkLimbKind::FootL || b.ikChain.kind == IkLimbKind::FootR);
                        c.drag.startPoleHint = isFoot ? c.renderSnap.rootForward : -c.renderSnap.rootForward;
                    }
                } else {
                    c.drag.mode = DragMode::JointFk;
                    if (c.hoveredBone == c.renderSnap.rootBoneIndex || b.parent < 0) {
                        c.drag.parentBoneIndex = -1;
                    } else {
                        int pivot = b.parent;
                        for (int hops = 0; hops < 4 && pivot >= 0 &&
                                           static_cast<size_t>(pivot) < c.renderSnap.bones.size(); ++hops) {
                            const auto& pb = c.renderSnap.bones[static_cast<size_t>(pivot)];
                            if ((b.worldPos - pb.worldPos).LengthSq() >= 1e-6) break;
                            pivot = pb.parent;
                        }
                        c.drag.parentBoneIndex = pivot;
                        if (pivot >= 0 && static_cast<size_t>(pivot) < c.renderSnap.bones.size()) {
                            const auto& pb = c.renderSnap.bones[static_cast<size_t>(pivot)];
                            c.drag.startParentWorldPos = pb.worldPos;
                            c.drag.startParentWorldRot = pb.worldRot;
                        }
                    }
                }
            } else if (c.hoveredRootAxis != kGizmoAxisNone) {
                snapshotForDrag();
                c.drag.mode = DragMode::RootArrow;
                c.drag.boneIndex = c.selectedBone;
                c.drag.gizmoAxis = rootPick.axis;
                c.drag.rootArrowPick = rootPick;
            } else if (c.hoveredGizmoAxis != kGizmoAxisNone) {
                snapshotForDrag();
                c.drag.mode = DragMode::GizmoRing;
                c.drag.boneIndex = c.selectedBone;
                c.drag.gizmoAxis = gizmoPick.axis;
                c.drag.gizmoDrag = BeginGizmoDrag(gizmoPick, mouse);
            }
        } else if (rightPressed && c.hoveredBone >= 0) {
            c.selectedBone = c.hoveredBone;
            snapshotForDrag();
            c.drag.mode = DragMode::JointTwist;
            c.drag.boneIndex = c.hoveredBone;
        }
    }

    // Orbit camera controls work everywhere outside UI panels, even when the 3D skeleton is hidden.
    if (!overUi && c.renderSnap.orbitActive && c.drag.mode == DragMode::None) {
        if (rightPressed && c.hoveredBone < 0) {
            c.drag = {};
            c.drag.mode = DragMode::OrbitRotate;
            c.drag.startMouse = mouse;
            c.drag.lastMouse = mouse;
        } else if (middlePressed) {
            c.drag = {};
            c.drag.mode = DragMode::OrbitPan;
            c.drag.startMouse = mouse;
            c.drag.lastMouse = mouse;
        }
        if (wheelNotches != 0.f) {
            std::scoped_lock lock(c.commandMutex);
            c.pending.hasOrbitDelta = true;
            c.pending.zoomNotches += wheelNotches;
        }
    }

    c.prevLeftDown = leftDown;
    c.prevRightDown = rightDown;
    c.prevMiddleDown = middleDown;
}

void DrawSkeletonAndGizmoOverlay(Context& c, const CabbirdUiServiceV1* ui, const Settings& s) {
    if (!HAS_UI_FIELD(ui, draw_screen_primitives) || !s.showSkeleton || !c.renderSnap.targetAlive ||
        !c.renderSnap.camera.valid) {
        return;
    }
    const auto& cam = c.renderSnap.camera;
    const auto& bones = c.renderSnap.bones;
    c.drawPrims.clear();
    c.drawPrims.reserve(bones.size() * 3 + 220);

    auto pushLine = [&](Vec2 a, Vec2 b, uint32_t col, float thick) {
        CabbirdUiScreenPrimitiveV1 p{sizeof(p)};
        p.kind = CABBIRD_UI_SCREEN_PRIMITIVE_V1_LINE;
        p.color_rgba = col;
        p.x = a.x; p.y = a.y;
        p.width = b.x; p.height = b.y;
        p.thickness = thick;
        c.drawPrims.push_back(p);
    };
    auto pushCircle = [&](Vec2 center, float radius, uint32_t col, bool filled, float thick = 1.5f) {
        CabbirdUiScreenPrimitiveV1 p{sizeof(p)};
        p.kind = filled ? CABBIRD_UI_SCREEN_PRIMITIVE_V1_FILLED_CIRCLE
                        : CABBIRD_UI_SCREEN_PRIMITIVE_V1_OUTLINE_CIRCLE;
        p.color_rgba = col;
        p.x = center.x; p.y = center.y;
        p.width = radius;
        p.thickness = thick;
        c.drawPrims.push_back(p);
    };

    std::vector<Vec2> screenPos(bones.size());
    std::vector<bool> projected(bones.size(), false);
    for (size_t i = 0; i < bones.size(); ++i) {
        if (BoneVisibleInOverlay(bones[i], s, static_cast<int>(i), c.selectedBone)) {
            projected[i] = cam.WorldToScreen(bones[i].worldPos, screenPos[i]);
        }
    }

    const uint32_t colLineShadow = CABBIRD_RGBA_V1(0, 0, 0, 130);
    const uint32_t colLineNormal = CABBIRD_RGBA_V1(210, 235, 255, 195);
    const uint32_t colLineEdited = CABBIRD_RGBA_V1(255, 215, 95, 225);
    const uint32_t colLineActive = CABBIRD_RGBA_V1(80, 240, 255, 245);

    // 1. Bone lines.
    for (size_t i = 0; i < bones.size(); ++i) {
        if (!projected[i]) continue;
        const int p = bones[i].parent;
        if (p < 0 || static_cast<size_t>(p) >= bones.size() || !projected[static_cast<size_t>(p)]) continue;
        const bool isActive = (static_cast<int>(i) == c.selectedBone || p == c.selectedBone);
        const bool isEdited = !bones[i].offsetEuler.IsNearZero() || !bones[static_cast<size_t>(p)].offsetEuler.IsNearZero();
        const uint32_t col = isActive ? colLineActive : (isEdited ? colLineEdited : colLineNormal);
        pushLine(screenPos[static_cast<size_t>(p)], screenPos[i], colLineShadow, isActive ? 4.2f : 3.2f);
        pushLine(screenPos[static_cast<size_t>(p)], screenPos[i], col, isActive ? 2.4f : 1.6f);
    }

    // 2. Rotation gizmo rings & root move arrows (drawn behind joint dots so dots remain crisp and clickable).
    if (s.showGizmo != 0 && c.selectedBone >= 0 && static_cast<size_t>(c.selectedBone) < bones.size()) {
        const auto& sb = bones[static_cast<size_t>(c.selectedBone)];
        const double ringRadius = ComputeGizmoWorldRadius(cam, sb.worldPos, kGizmoScreenRadiusPx);
        const uint32_t axisColorsFront[3]{
            CABBIRD_RGBA_V1(245, 78, 78, 240),
            CABBIRD_RGBA_V1(82, 230, 108, 240),
            CABBIRD_RGBA_V1(88, 162, 255, 240)
        };
        const uint32_t axisColorsBack[3]{
            CABBIRD_RGBA_V1(245, 78, 78, 65),
            CABBIRD_RGBA_V1(82, 230, 108, 65),
            CABBIRD_RGBA_V1(88, 162, 255, 65)
        };
        const uint32_t colHighlight = CABBIRD_RGBA_V1(255, 245, 130, 255);

        for (int axis = 0; axis < 3; ++axis) {
            const bool active = (c.drag.mode == DragMode::GizmoRing && c.drag.gizmoAxis == axis) ||
                                (c.drag.mode == DragMode::None && c.hoveredGizmoAxis == axis);
            BuildRotationRingSegments(cam, sb.worldPos, sb.worldRot, axis, ringRadius, c.gizmoSegs);
            for (const auto& seg : c.gizmoSegs) {
                const uint32_t col = (active && seg.frontFacing) ? colHighlight
                                   : (seg.frontFacing ? axisColorsFront[axis] : axisColorsBack[axis]);
                const float thick = (active && seg.frontFacing) ? 3.2f : (seg.frontFacing ? 2.0f : 1.0f);
                pushLine(seg.a, seg.b, col, thick);
            }
        }

        if (c.selectedBone == c.renderSnap.rootBoneIndex) {
            Vec2 o2{};
            if (cam.WorldToScreen(sb.worldPos, o2)) {
                const double arrowLen = ComputeGizmoWorldRadius(cam, sb.worldPos, kGizmoArrowScreenLenPx);
                const float innerGap = s.jointRadius + 5.f;
                for (int axis = 0; axis < 3; ++axis) {
                    const Vec3 dirWorld = c.renderSnap.rootParentWorldRot.Rotate(GizmoLocalAxis(axis)).Normalized();
                    Vec2 tip{};
                    if (!cam.WorldToScreen(sb.worldPos + dirWorld * arrowLen, tip)) continue;
                    const float ax = tip.x - o2.x;
                    const float ay = tip.y - o2.y;
                    const float segLen = std::hypot(ax, ay);
                    if (segLen < innerGap + 8.f) continue;
                    const float ux = ax / segLen;
                    const float uy = ay / segLen;
                    const Vec2 start{o2.x + ux * innerGap, o2.y + uy * innerGap};
                    const bool active = (c.drag.mode == DragMode::RootArrow && c.drag.gizmoAxis == axis) ||
                                        (c.drag.mode == DragMode::None && c.hoveredRootAxis == axis);
                    const uint32_t col = active ? colHighlight : axisColorsFront[axis];
                    const float thick = active ? 3.6f : 2.5f;
                    pushLine(start, tip, colLineShadow, thick + 1.8f);
                    pushLine(start, tip, col, thick);
                    // Arrowhead wings at tip.
                    const float headLen = active ? 10.f : 8.5f;
                    const float headWidth = active ? 5.5f : 4.5f;
                    const Vec2 leftWing{tip.x - ux * headLen - uy * headWidth, tip.y - uy * headLen + ux * headWidth};
                    const Vec2 rightWing{tip.x - ux * headLen + uy * headWidth, tip.y - uy * headLen - ux * headWidth};
                    pushLine(tip, leftWing, colLineShadow, thick + 1.4f);
                    pushLine(tip, rightWing, colLineShadow, thick + 1.4f);
                    pushLine(tip, leftWing, col, thick);
                    pushLine(tip, rightWing, col, thick);
                }
            }
        }
    }

    // 3. Joint dots rendered on top of lines and gizmo rings.
    const uint32_t colJointNormal   = CABBIRD_RGBA_V1(230, 245, 255, 215);
    const uint32_t colJointIk       = CABBIRD_RGBA_V1(120, 235, 175, 235);
    const uint32_t colJointEdited   = CABBIRD_RGBA_V1(255, 195, 70, 240);
    const uint32_t colJointHovered  = CABBIRD_RGBA_V1(255, 240, 120, 255);
    const uint32_t colJointSelected = CABBIRD_RGBA_V1(70, 235, 255, 255);
    const uint32_t colJointDragged  = CABBIRD_RGBA_V1(255, 110, 90, 255);
    const uint32_t colOutlineDark   = CABBIRD_RGBA_V1(12, 16, 22, 230);

    for (size_t i = 0; i < bones.size(); ++i) {
        if (!projected[i]) continue;
        const int idx = static_cast<int>(i);
        const bool isSelected = (idx == c.selectedBone);
        const bool isHovered = (idx == c.hoveredBone);
        const bool isDragged = (c.drag.mode != DragMode::None && idx == c.drag.boneIndex);
        const bool isEdited = !bones[i].offsetEuler.IsNearZero();
        const bool isIkEnd = (s.enableLimbIk != 0 && bones[i].ikChain.IsValid());

        float r = s.jointRadius;
        uint32_t fill = isIkEnd ? colJointIk : (isEdited ? colJointEdited : colJointNormal);
        if (isSelected) { r += 2.0f; fill = colJointSelected; }
        if (isHovered) { r += 2.5f; fill = colJointHovered; }
        if (isDragged) { r += 3.0f; fill = colJointDragged; }

        pushCircle(screenPos[i], r, fill, true);
        pushCircle(screenPos[i], r + 0.8f, colOutlineDark, false, 1.5f);
    }

    if (!c.drawPrims.empty()) {
        static_cast<void>(ui->draw_screen_primitives(ui->user, c.drawPrims.data(), c.drawPrims.size()));
    }

    const int tooltipBone = (c.drag.mode != DragMode::None && c.drag.boneIndex >= 0) ? c.drag.boneIndex : c.hoveredBone;
    if (s.showTooltip != 0 && tooltipBone >= 0 && static_cast<size_t>(tooltipBone) < bones.size() &&
        HAS_UI_FIELD(ui, overlay_text) && HAS_UI_FIELD(ui, overlay_measure_text) &&
        HAS_UI_FIELD(ui, overlay_rect_filled)) {
        Vec2 anchor{};
        if (cam.WorldToScreen(bones[static_cast<size_t>(tooltipBone)].worldPos, anchor)) {
            const std::string& label = bones[static_cast<size_t>(tooltipBone)].name;
            float tw = 0.f, th = 0.f;
            ui->overlay_measure_text(ui->user, StringView(label), 0.f, &tw, &th);
            const float padX = 7.f, padY = 4.f;
            const float bx = anchor.x + 14.f;
            const float by = anchor.y - th * 0.5f - padY;
            ui->overlay_rect_filled(ui->user, bx, by, tw + padX * 2.f, th + padY * 2.f,
                                    CABBIRD_RGBA_V1(16, 20, 28, 220), 4.f);
            ui->overlay_text(ui->user, bx + padX, by + padY, StringView(label),
                             CABBIRD_RGBA_V1(245, 250, 255, 255), 0.f);
        }
    }
}

std::string TargetKindLabel(const Context& c, TargetKind kind) {
    switch (kind) {
        case TargetKind::LocalPlayer: return c.text.Text("target.kind.local", "Player");
        case TargetKind::Player:      return c.text.Text("target.kind.player", "Avatar");
        case TargetKind::Npc:         return c.text.Text("target.kind.npc", "NPC");
        case TargetKind::Monster:     return c.text.Text("target.kind.monster", "Monster");
        case TargetKind::SceneRig:    return c.text.Text("target.kind.scene", "Scene");
        default:                      return c.text.Text("target.kind.entity", "Entity");
    }
}

void DrawTargetAndSettingsTab(Context& c, const CabbirdUiServiceV1* ui, Settings& s, bool& settingsChanged) {
    const auto u = ui->user;

    if (ui->button(u, StringView(c.text.Text("btn.refresh-targets", "Refresh Entities")), 0.f, 0.f)) {
        std::scoped_lock lock(c.commandMutex);
        c.pending.refreshTargets = true;
    }
    ui->same_line(u, 0.f, 6.f);
    if (ui->button(u, StringView(c.text.Text("btn.local-player", "Local Player")), 0.f, 0.f)) {
        std::scoped_lock lock(c.commandMutex);
        c.pending.selectLocalPlayer = true;
    }
    ui->same_line(u, 0.f, 6.f);
    if (ui->button(u, StringView(c.text.Text("btn.scan-scene", "Scan Scene")), 0.f, 0.f)) {
        std::scoped_lock lock(c.commandMutex);
        c.pending.scanSceneRigs = true;
    }

    if (c.renderSnap.targets.empty()) {
        UiTextWrapped(ui, c.text.Text("target.empty", "No selectable entities found"));
    } else {
        const size_t totalTargets = c.renderSnap.targets.size();
        const size_t targetPages = std::max<size_t>(1, (totalTargets + kTargetsPerPage - 1) / kTargetsPerPage);
        c.targetPage = std::min(c.targetPage, targetPages - 1);
        const size_t beginIdx = c.targetPage * kTargetsPerPage;
        const size_t endIdx = std::min(totalTargets, beginIdx + kTargetsPerPage);

        if (ui->begin_table(u, StringView("##bp_targets_tbl"), 4, CABBIRD_UI_TABLE_V1_SIZING_FIXED_FIT, 0.f, 0.f)) {
            ui->table_next_row(u);
            ui->table_next_column(u); ui->text(u, StringView(c.text.Text("target.col-action", "Action")));
            ui->table_next_column(u); ui->text(u, StringView(c.text.Text("target.col-name", "Entity")));
            ui->table_next_column(u); ui->text(u, StringView(c.text.Text("target.col-kind", "Type")));
            ui->table_next_column(u); ui->text(u, StringView(c.text.Text("target.col-bones", "Bones")));

            const std::string selectText = c.text.Text("btn.select", "Select");
            const std::string selectedText = c.text.Text("btn.selected", "Active");
            for (size_t i = beginIdx; i < endIdx; ++i) {
                const auto& t = c.renderSnap.targets[i];
                const bool active = (t.id == c.renderSnap.activeTargetId);
                ui->table_next_row(u);
                ui->table_next_column(u);
                const std::string btnLbl = (active ? selectedText : selectText) + "##bp_tgt_" + std::to_string(i);
                if (UiButtonSelectable(ui, btnLbl, 0.f, active)) {
                    std::scoped_lock lock(c.commandMutex);
                    c.pending.hasSelectTarget = true;
                    c.pending.selectTargetId = t.id;
                }
                ui->table_next_column(u);
                ui->text(u, StringView(t.rawName));
                ui->table_next_column(u);
                ui->text(u, StringView(TargetKindLabel(c, t.kind)));
                ui->table_next_column(u);
                ui->text(u, StringView(std::to_string(t.boneCount)));
            }
            ui->end_table(u);
        }

        if (targetPages > 1) {
            if (ui->button_enabled(u, StringView(c.text.Text("btn.prev-page", "Prev") + "##bp_tprev"), 0.f, 0.f,
                                   c.targetPage > 0 ? 1 : 0)) {
                --c.targetPage;
            }
            ui->same_line(u, 0.f, 6.f);
            if (ui->button_enabled(u, StringView(c.text.Text("btn.next-page", "Next") + "##bp_tnext"), 0.f, 0.f,
                                   c.targetPage + 1 < targetPages ? 1 : 0)) {
                ++c.targetPage;
            }
            ui->same_line(u, 0.f, 8.f);
            const std::string curPageStr = std::to_string(c.targetPage + 1);
            const std::string totPageStr = std::to_string(targetPages);
            const std::string totItemsStr = std::to_string(totalTargets);
            const std::string_view args[]{curPageStr, totPageStr, totItemsStr};
            ui->text(u, StringView(c.text.Format("page.summary", "Page {0}/{1} ({2})", args)));
        }
    }

    ui->separator(u);

    if (ui->checkbox(u, StringView(c.text.Text("anim.pause", "Pause Animation")), &s.pauseAnim)) {
        settingsChanged = true;
        std::scoped_lock lock(c.commandMutex);
        c.pending.hasPauseToggle = true;
        c.pending.wantPause = (s.pauseAnim != 0);
    }
    ui->same_line(u, 0.f, 12.f);
    int bindBase = c.renderSnap.useBindBase ? 1 : 0;
    if (ui->checkbox(u, StringView(c.text.Text("pose.bind-base", "Bind Pose Base (A/T)")), &bindBase)) {
        std::scoped_lock lock(c.commandMutex);
        c.pending.toggleBindBase = true;
        c.pending.wantBindBase = (bindBase != 0);
    }

    if (ui->button(u, StringView(c.text.Text("anim.speed-reset", "1.0x") + "##bp_spd1"), 48.f, 0.f)) {
        s.animSpeed = 1.f;
        settingsChanged = true;
        std::scoped_lock lock(c.commandMutex);
        c.pending.hasSpeedEdit = true;
        c.pending.wantSpeed = 1.f;
    }
    ui->same_line(u, 0.f, 6.f);
    if (ui->slider_float(u, StringView(c.text.Text("anim.speed", "Anim Speed")), &s.animSpeed, 0.f, 3.f)) {
        settingsChanged = true;
        std::scoped_lock lock(c.commandMutex);
        c.pending.hasSpeedEdit = true;
        c.pending.wantSpeed = s.animSpeed;
    }

    if (ui->button(u, StringView(c.text.Text("btn.refresh-anim", "Refresh Anim")), 0.f, 0.f)) {
        std::scoped_lock lock(c.commandMutex);
        c.pending.refreshAnim = true;
    }
    ui->same_line(u, 0.f, 8.f);
    if (ui->button_enabled(u, StringView(c.text.Text("btn.reset-all", "Reset All")), 0.f, 0.f,
                           c.renderSnap.targetAlive ? 1 : 0)) {
        c.showResetConfirm = true;
        ui->open_popup(u, StringView("##bp_reset_confirm"));
    }

    ui->separator(u);

    if (ui->checkbox(u, StringView(c.text.Text("overlay.show", "Show 3D Skeleton")), &s.showSkeleton)) {
        settingsChanged = true;
    }
    ui->same_line(u, 0.f, 12.f);
    if (ui->checkbox(u, StringView(c.text.Text("overlay.gizmo", "Rotation Gizmo")), &s.showGizmo)) {
        settingsChanged = true;
    }
    if (ui->checkbox(u, StringView(c.text.Text("overlay.body-only", "Body Bones Only")), &s.bodyOnly)) {
        settingsChanged = true;
    }
    if (s.bodyOnly) {
        ui->same_line(u, 0.f, 12.f);
        if (ui->checkbox(u, StringView(c.text.Text("overlay.face-bones", "Show Face Bones")), &s.showFaceBones)) {
            settingsChanged = true;
        }
    }
    if (ui->checkbox(u, StringView(c.text.Text("overlay.tooltip", "Hover Bone Tooltip")), &s.showTooltip)) {
        settingsChanged = true;
    }
    if (ui->checkbox(u, StringView(c.text.Text("edit.limb-ik", "2-Bone Limb IK (Hands/Feet)")), &s.enableLimbIk)) {
        settingsChanged = true;
    }
    if (s.enableLimbIk) {
        ui->same_line(u, 0.f, 12.f);
        if (ui->checkbox(u, StringView(c.text.Text("edit.lock-leg", "Lock Leg IK Plane")), &s.lockLegPlane)) {
            settingsChanged = true;
        }
    }
    if (ui->checkbox(u, StringView(c.text.Text("edit.joint-limits", "Anatomical Joint Limits")), &s.enableJointLimits)) {
        settingsChanged = true;
    }
    if (ui->slider_float(u, StringView(c.text.Text("overlay.joint-size", "Joint Radius (px)")),
                         &s.jointRadius, 3.f, 14.f)) {
        settingsChanged = true;
    }

    ui->separator(u);

    int orbitOn = c.renderSnap.orbitActive ? 1 : 0;
    if (ui->checkbox(u, StringView(c.text.Text("camera.orbit", "Pose Orbit Camera")), &orbitOn)) {
        std::scoped_lock lock(c.commandMutex);
        c.pending.hasOrbitToggle = true;
        c.pending.wantOrbitEnabled = (orbitOn != 0);
    }
    if (c.renderSnap.orbitActive) {
        float liveFov = c.renderSnap.orbit.initialized ? c.renderSnap.orbit.fovDeg : s.orbitFov;
        if (ui->slider_float(u, StringView(c.text.Text("camera.fov", "Camera FOV")), &liveFov, 15.f, 110.f)) {
            s.orbitFov = liveFov;
            settingsChanged = true;
            std::scoped_lock lock(c.commandMutex);
            c.pending.hasOrbitFov = true;
            c.pending.orbitFov = liveFov;
        }
        if (ui->button_enabled(u, StringView(c.text.Text("camera.focus-joint", "Focus Selected")), 0.f, 0.f,
                               c.selectedBone >= 0 ? 1 : 0)) {
            std::scoped_lock lock(c.commandMutex);
            c.pending.orbitFocusSelected = true;
            c.pending.orbitFocusBone = c.selectedBone;
        }
        ui->same_line(u, 0.f, 6.f);
        if (ui->button(u, StringView(c.text.Text("camera.reset", "Reset View")), 0.f, 0.f)) {
            std::scoped_lock lock(c.commandMutex);
            c.pending.orbitResetView = true;
        }
        UiTextWrapped(ui, c.text.Text("camera.hint",
            "Empty space: RMB orbit, MMB pan, Wheel zoom; Joints: LMB drag position (+Wheel depth), RMB twist"));
    }

    ui->separator(u);

    if (ui->input_text(u, StringView(c.text.Text("file.pose-path", "Pose File")),
                       c.posePathBuf, sizeof(c.posePathBuf), CABBIRD_UI_TEXT_INPUT_V1_NONE)) {
        if (c.posePathBuf[0] != '\0') {
            s.poseFilePath = c.posePathBuf;
            settingsChanged = true;
        }
    }
    const int hasSkel = c.renderSnap.targetAlive ? 1 : 0;
    if (ui->button_enabled(u, StringView(c.text.Text("file.export-pose", "Export Pose")), 0.f, 0.f, hasSkel)) {
        std::vector<std::string> names;
        std::vector<EulerAngles> offs;
        names.reserve(c.renderSnap.bones.size());
        offs.reserve(c.renderSnap.bones.size());
        for (const auto& b : c.renderSnap.bones) {
            names.push_back(b.name);
            offs.push_back(b.offsetEuler);
        }
        const auto doc = SerializePoseJson(c.renderSnap.activeTargetName, names, offs, c.renderSnap.rootOffset);
        QueueWorkerIo(c, {WorkerJobKind::ExportPose, c.posePathBuf, doc.dump(2), 0, {}});
    }
    ui->same_line(u, 0.f, 6.f);
    if (ui->button_enabled(u, StringView(c.text.Text("file.import-pose", "Import Pose")), 0.f, 0.f, hasSkel)) {
        QueueWorkerIo(c, {WorkerJobKind::ImportPose, c.posePathBuf, {}, 0, {}});
    }
    ui->same_line(u, 0.f, 6.f);
    if (ui->button_enabled(u, StringView(c.text.Text("file.export-skeleton", "Export Skeleton")), 0.f, 0.f, hasSkel)) {
        std::vector<SkeletonExportBone> out;
        out.reserve(c.renderSnap.bones.size());
        for (const auto& b : c.renderSnap.bones) {
            out.push_back({b.name, b.parent, {}, b.restLocalRot, b.worldPos});
        }
        const auto doc = SerializeSkeletonJson(c.renderSnap.activeTargetName, out);
        QueueWorkerIo(c, {WorkerJobKind::ExportSkeleton, c.skeletonPathBuf, doc.dump(2), 0, {}});
    }
}

void DrawMmdTab(Context& c, const CabbirdUiServiceV1* ui, Settings& s, bool& settingsChanged) {
    const auto u = ui->user;
    const auto& ms = c.renderSnap.mmd;
    bool mmdRuntimeChanged = false;

    auto buildRetargetOptions = [&]() -> mmd::RetargetOptions {
        mmd::RetargetOptions opts{};
        opts.fps = 30.0;
        opts.ikMode = static_cast<mmd::IkMode>(std::clamp(s.mmdIkMode, 0, 2));
        opts.hipOnPelvis = (s.mmdHipOnPelvis != 0);
        opts.feetAnchor = (s.mmdFeetAnchor != 0);
        opts.targetLegIk = (s.mmdTargetLegIk != 0);
        return opts;
    };

    if (ui->input_text(u, StringView(c.text.Text("mmd.file-path", "Motion File") + "##bp_mmd_path"),
                       c.mmdMotionPathBuf, sizeof(c.mmdMotionPathBuf), CABBIRD_UI_TEXT_INPUT_V1_NONE)) {
        s.mmdMotionPath = c.mmdMotionPathBuf;
        settingsChanged = true;
    }

    const int canBrowse = (!ms.fileDialogBusy && !ms.busyLoading) ? 1 : 0;
    const int canLoad = (c.renderSnap.targetAlive && !ms.busyLoading && c.mmdMotionPathBuf[0] != '\0') ? 1 : 0;
    if (ui->button_enabled(u, StringView(c.text.Text("mmd.browse", "Browse...") + "##bp_mmd_brw"), 0.f, 0.f,
                           (c.renderSnap.targetAlive && canBrowse) ? 1 : 0)) {
        QueueWorkerIo(c, {WorkerJobKind::BrowseMmdMotion, c.mmdMotionPathBuf, {}, s.mmdReferenceMode, buildRetargetOptions()});
    }
    ui->same_line(u, 0.f, 6.f);
    if (ui->button_enabled(u, StringView(c.text.Text("mmd.load", "Load VMD / VPD") + "##bp_mmd_load"), 0.f, 0.f, canLoad)) {
        QueueWorkerIo(c, {WorkerJobKind::LoadMmdMotion, c.mmdMotionPathBuf, {}, s.mmdReferenceMode, buildRetargetOptions()});
    }
    ui->same_line(u, 0.f, 6.f);
    if (ui->button_enabled(u, StringView(c.text.Text("mmd.unload", "Unload") + "##bp_mmd_unload"), 0.f, 0.f,
                           ms.motionLoaded ? 1 : 0)) {
        std::scoped_lock lock(c.commandMutex);
        c.pending.mmdUnload = true;
    }

    if (ms.busyLoading) {
        UiTextWrapped(ui, c.text.Text("mmd.loading", "Converting and loading motion..."));
    }

    const std::string refLabels[3]{
        c.text.Text("mmd.ref.pmx", "Standard Miku PMX"),
        c.text.Text("mmd.ref.unity", "Unity Miku V4C"),
        c.text.Text("mmd.ref.endfield", "Endfield Standard A")
    };

    if (ms.motionLoaded) {
        const std::string fStr = std::to_string(ms.frameCount);
        const std::string fpsStr = std::to_string(static_cast<int>(std::llround(ms.fps)));
        const std::string bStr = std::to_string(ms.mappedBoneCount);
        const std::string mStr = std::to_string(ms.matchedMorphCount);
        const std::string_view args[]{ms.motionName, fStr, fpsStr, bStr, mStr};
        UiTextWrapped(ui, c.text.Format("mmd.summary",
            "{0} | {1} frames ({2} FPS) | {3} bones, {4} morphs", args));

        const int recIdx = std::clamp(ms.recommendedReference, 0, 2);
        const std::string_view recArgs[]{refLabels[recIdx]};
        UiTextWrapped(ui, c.text.Format("mmd.recommend", "Recommended rig: {0}", recArgs));
    }

    ui->separator(u);

    const bool hasAnyPlayable = ms.motionLoaded || ms.cameraLoaded;
    const std::string playLabel = ms.playing
        ? c.text.Text("mmd.pause", "Pause")
        : c.text.Text("mmd.play", "Play");
    if (ui->button_enabled(u, StringView(playLabel + "##bp_mmd_play"), 0.f, 0.f, hasAnyPlayable ? 1 : 0)) {
        std::scoped_lock lock(c.commandMutex);
        c.pending.mmdTogglePlay = true;
    }
    ui->same_line(u, 0.f, 6.f);
    if (ui->button_enabled(u, StringView(c.text.Text("mmd.stop", "Stop") + "##bp_mmd_stop"), 0.f, 0.f, hasAnyPlayable ? 1 : 0)) {
        std::scoped_lock lock(c.commandMutex);
        c.pending.mmdStop = true;
    }
    ui->same_line(u, 0.f, 8.f);
    if (ui->checkbox(u, StringView(c.text.Text("mmd.loop", "Loop") + "##bp_mmd_loop"), &s.mmdLoop)) {
        settingsChanged = true;
        mmdRuntimeChanged = true;
    }
    ui->same_line(u, 0.f, 8.f);
    ui->text(u, StringView(c.text.Text("mmd.shortcut-space", "Space: Play / Pause")));

    if (hasAnyPlayable && ms.frameCount > 1) {
        float frameVal = static_cast<float>(ms.currentFrame);
        const float maxFrame = static_cast<float>(ms.frameCount - 1);
        if (ui->slider_float(u, StringView(c.text.Text("mmd.frame", "Frame") + "##bp_mmd_frm"),
                             &frameVal, 0.f, maxFrame)) {
            std::scoped_lock lock(c.commandMutex);
            c.pending.mmdHasSeek = true;
            c.pending.mmdSeekFrame = static_cast<double>(frameVal);
        }
    }

    if (ui->button(u, StringView(c.text.Text("anim.speed-reset", "1.0x") + "##bp_mmd_spd1"), 48.f, 0.f)) {
        s.mmdSpeed = 1.f;
        settingsChanged = true;
        mmdRuntimeChanged = true;
    }
    ui->same_line(u, 0.f, 6.f);
    if (ui->slider_float(u, StringView(c.text.Text("mmd.speed", "Speed") + "##bp_mmd_spd"),
                         &s.mmdSpeed, 0.1f, 2.5f)) {
        settingsChanged = true;
        mmdRuntimeChanged = true;
    }
    if (ui->slider_float(u, StringView(c.text.Text("mmd.amp-master", "Amplitude") + "##bp_mmd_amp"),
                         &s.mmdAmpMaster, 0.f, 2.f)) {
        settingsChanged = true;
        mmdRuntimeChanged = true;
    }

    if (ui->checkbox(u, StringView(c.text.Text("mmd.apply-root", "Root Motion") + "##bp_mmd_root"), &s.mmdApplyRoot)) {
        settingsChanged = true;
        mmdRuntimeChanged = true;
    }
    ui->same_line(u, 0.f, 10.f);
    if (ui->checkbox(u, StringView(c.text.Text("mmd.lock-planar", "Lock Planar") + "##bp_mmd_pln"), &s.mmdLockPlanar)) {
        settingsChanged = true;
        mmdRuntimeChanged = true;
    }
    if (ui->checkbox(u, StringView(c.text.Text("mmd.expressions", "Facial Morphs") + "##bp_mmd_expr"), &s.mmdPlayExpressions)) {
        settingsChanged = true;
        mmdRuntimeChanged = true;
    }
    ui->same_line(u, 0.f, 10.f);
    if (ui->checkbox(u, StringView(c.text.Text("mmd.target-leg-ik", "Target Leg IK") + "##bp_mmd_tlik"), &s.mmdTargetLegIk)) {
        settingsChanged = true;
    }
    if (ui->checkbox(u, StringView(c.text.Text("mmd.hip-pelvis", "Hip on Pelvis") + "##bp_mmd_hip"), &s.mmdHipOnPelvis)) {
        settingsChanged = true;
    }
    ui->same_line(u, 0.f, 10.f);
    if (ui->checkbox(u, StringView(c.text.Text("mmd.feet-anchor", "Feet Anchor") + "##bp_mmd_feet"), &s.mmdFeetAnchor)) {
        settingsChanged = true;
    }

    ui->separator(u);

    ui->text(u, StringView(c.text.Text("mmd.ref-label", "Reference Rig")));
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ui->same_line(u, 0.f, 4.f);
        const std::string lbl = refLabels[i] + "##bp_mmd_ref_" + std::to_string(i);
        if (UiButtonSelectable(ui, lbl, 0.f, s.mmdReferenceMode == i)) {
            s.mmdReferenceMode = i;
            settingsChanged = true;
        }
    }

    ui->text(u, StringView(c.text.Text("mmd.ik-label", "Source IK")));
    const std::string ikLabels[3]{
        c.text.Text("mmd.ik.follow", "Follow VMD"),
        c.text.Text("mmd.ik.on", "Force On"),
        c.text.Text("mmd.ik.off", "Force Off")
    };
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ui->same_line(u, 0.f, 4.f);
        const std::string lbl = ikLabels[i] + "##bp_mmd_ik_" + std::to_string(i);
        if (UiButtonSelectable(ui, lbl, 0.f, s.mmdIkMode == i)) {
            s.mmdIkMode = i;
            settingsChanged = true;
        }
    }

    ui->separator(u);

    ui->text(u, StringView(c.text.Text("mmd.cam-section", "MMD Camera Track")));
    if (ui->input_text(u, StringView(c.text.Text("mmd.cam-path", "Camera VMD") + "##bp_cam_path"),
                       c.mmdCameraPathBuf, sizeof(c.mmdCameraPathBuf), CABBIRD_UI_TEXT_INPUT_V1_NONE)) {
        s.mmdCameraPath = c.mmdCameraPathBuf;
        settingsChanged = true;
    }
    if (ui->button_enabled(u, StringView(c.text.Text("mmd.browse", "Browse...") + "##bp_cam_brw"), 0.f, 0.f, canBrowse)) {
        QueueWorkerIo(c, {WorkerJobKind::BrowseMmdCamera, c.mmdCameraPathBuf, {}, 0, {}});
    }
    ui->same_line(u, 0.f, 6.f);
    if (ui->button_enabled(u, StringView(c.text.Text("mmd.cam-load", "Load Camera") + "##bp_cam_load"), 0.f, 0.f,
                           c.mmdCameraPathBuf[0] != '\0' ? 1 : 0)) {
        QueueWorkerIo(c, {WorkerJobKind::LoadMmdCamera, c.mmdCameraPathBuf, {}, 0, {}});
    }
    ui->same_line(u, 0.f, 6.f);
    if (ui->button_enabled(u, StringView(c.text.Text("mmd.cam-clear", "Clear") + "##bp_cam_clr"), 0.f, 0.f,
                           ms.cameraLoaded ? 1 : 0)) {
        std::scoped_lock lock(c.commandMutex);
        c.pending.mmdCamClear = true;
    }

    if (ms.cameraLoaded) {
        const std::string kStr = std::to_string(ms.cameraKeyCount);
        const std::string_view cArgs[]{ms.cameraName, kStr};
        UiTextWrapped(ui, c.text.Format("mmd.cam-loaded-summary", "Camera: {0} ({1} keys)", cArgs));

        int camOn = ms.cameraEnabled ? 1 : 0;
        if (ui->checkbox(u, StringView(c.text.Text("mmd.cam-enable", "Enable MMD Camera") + "##bp_cam_en"), &camOn)) {
            std::scoped_lock lock(c.commandMutex);
            c.pending.mmdCamHasEnable = true;
            c.pending.mmdCamWantEnable = (camOn != 0);
        }
        if (ui->checkbox(u, StringView(c.text.Text("mmd.cam-follow", "Follow Character") + "##bp_cam_flw"), &s.mmdCamFollow)) {
            settingsChanged = true;
            mmdRuntimeChanged = true;
        }
        ui->same_line(u, 0.f, 8.f);
        if (ui->checkbox(u, StringView(c.text.Text("mmd.cam-autocut", "Auto Cut") + "##bp_cam_cut"), &s.mmdCamAutoCut)) {
            settingsChanged = true;
            mmdRuntimeChanged = true;
        }
        ui->same_line(u, 0.f, 8.f);
        if (ui->checkbox(u, StringView(c.text.Text("mmd.cam-mirror", "Mirror Camera") + "##bp_cam_mir"), &s.mmdCamMirror)) {
            settingsChanged = true;
            mmdRuntimeChanged = true;
        }
        if (ui->slider_float(u, StringView(c.text.Text("mmd.cam-dist", "Distance Scale") + "##bp_cam_dst"),
                             &s.mmdCamDistScale, 0.2f, 3.f)) {
            settingsChanged = true;
            mmdRuntimeChanged = true;
        }
        if (ui->slider_float(u, StringView(c.text.Text("mmd.cam-fov-offset", "FOV Offset") + "##bp_cam_fov"),
                             &s.mmdCamFovOffset, -45.f, 45.f)) {
            settingsChanged = true;
            mmdRuntimeChanged = true;
        }
        if (ui->slider_float(u, StringView(c.text.Text("mmd.cam-yaw-offset", "Yaw Offset") + "##bp_cam_yaw"),
                             &s.mmdCamYawOffset, -180.f, 180.f)) {
            settingsChanged = true;
            mmdRuntimeChanged = true;
        }
        if (ui->slider_float(u, StringView(c.text.Text("mmd.cam-offset-y", "Height Offset (m)") + "##bp_cam_y"),
                             &s.mmdCamOffsetY, -2.f, 2.f)) {
            settingsChanged = true;
            mmdRuntimeChanged = true;
        }
        if (ui->button(u, StringView(c.text.Text("mmd.cam-reset", "Reset Camera") + "##bp_cam_rst"), 0.f, 0.f)) {
            s.mmdCamFollow = 1;
            s.mmdCamAutoCut = 1;
            s.mmdCamMirror = 0;
            s.mmdCamDistScale = 1.f;
            s.mmdCamFovOffset = 0.f;
            s.mmdCamYawOffset = 0.f;
            s.mmdCamOffsetY = 0.f;
            settingsChanged = true;
            mmdRuntimeChanged = true;
            std::scoped_lock lock(c.commandMutex);
            c.pending.mmdCamReanchor = true;
        }
    }

    if (mmdRuntimeChanged) {
        std::scoped_lock lock(c.commandMutex);
        c.pending.mmdSyncSettings = true;
    }
}

void DrawJointPoseTab(Context& c, const CabbirdUiServiceV1* ui, const Settings& s) {
    const auto u = ui->user;

    if (!c.renderSnap.targetAlive) {
        UiTextWrapped(ui, c.text.Text("pose.no-target",
            "No active skeleton loaded. Select a target in the Skeleton tab."));
        return;
    }

    if (c.selectedBone >= 0 && static_cast<size_t>(c.selectedBone) < c.renderSnap.bones.size()) {
        auto& b = c.renderSnap.bones[static_cast<size_t>(c.selectedBone)];
        const std::string idxStr = std::to_string(c.selectedBone);
        const std::string_view args[]{b.name, idxStr};
        ui->text(u, StringView(c.text.Format("pose.selected-bone", "Bone: {0} (#{1})", args)));
        ui->same_line(u, 0.f, 10.f);
        if (ui->button(u, StringView(c.text.Text("pose.reset-bone", "Reset Bone")), 0.f, 0.f)) {
            QueueBoneOffsetEdit(c, c.selectedBone, {});
            c.history.CommitNow(CaptureLiveSnapshot(c.renderSnap));
        }

        EulerAngles e = b.offsetEuler;
        bool edited = false;
        const std::string zeroLbl = c.text.Text("btn.zero", "0");

        if (ui->button(u, StringView(zeroLbl + "##bp_rp"), 32.f, 0.f)) { e.pitch = 0.f; edited = true; }
        ui->same_line(u, 0.f, 6.f);
        if (ui->slider_float(u, StringView(c.text.Text("axis.pitch", "Pitch (X)") + "##bp_p"), &e.pitch, -180.f, 180.f)) {
            edited = true;
        }

        if (ui->button(u, StringView(zeroLbl + "##bp_ry"), 32.f, 0.f)) { e.yaw = 0.f; edited = true; }
        ui->same_line(u, 0.f, 6.f);
        if (ui->slider_float(u, StringView(c.text.Text("axis.yaw", "Yaw (Y)") + "##bp_y"), &e.yaw, -180.f, 180.f)) {
            edited = true;
        }

        if (ui->button(u, StringView(zeroLbl + "##bp_rr"), 32.f, 0.f)) { e.roll = 0.f; edited = true; }
        ui->same_line(u, 0.f, 6.f);
        if (ui->slider_float(u, StringView(c.text.Text("axis.roll", "Roll (Z)") + "##bp_r"), &e.roll, -180.f, 180.f)) {
            edited = true;
        }

        if (edited) {
            if (s.enableJointLimits != 0 && b.limit.kind != JointLimitKind::None) {
                e = ClampOffsetInBoneFrame(Quat::FromUnityEuler(e), b.limit).ToUnityEuler();
            }
            QueueBoneOffsetEdit(c, c.selectedBone, e);
        }
    } else {
        UiTextWrapped(ui, c.text.Text("pose.select-hint",
            "Click a joint on screen or select a bone below to edit."));
    }

    ui->separator(u);

    ui->text(u, StringView(c.text.Text("pose.root-offset", "Body Offset (m)")));
    ui->same_line(u, 0.f, 10.f);
    if (ui->button(u, StringView(c.text.Text("pose.reset-offset", "Reset Offset")), 0.f, 0.f)) {
        QueueRootOffsetEdit(c, {});
        c.history.CommitNow(CaptureLiveSnapshot(c.renderSnap));
    }
    float rx = static_cast<float>(c.renderSnap.rootOffset.x);
    float ry = static_cast<float>(c.renderSnap.rootOffset.y);
    float rz = static_cast<float>(c.renderSnap.rootOffset.z);
    bool rootEdited = false;
    const std::string zeroOffsetLbl = c.text.Text("btn.zero", "0");

    if (ui->button(u, StringView(zeroOffsetLbl + "##bp_rox"), 32.f, 0.f)) { rx = 0.f; rootEdited = true; }
    ui->same_line(u, 0.f, 6.f);
    if (ui->slider_float(u, StringView(c.text.Text("offset.x", "Offset X") + "##bp_ox"), &rx, -5.f, 5.f)) rootEdited = true;

    if (ui->button(u, StringView(zeroOffsetLbl + "##bp_roy"), 32.f, 0.f)) { ry = 0.f; rootEdited = true; }
    ui->same_line(u, 0.f, 6.f);
    if (ui->slider_float(u, StringView(c.text.Text("offset.y", "Offset Y") + "##bp_oy"), &ry, -5.f, 5.f)) rootEdited = true;

    if (ui->button(u, StringView(zeroOffsetLbl + "##bp_roz"), 32.f, 0.f)) { rz = 0.f; rootEdited = true; }
    ui->same_line(u, 0.f, 6.f);
    if (ui->slider_float(u, StringView(c.text.Text("offset.z", "Offset Z") + "##bp_oz"), &rz, -5.f, 5.f)) rootEdited = true;

    if (rootEdited) QueueRootOffsetEdit(c, {rx, ry, rz});

    ui->separator(u);

    auto triggerMirror = [&](MirrorMode mode) {
        c.history.CommitNow(CaptureLiveSnapshot(c.renderSnap));
        std::scoped_lock lock(c.commandMutex);
        c.pending.hasMirror = true;
        c.pending.mirrorMode = mode;
        c.history.MarkDirty(NowMs());
    };
    if (ui->button(u, StringView(c.text.Text("mirror.flip", "Flip L <-> R")), 0.f, 0.f)) {
        triggerMirror(MirrorMode::Flip);
    }
    ui->same_line(u, 0.f, 6.f);
    if (ui->button(u, StringView(c.text.Text("mirror.l2r", "Copy L -> R")), 0.f, 0.f)) {
        triggerMirror(MirrorMode::CopyLeftToRight);
    }
    ui->same_line(u, 0.f, 6.f);
    if (ui->button(u, StringView(c.text.Text("mirror.r2l", "Copy R -> L")), 0.f, 0.f)) {
        triggerMirror(MirrorMode::CopyRightToLeft);
    }

    ui->separator(u);

    auto filterBtn = [&](BoneCategoryFilter cat, std::string title, bool sameLine) {
        if (sameLine) ui->same_line(u, 0.f, 4.f);
        const bool sel = (c.categoryFilter == cat);
        const std::string lbl = std::move(title) + "##bp_cat_" + std::to_string(static_cast<int>(cat));
        if (UiButtonSelectable(ui, lbl, 0.f, sel)) {
            c.categoryFilter = cat;
            c.bonePage = 0;
        }
    };
    filterBtn(BoneCategoryFilter::All, c.text.Text("filter.all", "All"), false);
    filterBtn(BoneCategoryFilter::Spine, c.text.Text("filter.spine", "Spine"), true);
    filterBtn(BoneCategoryFilter::Head, c.text.Text("filter.head", "Head"), true);
    filterBtn(BoneCategoryFilter::Arms, c.text.Text("filter.arms", "Arms"), true);
    filterBtn(BoneCategoryFilter::Hands, c.text.Text("filter.hands", "Hands"), true);
    filterBtn(BoneCategoryFilter::Legs, c.text.Text("filter.legs", "Legs"), true);
    filterBtn(BoneCategoryFilter::Face, c.text.Text("filter.face", "Face"), true);

    if (ui->button(u, StringView(c.text.Text("btn.reload-bones", "Reload")), 0.f, 0.f)) {
        std::scoped_lock lock(c.commandMutex);
        c.pending.reloadBones = true;
    }
    ui->same_line(u, 0.f, 6.f);
    if (ui->input_text(u, StringView(c.text.Text("filter.search", "Filter") + "##bp_bsearch"),
                       c.boneSearchBuf, sizeof(c.boneSearchBuf), CABBIRD_UI_TEXT_INPUT_V1_NONE)) {
        c.bonePage = 0;
    }

    const std::string searchLower = ToLowerAscii(c.boneSearchBuf);
    std::vector<size_t> filtered;
    filtered.reserve(c.renderSnap.bones.size());
    std::optional<size_t> selectedFilteredPos;
    for (size_t i = 0; i < c.renderSnap.bones.size(); ++i) {
        const auto& b = c.renderSnap.bones[i];
        if (c.categoryFilter == BoneCategoryFilter::All && s.bodyOnly != 0 &&
            !b.isBodyBone && !(s.showFaceBones != 0 && b.isFaceBone) &&
            static_cast<int>(i) != c.selectedBone) {
            continue;
        }
        if (!MatchesBoneCategory(b.name, c.categoryFilter)) continue;
        if (!searchLower.empty() && !ContainsAscii(ToLowerAscii(b.name), searchLower)) continue;
        if (static_cast<int>(i) == c.selectedBone) {
            selectedFilteredPos = filtered.size();
        }
        filtered.push_back(i);
    }

    if (filtered.empty()) {
        UiTextWrapped(ui, c.text.Text("bone.list-empty", "No matching bones"));
        return;
    }

    const size_t totalPages = std::max<size_t>(1, (filtered.size() + kBonesPerPage - 1) / kBonesPerPage);
    c.bonePage = std::min(c.bonePage, totalPages - 1);

    if (ui->button_enabled(u, StringView(c.text.Text("btn.prev-page", "Prev") + "##bp_bprev"), 0.f, 0.f,
                           c.bonePage > 0 ? 1 : 0)) {
        --c.bonePage;
    }
    ui->same_line(u, 0.f, 4.f);
    if (ui->button_enabled(u, StringView(c.text.Text("btn.next-page", "Next") + "##bp_bnext"), 0.f, 0.f,
                           c.bonePage + 1 < totalPages ? 1 : 0)) {
        ++c.bonePage;
    }
    ui->same_line(u, 0.f, 4.f);
    if (ui->button_enabled(u, StringView(c.text.Text("btn.locate-selected", "Locate") + "##bp_bloc"), 0.f, 0.f,
                           selectedFilteredPos.has_value() ? 1 : 0)) {
        c.bonePage = *selectedFilteredPos / kBonesPerPage;
    }
    ui->same_line(u, 0.f, 8.f);
    {
        const std::string curStr = std::to_string(c.bonePage + 1);
        const std::string totStr = std::to_string(totalPages);
        const std::string cntStr = std::to_string(filtered.size());
        const std::string_view args[]{curStr, totStr, cntStr};
        ui->text(u, StringView(c.text.Format("page.summary", "Page {0}/{1} ({2})", args)));
    }

    const size_t startRow = c.bonePage * kBonesPerPage;
    const size_t endRow = std::min(filtered.size(), startRow + kBonesPerPage);
    if (ui->begin_table(u, StringView("##bp_bones_tbl"), 4, CABBIRD_UI_TABLE_V1_SIZING_FIXED_FIT, 0.f, 0.f)) {
        ui->table_next_row(u);
        ui->table_next_column(u); ui->text(u, StringView(c.text.Text("bone.col-action", "Action")));
        ui->table_next_column(u); ui->text(u, StringView(c.text.Text("bone.col-index", "#")));
        ui->table_next_column(u); ui->text(u, StringView(c.text.Text("bone.col-name", "Bone")));
        ui->table_next_column(u); ui->text(u, StringView(c.text.Text("bone.col-tag", "Tag")));

        const std::string selectText = c.text.Text("btn.select", "Select");
        const std::string selectedText = c.text.Text("btn.selected", "Active");
        const std::string ikText = c.text.Text("bone.tag.ik", "IK");
        const std::string editedText = c.text.Text("bone.tag.edited", "Edited");
        const std::string ikEditedText = c.text.Text("bone.tag.ik-edited", "IK / Edited");

        for (size_t row = startRow; row < endRow; ++row) {
            const size_t i = filtered[row];
            const auto& b = c.renderSnap.bones[i];
            const bool sel = (static_cast<int>(i) == c.selectedBone);
            const bool mod = !b.offsetEuler.IsNearZero();
            const bool isIk = (s.enableLimbIk != 0 && b.ikChain.IsValid());

            ui->table_next_row(u);
            ui->table_next_column(u);
            const std::string btnLbl = (sel ? selectedText : selectText) + "##bp_bone_" + std::to_string(i);
            if (UiButtonSelectable(ui, btnLbl, 0.f, sel)) {
                c.selectedBone = static_cast<int>(i);
            }
            ui->table_next_column(u);
            ui->text(u, StringView(std::to_string(i)));
            ui->table_next_column(u);
            ui->text(u, StringView(b.name));
            ui->table_next_column(u);
            if (isIk && mod) {
                ui->text(u, StringView(ikEditedText));
            } else if (isIk) {
                ui->text(u, StringView(ikText));
            } else if (mod) {
                ui->text(u, StringView(editedText));
            }
        }
        ui->end_table(u);
    }
}

// ---------- Plugin Lifecycle Callbacks ----------

void CABBIRD_CALL Update(void* raw, double) noexcept {
    auto& c = *static_cast<Context*>(raw);
    try {
        if (!c.running.load(std::memory_order_acquire)) return;
        if (c.il2cpp && c.il2cpp->with_runtime) {
            static_cast<void>(c.il2cpp->with_runtime(c.il2cpp->user, GameUpdateCallback, &c));
        }
    } catch (...) {
        LogOnceText(c, 1U << 2U, c.text.Text("log.update-failed", "BetterPose: Game-domain update failed."));
    }
}

void CABBIRD_CALL Draw(void* raw, const CabbirdUiServiceV1* frameUi) noexcept {
    auto& c = *static_cast<Context*>(raw);
    try {
        if (!c.running.load(std::memory_order_acquire)) return;
        const CabbirdUiServiceV1* ui = frameUi ? frameUi : c.ui;
        if (!ui) return;

        // Lock-free snapshot read from Game thread.
        if (const auto latest = c.published.load(std::memory_order_acquire)) {
            if (latest->revision != c.renderSnap.revision) {
                c.renderSnap = *latest;
            }
        }

        if (c.renderSnap.targetAlive) {
            if (c.historyTargetId != c.renderSnap.activeTargetId ||
                c.historyBoneCount != c.renderSnap.bones.size()) {
                c.historyTargetId = c.renderSnap.activeTargetId;
                c.historyBoneCount = c.renderSnap.bones.size();
                c.history.Reset(CaptureLiveSnapshot(c.renderSnap));
                const size_t boneCount = c.renderSnap.bones.size();
                c.boneDescendantCount.assign(boneCount, 0U);
                for (size_t i = 0; i < boneCount; ++i) {
                    int p = c.renderSnap.bones[i].parent;
                    for (size_t hop = 0; hop < boneCount && p >= 0 && static_cast<size_t>(p) < boneCount; ++hop) {
                        ++c.boneDescendantCount[static_cast<size_t>(p)];
                        p = c.renderSnap.bones[static_cast<size_t>(p)].parent;
                    }
                }
                if (c.selectedBone >= static_cast<int>(boneCount)) {
                    c.selectedBone = c.renderSnap.rootBoneIndex;
                }
            }
        } else {
            c.selectedBone = -1;
            c.hoveredBone = -1;
            c.boneDescendantCount.clear();
        }

        Settings s;
        {
            std::scoped_lock lock(c.settingsMutex);
            s = c.settings;
        }
        if (!c.pathBufsInitialized) {
            std::snprintf(c.posePathBuf, sizeof(c.posePathBuf), "%s", s.poseFilePath.c_str());
            std::snprintf(c.skeletonPathBuf, sizeof(c.skeletonPathBuf), "%s", s.skeletonFilePath.c_str());
            std::snprintf(c.mmdMotionPathBuf, sizeof(c.mmdMotionPathBuf), "%s", s.mmdMotionPath.c_str());
            std::snprintf(c.mmdCameraPathBuf, sizeof(c.mmdCameraPathBuf), "%s", s.mmdCameraPath.c_str());
            c.pathBufsInitialized = true;
        }
        {
            std::scoped_lock lock(c.mmdMutex);
            if (c.pickedMotionPath.has_value()) {
                std::snprintf(c.mmdMotionPathBuf, sizeof(c.mmdMotionPathBuf), "%s", c.pickedMotionPath->c_str());
                s.mmdMotionPath = *c.pickedMotionPath;
                c.pickedMotionPath.reset();
            }
            if (c.pickedCameraPath.has_value()) {
                std::snprintf(c.mmdCameraPathBuf, sizeof(c.mmdCameraPathBuf), "%s", c.pickedCameraPath->c_str());
                s.mmdCameraPath = *c.pickedCameraPath;
                c.pickedCameraPath.reset();
            }
        }

        if (c.renderSnap.targetAlive) {
            c.history.TickSettle(NowMs(), c.drag.mode != DragMode::None, CaptureLiveSnapshot(c.renderSnap));
        }

        // Align camera projection viewport to ImGui's overlay viewport pixels (1:1 with mouse & draw_screen_primitives).
        if (c.renderSnap.camera.valid && HAS_UI_FIELD(ui, overlay_viewport_size)) {
            float vpW = 0.f, vpH = 0.f;
            ui->overlay_viewport_size(ui->user, &vpW, &vpH);
            if (std::isfinite(vpW) && std::isfinite(vpH) && vpW > 1.f && vpH > 1.f) {
                c.renderSnap.camera.viewportW = vpW;
                c.renderSnap.camera.viewportH = vpH;
            }
        }

        CabbirdInputSnapshotV1 inputSnap{sizeof(inputSnap)};
        const bool hasInput = c.input && c.input->snapshot && Succeeded(c.input->snapshot(c.input->user, &inputSnap));
        const uint32_t uiFlags = ui->frame_state ? ui->frame_state(ui->user) : 0;
        // CABBIRD_INPUT_CAPTURE_V1_MOUSE means the overlay menu owns the cursor (game mouse is blocked).
        // CABBIRD_UI_FRAME_V1_WANT_CAPTURE_MOUSE means the pointer is over an ImGui window/widget.
        const bool menuOwnsMouse = hasInput && (inputSnap.capture_flags & CABBIRD_INPUT_CAPTURE_V1_MOUSE) != 0;
        const bool overUi = (uiFlags & CABBIRD_UI_FRAME_V1_WANT_CAPTURE_MOUSE) != 0;
        const bool uiWantsKeyboard = (uiFlags & (CABBIRD_UI_FRAME_V1_WANT_CAPTURE_KEYBOARD | CABBIRD_UI_FRAME_V1_WANT_TEXT_INPUT)) != 0 ||
                                     (hasInput && (inputSnap.capture_flags & (CABBIRD_INPUT_CAPTURE_V1_KEYBOARD | CABBIRD_INPUT_CAPTURE_V1_TEXT)) != 0);

        if (hasInput) {
            HandleViewportInteraction(c, s, inputSnap, menuOwnsMouse, overUi, uiWantsKeyboard);
        }

        DrawSkeletonAndGizmoOverlay(c, ui, s);

        CabbirdWindowStateV1 winState{sizeof(winState)};
        if (!Succeeded(c.windows->state(c.windows->user, c.window, &winState)) || !winState.open) {
            return;
        }
        std::int32_t visible = 0;
        if (!Succeeded(c.windows->begin(c.windows->user, c.window, 0, &visible))) return;
        struct WindowEndGuard {
            Context& c;
            ~WindowEndGuard() { static_cast<void>(c.windows->end(c.windows->user, c.window)); }
        } winGuard{c};
        if (!visible) return;

        const auto u = ui->user;

        if (!c.renderSnap.bound && !c.renderSnap.statusDetail.empty()) {
            const std::string detail = c.renderSnap.statusDetail.empty() ? "unavailable" : c.renderSnap.statusDetail;
            const std::string_view args[]{detail};
            UiTextWrapped(ui, c.text.Format("status.unavailable", "Unavailable: {0}", args));
        } else if (c.renderSnap.bound && c.renderSnap.targetAlive) {
            const std::string boneCountStr = std::to_string(c.renderSnap.bones.size());
            const std::string modeStr = c.renderSnap.postLateHookActive
                ? c.text.Text("status.hook-late", "Post-LateUpdate")
                : c.text.Text("status.hook-tick", "Game Tick");
            const std::string_view args[]{c.renderSnap.activeTargetName, boneCountStr, modeStr};
            UiTextWrapped(ui, c.text.Format("status.active", "Target: {0} | {1} bones | {2}", args));
        } else if (c.renderSnap.bound) {
            UiTextWrapped(ui, c.text.Text("status.no-target", "No target skeleton active."));
        }

        if (ui->button_enabled(u, StringView(c.text.Text("btn.undo", "Undo")), 0.f, 0.f,
                               c.history.CanUndo() ? 1 : 0)) {
            PoseSnapshot restored;
            if (c.history.Undo(CaptureLiveSnapshot(c.renderSnap), restored)) {
                ApplySnapshotToTarget(c, restored);
            }
        }
        ui->same_line(u, 0.f, 6.f);
        if (ui->button_enabled(u, StringView(c.text.Text("btn.redo", "Redo")), 0.f, 0.f,
                               c.history.CanRedo() ? 1 : 0)) {
            PoseSnapshot restored;
            if (c.history.Redo(restored)) {
                ApplySnapshotToTarget(c, restored);
            }
        }
        {
            std::string noticeCopy;
            {
                std::scoped_lock lock(c.noticeMutex);
                noticeCopy = c.noticeText;
            }
            if (!noticeCopy.empty()) {
                UiTextWrapped(ui, noticeCopy);
            }
        }

        ui->separator(u);

        bool settingsChanged = false;
        if (ui->begin_tab_bar && ui->begin_tab_bar(u, StringView("##bp_tabs"), CABBIRD_UI_TAB_BAR_V1_NONE)) {
            if (ui->begin_tab_item(u, StringView(c.text.Text("tab.skeleton", "Skeleton")), nullptr,
                                   CABBIRD_UI_TAB_ITEM_V1_NONE, 1)) {
                DrawTargetAndSettingsTab(c, ui, s, settingsChanged);
                ui->end_tab_item(u);
            }
            if (ui->begin_tab_item(u, StringView(c.text.Text("tab.mmd", "MMD Motion")), nullptr,
                                   CABBIRD_UI_TAB_ITEM_V1_NONE, 1)) {
                DrawMmdTab(c, ui, s, settingsChanged);
                ui->end_tab_item(u);
            }
            if (ui->begin_tab_item(u, StringView(c.text.Text("tab.pose", "Joint Pose")), nullptr,
                                   CABBIRD_UI_TAB_ITEM_V1_NONE, 1)) {
                DrawJointPoseTab(c, ui, s);
                ui->end_tab_item(u);
            }
            ui->end_tab_bar(u);
        }

        int modalOpen = c.showResetConfirm ? 1 : 0;
        if (ui->begin_popup_modal && ui->begin_popup_modal(u, StringView("##bp_reset_confirm"), &modalOpen, 0)) {
            UiTextWrapped(ui, c.text.Text("modal.reset-body",
                "Reset all joint rotations and body offset for the current character?"));
            if (ui->button(u, StringView(c.text.Text("modal.confirm", "Confirm")), 0.f, 0.f)) {
                c.history.CommitNow(CaptureLiveSnapshot(c.renderSnap));
                for (auto& b : c.renderSnap.bones) b.offsetEuler = {};
                c.renderSnap.rootOffset = {};
                {
                    std::scoped_lock lock(c.commandMutex);
                    c.pending.resetAll = true;
                }
                c.history.CommitNow(CaptureLiveSnapshot(c.renderSnap));
                c.showResetConfirm = false;
                ui->close_current_popup(u);
            }
            ui->same_line(u, 0.f, 8.f);
            if (ui->button(u, StringView(c.text.Text("modal.cancel", "Cancel")), 0.f, 0.f)) {
                c.showResetConfirm = false;
                ui->close_current_popup(u);
            }
            ui->end_popup(u);
        }

        if (settingsChanged) {
            {
                std::scoped_lock lock(c.settingsMutex);
                c.settings = s;
            }
            QueueSaveSettings(c);
        }
    } catch (...) {
        LogOnceText(c, 1U << 5U, c.text.Text("log.draw-failed", "BetterPose: UI draw callback failed."));
    }
}

std::string QueryDirectoryString(auto&& fn) {
    size_t size = 0;
    const auto st = fn(nullptr, &size);
    if ((Succeeded(st) || st.code == CABBIRD_STATUS_V1_BUFFER_TOO_SMALL) && size > 0 && size < 4096) {
        std::string dir(size, '\0');
        if (Succeeded(fn(dir.data(), &size))) {
            while (!dir.empty() && dir.back() == '\0') dir.pop_back();
            return dir;
        }
    }
    return {};
}

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** result) noexcept {
    if (!host || !result) return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    *result = nullptr;
    try {
        auto c = std::make_unique<Context>(host);
        c->core = Query<CabbirdCoreServiceV1>(host, CABBIRD_CORE_SERVICE_V1_ID);
        // Check required v1 core UI prefix (optional append-only fields are checked via HAS_UI_FIELD).
        const auto uiView = cabbird::sdk::Host(host).Query<CabbirdUiServiceV1>(CABBIRD_UI_SERVICE_V1_ID);
        c->ui = (uiView && uiView->struct_size >= offsetof(CabbirdUiServiceV1, end_tab_bar) + sizeof(uiView->end_tab_bar))
            ? uiView.get() : nullptr;
        c->windows = Query<CabbirdWindowServiceV1>(host, CABBIRD_WINDOW_SERVICE_V1_ID);
        c->input = Query<CabbirdInputServiceV1>(host, CABBIRD_INPUT_SERVICE_V1_ID);
        c->player = Query<CabbirdUnityPlayerServiceV1>(host, CABBIRD_UNITY_PLAYER_SERVICE_V1_ID);
        c->entities = Query<CabbirdUnityEntitiesServiceV1>(host, CABBIRD_UNITY_ENTITIES_SERVICE_V1_ID);
        c->il2cpp = Query<CabbirdIl2CppServiceV1>(host, CABBIRD_IL2CPP_SERVICE_V1_ID);
        c->metadata = Query<CabbirdUnityMetadataServiceV1>(host, CABBIRD_UNITY_METADATA_SERVICE_V1_ID);
        c->lifecycle = Query<CabbirdGameLifecycleServiceV1>(host, CABBIRD_GAME_LIFECYCLE_SERVICE_V1_ID);
        c->hook = Query<CabbirdHookServiceV1>(host, CABBIRD_HOOK_SERVICE_V1_ID);
        c->scheduler = Query<CabbirdSchedulerServiceV1>(host, CABBIRD_SCHEDULER_SERVICE_V1_ID);

        if (!c->core || !c->core->log || !c->core->read_memory || !c->core->module_base ||
            !c->ui || !c->ui->checkbox || !c->ui->slider_float || !c->ui->button ||
            !c->ui->button_enabled || !c->ui->text || !c->ui->input_text ||
            !c->ui->begin_table || !c->ui->table_next_row || !c->ui->table_next_column ||
            !c->ui->end_table || !c->ui->separator ||
            !c->ui->same_line || !c->ui->begin_tab_bar || !c->ui->begin_tab_item ||
            !c->ui->end_tab_item || !c->ui->end_tab_bar ||
            !c->windows || !c->windows->register_window || !c->windows->release_window ||
            !c->windows->state || !c->windows->begin || !c->windows->end ||
            !c->input || !c->input->snapshot ||
            !c->il2cpp || !c->il2cpp->with_runtime || !c->metadata || !c->metadata->resolve_method ||
            !c->metadata->resolve_instance_field ||
            !c->lifecycle || !c->lifecycle->register_stop_cleanup || !c->lifecycle->unregister_stop_cleanup) {
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        }

        if (c->core->plugin_directory) {
            c->pluginDir = QueryDirectoryString([&](char* buf, size_t* sz) {
                return c->core->plugin_directory(c->core->user, buf, sz);
            });
        }
        // Prefer writable plugin-state directory (never write user files into the read-only package/profile dir).
        const auto* pluginState = Query<CabbirdPluginStateServiceV1>(host, CABBIRD_PLUGIN_STATE_SERVICE_V1_ID);
        if (pluginState && pluginState->directory) {
            c->writableStateDir = QueryDirectoryString([&](char* buf, size_t* sz) {
                return pluginState->directory(pluginState->user, buf, sz);
            });
        }
        if (c->writableStateDir.empty()) {
            c->writableStateDir = c->pluginDir;
        }

        PublishStatusSnapshot(*c, {});

        const auto cfgStatus = c->config.Bind(host, kSettingsId, SettingsSchema());
        if (!Succeeded(cfgStatus)) return cfgStatus;
        try {
            if (const auto saved = c->config.Read()) {
                const Settings loaded = saved->get<Settings>();
                if (ValidSettings(loaded)) c->settings = loaded;
            }
        } catch (...) {
            LogOnceText(*c, 1U << 1U, c->text.Text("log.settings-invalid",
                        "BetterPose: saved settings are invalid; defaults retained."));
        }

        const auto cleanupStatus = c->lifecycle->register_stop_cleanup(
            c->lifecycle->user, StopCleanup, c.get(), &c->cleanup);
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
        const auto title = c.text.Text("window.title", "BetterPose");
        CabbirdWindowSpecV1 spec{sizeof(spec)};
        spec.id = StringView("better-pose.main");
        spec.title = StringView(title);
        spec.initial_width = 430.f;
        spec.initial_height = 610.f;
        spec.minimum_width = 340.f;
        spec.minimum_height = 360.f;
        spec.default_open = 1;
        const auto status = c.windows->register_window(c.windows->user, &spec, &c.window);
        if (Succeeded(status)) {
            c.stopping.store(false, std::memory_order_release);
            c.running.store(true, std::memory_order_release);
        }
        return status;
    } catch (...) {
        return Status(CABBIRD_STATUS_V1_FAILED);
    }
}

CabbirdStatusV1 CABBIRD_CALL Stop(void* raw, uint32_t) noexcept {
    auto& c = *static_cast<Context*>(raw);
    try {
        c.stopping.store(true, std::memory_order_release);
        c.running.store(false, std::memory_order_release);
        RemoveAfterLateHook(c);

        if (c.il2cpp && c.il2cpp->release_handles) {
            std::vector<CabbirdIl2CppGCHandleV1> handles;
            c.unity.CollectHandles(handles);
            auto take = [&](CabbirdIl2CppGCHandleV1& h) {
                if (h) { handles.push_back(h); h = nullptr; }
            };
            take(c.activeGoHandle);
            take(c.activePlayableAnimHandle);
            take(c.activeAnimatorHandle);
            take(c.disabledBrainHandle);
            take(c.modifiedCameraHandle);
            for (auto& b : c.bones) take(b.transformHandle);
            for (auto& s : c.activeSmrs) take(s.smrHandle);
            for (auto& t : c.targets) {
                take(t.gameObjectHandle);
                take(t.playableAnimHandle);
                take(t.animatorHandle);
            }
            if (!handles.empty()) {
                static_cast<void>(c.il2cpp->release_handles(c.il2cpp->user, handles.data(), handles.size()));
            }
        }

        const bool saved = c.config.Stop();
        bool released = true;
        if (c.window.id) {
            const auto st = c.windows->release_window(c.windows->user, c.window);
            released = released && (Succeeded(st) || st.code == CABBIRD_STATUS_V1_NOT_FOUND);
            c.window = {};
        }
        if (c.cleanup.id) {
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
        delete static_cast<Context*>(raw);
    } catch (...) {}
}

}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(CabbirdPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor) ||
        descriptor->api_major != CABBIRD_PLUGIN_API_V1_MAJOR) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    descriptor->id = StringView("cabbird.better-pose");
    descriptor->name = StringView("BetterPose");
    descriptor->author = StringView("Cabbird");
    descriptor->version = StringView("1.0.0");
    descriptor->on_load = Load;
    descriptor->on_start = Start;
    descriptor->on_stop = Stop;
    descriptor->on_unload = Unload;
    descriptor->on_update = Update;
    descriptor->on_draw = Draw;
    return Ok();
}

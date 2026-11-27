#pragma once

#include "pose_math.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace betterpose::mmd {

// ---- VMD / VPD Parsed Data Structures ---------------------------------------

struct VmdBezier {
    uint8_t x1{20};
    uint8_t y1{20};
    uint8_t x2{107};
    uint8_t y2{107};
};

struct VmdBoneKey {
    uint32_t frame{};
    Vec3 position{};
    Quat rotation{Quat::Identity()};
    VmdBezier interp[4]{};  // 0:X, 1:Y, 2:Z, 3:Rotation
};

struct VmdBoneTrack {
    std::string name;
    std::vector<VmdBoneKey> keys;
};

struct VmdMorphKey {
    uint32_t frame{};
    float weight{};
};

struct VmdMorphTrack {
    std::string name;
    std::vector<VmdMorphKey> keys;
};

struct VmdCameraKey {
    uint32_t frame{};
    float distance{};
    Vec3 position{};
    Vec3 rotation{};         // radians (rx, ry, rz)
    VmdBezier interp[6]{};   // 0:X, 1:Y, 2:Z, 3:Rot, 4:Dist, 5:FOV
    uint32_t viewAngleDeg{30};
    bool perspective{true};
};

struct VmdIkSwitch {
    uint32_t frame{};
    bool enabled{true};
};

struct VmdVisibilityKey {
    uint32_t frame{};
    bool visible{true};
};

struct VmdDocument {
    std::string modelName;
    uint32_t maxBoneFrame{};
    uint32_t maxMorphFrame{};
    uint32_t maxCameraFrame{};
    std::vector<VmdBoneTrack> boneTracks;
    std::vector<VmdMorphTrack> morphTracks;
    std::vector<VmdCameraKey> cameraKeys;
    std::vector<VmdVisibilityKey> visibilityKeys;
    std::vector<std::pair<std::string, std::vector<VmdIkSwitch>>> ikStates;
};

// ---- Reference MMD Skeleton (loaded from assets/reference-*.json) -----------

struct ReferenceIkLink {
    uint32_t bone{};
    bool hasLimit{};
    Vec3 minRad{};
    Vec3 maxRad{};
};

struct ReferenceIk {
    bool valid{};
    uint32_t target{};
    uint32_t loop{};
    double limitRadian{};
    std::vector<ReferenceIkLink> links;
};

struct ReferenceBone {
    std::string name;
    uint32_t index{};
    int32_t parent{-1};
    Vec3 position{};
    int32_t tailBone{-1};
    Vec3 tailOffset{};
    ReferenceIk ik;
};

struct ReferenceSkeleton {
    std::string modelName;
    std::vector<ReferenceBone> bones;
};

// ---- Compatibility Analysis -------------------------------------------------

enum class ReferencePreset : int {
    StandardPmx = 0,
    UnityMiku = 1,
    EndfieldStandard = 2
};

struct VmdCompatibilityReport {
    bool ok{};
    std::string error;
    int recommendedReference{0};
    int confidence{0};
    uint32_t boneTrackCount{};
    uint32_t boneKeyCount{};
    uint32_t morphTrackCount{};
    uint32_t cameraKeyCount{};
    uint32_t frameCount{};
    bool hasFootIk{};
    bool hasCompleteLegFk{};
    bool hasLegD{};
    bool hasShoulderCancel{};
    bool hasWaistCancel{};
    bool hasTwistTracks{};
    bool hasUpperBody1{};
    bool hasUpperBody3{};
};

// ---- Target Unity Skeleton Snapshot (for Worker-thread retargeting) ---------

struct TargetBoneInput {
    std::string name;
    int parent{-1};
    Vec3 localPos{};
    Quat restLocalRot{Quat::Identity()};
    bool hasBindPose{};
    Vec3 localScale{1.0, 1.0, 1.0};
};

struct TargetSkeletonInput {
    std::string targetName;
    int rootBoneIndex{-1};
    // Rotation from character GameObject space into rootBone's parent Transform space.
    Quat goToRootParentRot{Quat::Identity()};
    std::vector<TargetBoneInput> bones;
};

enum class IkMode : int {
    FollowMotion = 0,
    ForceOn = 1,
    ForceOff = 2
};

struct RetargetOptions {
    double fps{30.0};
    IkMode ikMode{IkMode::FollowMotion};
    bool hipOnPelvis{true};
    bool feetAnchor{false};
    bool targetLegIk{true};
};

// ---- Runtime Playback Motion (compact, lock-free-friendly) ------------------

enum class MotionPart : uint8_t {
    Other = 0,
    Root,
    Pelvis,
    Spine,
    Head,
    Arm,
    Hand,
    Leg
};

struct MappedBoneTrack {
    int targetBoneIndex{-1};
    std::string targetBoneName;
    Quat restLocalRot{Quat::Identity()};
    MotionPart part{MotionPart::Other};
    std::vector<Quat> frames;  // target local rotations
};

struct BoneOffsetTrack {
    int targetBoneIndex{-1};
    Vec3 restLocalPos{};
    std::vector<Vec3> frames;  // absolute target local positions
};

struct LoadedMotion {
    std::string sourcePath;
    std::string displayName;
    double fps{30.0};
    uint32_t frameCount{};
    double unitMetersPerMmdUnit{0.08};
    double mmdLegLength{};
    double targetLegLength{};
    int referenceMode{0};
    int rootBoneIndex{-1};
    Vec3 rootRestLocalPos{};
    // Rotation from character GameObject space into rootBone's parent Transform space.
    Quat charToRootParentRot{Quat::Identity()};
    // Root translation deltas in root-bone parent's local space (meters, relative to frame 0).
    std::vector<Vec3> rootTranslationsLocal;
    // Root translation deltas in character GameObject space (meters, relative to frame 0; for camera follow).
    std::vector<Vec3> rootTranslationsChar;
    std::vector<MappedBoneTrack> boneTracks;
    std::vector<BoneOffsetTrack> boneOffsetTracks;
    // Unmapped body / twist / corrective helper bone indices held at clean bindLocalRot during playback
    // so live game idle animations do not distort limbs while hair/cloth physics remain free.
    std::vector<int> rigidHoldBones;
    std::vector<VmdMorphTrack> morphTracks;
    std::vector<VmdCameraKey> embeddedCameraKeys;
    VmdCompatibilityReport report;
    uint32_t mappedBoneCount{};
    uint32_t matchedTargetRoles{};
    std::string missingSummary;
};

struct MotionAmplitude {
    float master{1.f};
    float spine{1.f};
    float arms{1.f};
    float legs{1.f};
    float head{1.f};
};

struct MotionTravel {
    bool applyRootMotion{true};
    bool lockPlanar{false};
    float planarScale{1.f};
    float verticalScale{1.f};
};

// ---- Public Functions -------------------------------------------------------

std::string CanonicalMmdBoneName(std::string_view name);

double SampleVmdCurve(const VmdBezier& curve, double t) noexcept;
bool SampleBoneTrack(const VmdBoneTrack& track, double frame, Quat& outRot, Vec3& outPos) noexcept;
float SampleMorphTrack(const VmdMorphTrack& track, double frame) noexcept;
bool SampleCameraTrackRaw(const std::vector<VmdCameraKey>& keys, double frame, bool autoCut,
                          Vec3& outPos, Vec3& outRot, double& outDistance, double& outFovDeg) noexcept;

bool ParseVmdBytes(const uint8_t* data, size_t size, VmdDocument& out, std::string& error);
bool ParseVmdFile(const std::filesystem::path& path, VmdDocument& out, std::string& error);
bool ParseVpdBytes(const uint8_t* data, size_t size, VmdDocument& out, std::string& error);
bool ParseVpdFile(const std::filesystem::path& path, VmdDocument& out, std::string& error);

bool LoadReferenceSkeletonFile(const std::filesystem::path& path, ReferenceSkeleton& out, std::string& error);
bool LoadReferenceSkeletonJson(std::string_view jsonText, ReferenceSkeleton& out, std::string& error);

VmdCompatibilityReport AnalyzeVmdCompatibility(const VmdDocument& vmd);

// Retargets a parsed VMD/VPD document against a Unity target skeleton and MMD reference skeleton.
bool BuildUnityMotion(const VmdDocument& vmd,
                      const ReferenceSkeleton& reference,
                      const TargetSkeletonInput& target,
                      const RetargetOptions& options,
                      LoadedMotion& out,
                      std::string& error);

// Frame sampling helpers for runtime playback (allocation-free in hot path).
Quat SlerpQuat(const Quat& a, const Quat& b, double t) noexcept;
Quat ApplyAmplitudeToLocalRot(const Quat& restLocal, const Quat& posedLocal, float scale) noexcept;
Quat SampleMappedBoneFrame(const MappedBoneTrack& track, double frame, const MotionAmplitude& amp) noexcept;
Vec3 SampleVec3Track(const std::vector<Vec3>& frames, double frame) noexcept;
Vec3 FilterRootTravel(const Vec3& rawDelta, const MotionTravel& travel,
                      const Quat& charToRootParentRot) noexcept;

// Reference asset filename for preset index (0..2).
const char* ReferencePresetFilename(int presetIndex) noexcept;

}  // namespace betterpose::mmd

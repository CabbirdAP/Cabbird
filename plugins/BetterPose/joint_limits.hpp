#pragma once

#include "pose_math.hpp"
#include "skeleton_filter.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace betterpose {

enum class JointLimitKind : int {
    None = 0,
    BallCone,
    Hinge
};

// Geometry-derived per-bone joint limit computed from the character's rest skeleton.
// Because axes are measured from actual bone geometry instead of hardcoding rig-specific
// local axis signs, clamping works cleanly across Maya, 3dsMax/Biped, Blender, and Unity rigs.
struct JointLimit {
    JointLimitKind kind{JointLimitKind::None};
    // Unit axis along the bone in the bone's rest local space (toward its main child).
    Vec3 localBoneAxis{0.0, 1.0, 0.0};
    // For Hinge joints (knee / elbow): unit hinge axis in rest local space where positive
    // rotation bends the limb in its anatomical flexion direction.
    Vec3 localHingeAxis{1.0, 0.0, 0.0};
    float minHingeDeg{-5.f};
    float maxHingeDeg{145.f};
    float maxSwingDeg{90.f};
    float minTwistDeg{-60.f};
    float maxTwistDeg{60.f};
};

// Decompose rotation `q` into swing * twist around `unitAxis`, where twist rotates purely
// around `unitAxis` and swing rotates around an axis orthogonal to `unitAxis`.
inline void DecomposeSwingTwist(const Quat& qIn, const Vec3& unitAxis,
                                Quat& outSwing, Quat& outTwist) noexcept {
    Quat q = qIn.Normalized();
    if (q.w < 0.0) {
        q = {-q.w, -q.x, -q.y, -q.z};
    }
    const Vec3 r{q.x, q.y, q.z};
    const Vec3 proj = unitAxis * r.Dot(unitAxis);
    outTwist = Quat{q.w, proj.x, proj.y, proj.z}.Normalized();
    outSwing = (q * outTwist.Conjugate()).Normalized();
    if (outSwing.w < 0.0) {
        outSwing = {-outSwing.w, -outSwing.x, -outSwing.y, -outSwing.z};
    }
}

inline double SignedAngleAroundAxis(const Quat& pureAxisRot, const Vec3& unitAxis) noexcept {
    Quat q = pureAxisRot.Normalized();
    if (q.w < 0.0) {
        q = {-q.w, -q.x, -q.y, -q.z};
    }
    const Vec3 v{q.x, q.y, q.z};
    const double sinHalf = v.Dot(unitAxis);
    const double cosHalf = q.w;
    return 2.0 * std::atan2(sinHalf, cosHalf);
}

inline Quat ClampSwingCone(const Quat& swingIn, double maxSwingRad) noexcept {
    Quat s = swingIn.Normalized();
    if (s.w < 0.0) s = {-s.w, -s.x, -s.y, -s.z};
    const Vec3 v{s.x, s.y, s.z};
    const double sinHalf = v.Length();
    if (!(sinHalf > 1e-8)) return Quat::Identity();
    const double angle = 2.0 * std::atan2(sinHalf, s.w);
    if (angle <= maxSwingRad) return s;
    const Vec3 axis = v / sinHalf;
    return Quat::FromAxisAngle(axis, maxSwingRad);
}

// Classify a bone and derive its rest-space anatomical limit from rest world positions/rotations.
inline JointLimit BuildJointLimit(int boneIndex,
                                  const std::vector<std::string>& names,
                                  const std::vector<int>& parents,
                                  const std::vector<Vec3>& restWorldPos,
                                  const std::vector<Quat>& restWorldRot,
                                  const Vec3& rootForward,
                                  const Vec3& rootRight) {
    JointLimit limit;
    if (boneIndex < 0 || static_cast<size_t>(boneIndex) >= names.size()) return limit;
    const size_t bi = static_cast<size_t>(boneIndex);
    const std::string low = ToLowerAscii(names[bi]);

    // Determine primary child direction in world space (average of non-helper children).
    Vec3 childWorldDir{0.0, 0.0, 0.0};
    int childCount = 0;
    for (size_t c = 0; c < parents.size(); ++c) {
        if (parents[c] != boneIndex) continue;
        if (IsTwistOrHelperBone(names[c])) continue;
        const Vec3 d = restWorldPos[c] - restWorldPos[bi];
        if (d.LengthSq() > 1e-8) {
            childWorldDir += d.Normalized();
            ++childCount;
        }
    }
    if (childCount > 0 && childWorldDir.LengthSq() > 1e-8) {
        childWorldDir = childWorldDir.Normalized();
    } else if (parents[bi] >= 0) {
        const Vec3 d = restWorldPos[bi] - restWorldPos[static_cast<size_t>(parents[bi])];
        childWorldDir = d.Normalized({0.0, 1.0, 0.0});
    } else {
        childWorldDir = {0.0, 1.0, 0.0};
    }

    const Quat invWorld = restWorldRot[bi].Inverse();
    limit.localBoneAxis = invWorld.Rotate(childWorldDir).Normalized({0.0, 1.0, 0.0});

    // Skip root / pelvis (user can freely orient root) and physics/twist bones.
    if (parents[bi] < 0 || ContainsAscii(low, "pelvis") || ContainsAscii(low, "hips") ||
        ContainsAscii(low, "root") || IsTwistOrHelperBone(low)) {
        return limit;
    }

    // 1. Knees / Calves / Lower legs -> hinge bending backward (-rootForward).
    const bool isKnee = ContainsAscii(low, "calf") || ContainsAscii(low, "knee") ||
                        ContainsAscii(low, "lowerleg") || ContainsAscii(low, "shin") ||
                        (ContainsAscii(low, "leg") && !ContainsAscii(low, "upper") && !ContainsAscii(low, "thigh"));
    if (isKnee) {
        // Bone points roughly downward (-Y). Rotating toward -rootForward uses axis childDir x (-rootForward).
        Vec3 worldHinge = childWorldDir.Cross(-rootForward).Normalized();
        if (worldHinge.LengthSq() < 0.5) worldHinge = rootRight;
        // Verify sign: rotating childWorldDir by +90 deg around worldHinge moves toward -rootForward.
        const Vec3 testBent = Quat::FromAxisAngle(worldHinge, kPi * 0.5).Rotate(childWorldDir);
        if (testBent.Dot(-rootForward) < 0.0) worldHinge = -worldHinge;

        limit.kind = JointLimitKind::Hinge;
        limit.localHingeAxis = invWorld.Rotate(worldHinge).Normalized({1.0, 0.0, 0.0});
        limit.minHingeDeg = -5.f;
        limit.maxHingeDeg = 145.f;
        limit.maxSwingDeg = 10.f;
        limit.minTwistDeg = -15.f;
        limit.maxTwistDeg = 15.f;
        return limit;
    }

    // 2. Elbows / Forearms / Lower arms -> hinge bending forward/inward (+rootForward).
    const bool isElbow = ContainsAscii(low, "forearm") || ContainsAscii(low, "elbow") ||
                         ContainsAscii(low, "lowerarm");
    if (isElbow) {
        Vec3 worldHinge = childWorldDir.Cross(rootForward).Normalized();
        if (worldHinge.LengthSq() < 0.5) worldHinge = {0.0, 1.0, 0.0};
        const Vec3 testBent = Quat::FromAxisAngle(worldHinge, kPi * 0.5).Rotate(childWorldDir);
        if (testBent.Dot(rootForward) < 0.0) worldHinge = -worldHinge;

        limit.kind = JointLimitKind::Hinge;
        limit.localHingeAxis = invWorld.Rotate(worldHinge).Normalized({0.0, 1.0, 0.0});
        limit.minHingeDeg = -5.f;
        limit.maxHingeDeg = 145.f;
        limit.maxSwingDeg = 12.f;
        limit.minTwistDeg = -75.f;
        limit.maxTwistDeg = 75.f;
        return limit;
    }

    // 3. Ball-and-socket / cone joints.
    limit.kind = JointLimitKind::BallCone;
    if (ContainsAscii(low, "clavicle") || ContainsAscii(low, "shoulder")) {
        limit.maxSwingDeg = 35.f;
        limit.minTwistDeg = -20.f;
        limit.maxTwistDeg = 20.f;
    } else if (ContainsAscii(low, "upperarm") || ContainsAscii(low, "arm")) {
        limit.maxSwingDeg = 135.f;
        limit.minTwistDeg = -90.f;
        limit.maxTwistDeg = 90.f;
    } else if (ContainsAscii(low, "thigh") || ContainsAscii(low, "upperleg")) {
        limit.maxSwingDeg = 115.f;
        limit.minTwistDeg = -50.f;
        limit.maxTwistDeg = 50.f;
    } else if (ContainsAscii(low, "spine") || ContainsAscii(low, "chest") || ContainsAscii(low, "waist")) {
        limit.maxSwingDeg = 45.f;
        limit.minTwistDeg = -40.f;
        limit.maxTwistDeg = 40.f;
    } else if (ContainsAscii(low, "neck")) {
        limit.maxSwingDeg = 45.f;
        limit.minTwistDeg = -50.f;
        limit.maxTwistDeg = 50.f;
    } else if (ContainsAscii(low, "head")) {
        limit.maxSwingDeg = 55.f;
        limit.minTwistDeg = -65.f;
        limit.maxTwistDeg = 65.f;
    } else if (ContainsAscii(low, "hand") || ContainsAscii(low, "wrist")) {
        limit.maxSwingDeg = 75.f;
        limit.minTwistDeg = -45.f;
        limit.maxTwistDeg = 45.f;
    } else if (ContainsAscii(low, "foot") || ContainsAscii(low, "ankle")) {
        limit.maxSwingDeg = 60.f;
        limit.minTwistDeg = -30.f;
        limit.maxTwistDeg = 30.f;
    } else if (ContainsAscii(low, "finger") || ContainsAscii(low, "thumb") ||
               ContainsAscii(low, "index") || ContainsAscii(low, "middle") ||
               ContainsAscii(low, "ring") || ContainsAscii(low, "pinky") ||
               ContainsAscii(low, "little") || ContainsAscii(low, "toe")) {
        limit.maxSwingDeg = 95.f;
        limit.minTwistDeg = -15.f;
        limit.maxTwistDeg = 15.f;
    } else {
        limit.kind = JointLimitKind::None;
    }
    return limit;
}

// Apply anatomical limit to a bone's local offset rotation (relative to rest local rotation).
// Note: offset is applied as `finalLocal = baseLocal * localOffset` OR `localOffset * baseLocal`.
// Because `localBoneAxis` and `localHingeAxis` are expressed in the bone's rest frame (after `baseLocal`),
// `offsetInBoneFrame` is the right-multiplied delta (`finalLocal = baseLocal * offsetInBoneFrame`).
// This function accepts `offsetInBoneFrame` and returns the clamped `offsetInBoneFrame`.
inline Quat ClampOffsetInBoneFrame(const Quat& offsetInBoneFrame, const JointLimit& limit) noexcept {
    if (limit.kind == JointLimitKind::None) return offsetInBoneFrame.Normalized();

    if (limit.kind == JointLimitKind::Hinge) {
        // Decompose into off-hinge swing and pure hinge twist around localHingeAxis.
        Quat offHinge, hingeRot;
        DecomposeSwingTwist(offsetInBoneFrame, limit.localHingeAxis, offHinge, hingeRot);
        const double hingeDeg = SignedAngleAroundAxis(hingeRot, limit.localHingeAxis) * kRadToDeg;
        const double clampedHingeDeg = std::clamp(hingeDeg,
            static_cast<double>(limit.minHingeDeg), static_cast<double>(limit.maxHingeDeg));
        const Quat clampedHinge = Quat::FromAxisAngle(limit.localHingeAxis, clampedHingeDeg * kDegToRad);
        const Quat clampedOff = ClampSwingCone(offHinge, static_cast<double>(limit.maxSwingDeg) * kDegToRad);
        return (clampedOff * clampedHinge).Normalized();
    }

    // BallCone: decompose into swing-twist around localBoneAxis.
    Quat swing, twist;
    DecomposeSwingTwist(offsetInBoneFrame, limit.localBoneAxis, swing, twist);
    const Quat clampedSwing = ClampSwingCone(swing, static_cast<double>(limit.maxSwingDeg) * kDegToRad);
    const double twistDeg = SignedAngleAroundAxis(twist, limit.localBoneAxis) * kRadToDeg;
    const double clampedTwistDeg = std::clamp(twistDeg,
        static_cast<double>(limit.minTwistDeg), static_cast<double>(limit.maxTwistDeg));
    const Quat clampedTwist = Quat::FromAxisAngle(limit.localBoneAxis, clampedTwistDeg * kDegToRad);
    return (clampedSwing * clampedTwist).Normalized();
}

}  // namespace betterpose

#pragma once

#include "mmd_retarget.hpp"
#include "pose_math.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace betterpose::mmd {

struct MmdCameraSettings {
    bool enabled{false};
    bool followCharacter{true};
    bool autoCut{true};       // true: adjacent-frame keys snap cleanly without sliding
    bool mirror{false};
    float distanceScale{1.f};
    float fovOffsetDeg{0.f};
    float yawOffsetDeg{0.f};
    float offsetX{0.f};       // meters in character space (+right)
    float offsetY{0.f};       // meters in character space (+up)
    float offsetZ{0.f};       // meters in character space (+forward)
};

struct MmdCameraAnchor {
    bool valid{};
    Vec3 feetWorldPos{};      // Character ground/feet anchor in Unity world space
    double charYawRad{};      // Character facing yaw around Unity +Y (radians)
    double unitMeters{0.08};  // Meters per MMD unit
};

struct MmdCameraPoseUnity {
    bool valid{};
    Vec3 worldPos{};
    Quat worldRot{Quat::Identity()};
    float verticalFovDeg{30.f};
};

// Camera basis of an MMD camera rotation triple (rx, ry, rz in radians).
// R = Ry(ry) * Rx(rx) * Rz(rz); columns are camera right (+X), up (+Y), forward (+Z).
inline void ComputeMmdCameraBasis(const Vec3& rotRad, Vec3& outRight, Vec3& outUp, Vec3& outForward) noexcept {
    const double cx = std::cos(rotRad.x), sx = std::sin(rotRad.x);
    const double cy = std::cos(rotRad.y), sy = std::sin(rotRad.y);
    const double cz = std::cos(rotRad.z), sz = std::sin(rotRad.z);
    outRight   = {cy * cz + sy * sx * sz,  cx * sz, -sy * cz + cy * sx * sz};
    outUp      = {-cy * sz + sy * sx * cz, cx * cz,  sy * sz + cy * sx * cz};
    outForward = {sy * cx,                -sx,       cy * cx};
}

// Evaluates an MMD VMD camera track at `frame` directly into Unity LH Y-up world coordinates.
// Both MMD (+X left, +Y up, -Z front) and Unity character space (-X left, +Y up, +Z front) are
// left-handed Y-up; rotating by 180 deg around +Y maps MMD -> Unity character space with det = +1,
// preserving camera position, pitch, yaw, roll, and vertical FOV exactly.
inline MmdCameraPoseUnity EvaluateMmdCameraUnity(const std::vector<VmdCameraKey>& keys,
                                                 double frame,
                                                 const MmdCameraAnchor& anchor,
                                                 const MmdCameraSettings& settings,
                                                 const Vec3& authoredRootCharDelta = {},
                                                 const Vec3& appliedRootCharDelta = {}) noexcept {
    MmdCameraPoseUnity out{};
    if (!anchor.valid || keys.empty()) return out;

    Vec3 filePos{};
    Vec3 fileRot{};
    double fileDist = 0.0;
    double fileFov = 30.0;
    if (!SampleCameraTrackRaw(keys, frame, settings.autoCut, filePos, fileRot, fileDist, fileFov)) {
        return out;
    }

    if (settings.mirror) {
        filePos.x = -filePos.x;
        fileRot.y = -fileRot.y;
        fileRot.z = -fileRot.z;
    }

    Vec3 mmdRight{}, mmdUp{}, mmdForward{};
    ComputeMmdCameraBasis(fileRot, mmdRight, mmdUp, mmdForward);

    const double distScale = std::clamp(static_cast<double>(settings.distanceScale), 0.05, 10.0);
    const Vec3 camMmd = filePos + mmdForward * (fileDist * distScale);
    const double unit = (anchor.unitMeters > 1e-5) ? anchor.unitMeters : 0.08;

    // MMD -> Unity character space: Ry(180 deg): (x, y, z) -> (-x, y, -z).
    auto mmdToChar = [](const Vec3& v) noexcept -> Vec3 { return {-v.x, v.y, -v.z}; };

    Vec3 camChar = mmdToChar(camMmd) * unit;
    camChar.x += static_cast<double>(settings.offsetX);
    camChar.y += static_cast<double>(settings.offsetY);
    camChar.z += static_cast<double>(settings.offsetZ);

    // Replace the authored root walk with the root displacement actually applied on the character,
    // so locking planar root motion keeps the camera framed tightly on the character.
    if (settings.followCharacter) {
        camChar += (appliedRootCharDelta - authoredRootCharDelta);
    }

    const Vec3 rightChar   = mmdToChar(mmdRight).Normalized({-1.0, 0.0, 0.0});
    const Vec3 upChar      = mmdToChar(mmdUp).Normalized({0.0, 1.0, 0.0});
    const Vec3 forwardChar = mmdToChar(mmdForward).Normalized({0.0, 0.0, -1.0});
    const Quat rotChar = Quat::FromBasis(rightChar, upChar, forwardChar);

    const double totalYaw = anchor.charYawRad + static_cast<double>(settings.yawOffsetDeg) * kDegToRad;
    const Quat anchorRot = Quat::FromAxisAngle({0.0, 1.0, 0.0}, totalYaw);

    out.worldPos = anchor.feetWorldPos + anchorRot.Rotate(camChar);
    out.worldRot = (anchorRot * rotChar).Normalized();
    out.verticalFovDeg = static_cast<float>(std::clamp(fileFov + static_cast<double>(settings.fovOffsetDeg), 5.0, 150.0));
    out.valid = out.worldPos.IsFinite() && out.worldRot.IsFinite() && std::isfinite(out.verticalFovDeg);
    return out;
}

}  // namespace betterpose::mmd

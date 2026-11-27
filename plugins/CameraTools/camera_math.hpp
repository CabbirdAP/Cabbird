#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cameratools {

constexpr double kMinExtraDistance = 0.0;
constexpr double kMaxExtraDistance = 100.0;
constexpr double kMinFov = 15.0;
constexpr double kMaxFov = 170.0;
constexpr float kMinSpeed = 1.0f;
constexpr float kMaxSpeed = 200.0f;
constexpr float kDefaultSpeed = 10.0f;
constexpr double kBoostMultiplier = 4.0;
constexpr double kMaxStepDeltaSeconds = 0.1;

struct Vec3 {
    double x{}, y{}, z{};

    Vec3 operator+(const Vec3& o) const noexcept { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const noexcept { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const noexcept { return {x * s, y * s, z * s}; }

    bool IsFinite() const noexcept {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }

    double LengthSq() const noexcept { return x * x + y * y + z * z; }
    double Length() const noexcept { return std::sqrt(LengthSq()); }
};

struct Quat {
    // Matches UnityEngine.Quaternion storage convention (x, y, z, w).
    double x{}, y{}, z{}, w{1.0};

    bool IsFinite() const noexcept {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(w);
    }

    Quat Normalized() const noexcept {
        const double len = std::sqrt(x * x + y * y + z * z + w * w);
        if (!std::isfinite(len) || len < 1e-12) return {0.0, 0.0, 0.0, 1.0};
        const double inv = 1.0 / len;
        return {x * inv, y * inv, z * inv, w * inv};
    }
};

inline Vec3 Cross(const Vec3& a, const Vec3& b) noexcept {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

inline Vec3 NormalizeOrZero(const Vec3& v) noexcept {
    const double len = v.Length();
    if (!std::isfinite(len) || len < 1e-9) return {};
    return v * (1.0 / len);
}

inline Vec3 Rotate(const Quat& qRaw, const Vec3& v) noexcept {
    const Quat q = qRaw.Normalized();
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = Cross(u, v) * 2.0;
    return v + t * q.w + Cross(u, t);
}

// Unity Left-Handed Y-up basis vectors from camera rotation.
inline Vec3 ForwardFromQuat(const Quat& q) noexcept {
    return NormalizeOrZero(Rotate(q, {0.0, 0.0, 1.0}));
}

// Horizontal right vector in Unity LH Y-up: cross(worldUp, forward), falling back
// to rotated local +X when looking straight up or down.
inline Vec3 HorizontalRightFromQuat(const Quat& q) noexcept {
    const Vec3 forward = ForwardFromQuat(q);
    const Vec3 flatRight = Cross({0.0, 1.0, 0.0}, forward);
    if (flatRight.LengthSq() >= 1e-6) return NormalizeOrZero(flatRight);
    return NormalizeOrZero(Rotate(q, {1.0, 0.0, 0.0}));
}

inline Vec3 ApplyExtraViewDistance(const Vec3& cameraPos, const Quat& cameraRot,
                                   double extraDistance) noexcept {
    if (!cameraPos.IsFinite() || !std::isfinite(extraDistance) || extraDistance <= 0.0) {
        return cameraPos;
    }
    const double clamped = std::clamp(extraDistance, kMinExtraDistance, kMaxExtraDistance);
    const Vec3 forward = ForwardFromQuat(cameraRot);
    return cameraPos - forward * clamped;
}

struct FreeCameraInput {
    bool forward{};
    bool backward{};
    bool right{};
    bool left{};
    bool up{};
    bool down{};
    bool boost{};
    bool appFocused{};
    bool uiCapturesKeyboard{};
};

inline Vec3 ComputeFreeCameraStep(const Quat& cameraRot, const FreeCameraInput& input,
                                  double speedMetersPerSec, double deltaSeconds) noexcept {
    if (!input.appFocused || input.uiCapturesKeyboard) return {};
    if (!std::isfinite(speedMetersPerSec) || speedMetersPerSec <= 0.0) return {};
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0) return {};

    const double dt = std::clamp(deltaSeconds, 0.0, kMaxStepDeltaSeconds);
    const double longitudinal = static_cast<double>(input.forward) - static_cast<double>(input.backward);
    const double lateral = static_cast<double>(input.right) - static_cast<double>(input.left);
    const double vertical = static_cast<double>(input.up) - static_cast<double>(input.down);
    if (longitudinal == 0.0 && lateral == 0.0 && vertical == 0.0) return {};

    const Vec3 fwd = ForwardFromQuat(cameraRot);
    const Vec3 rgt = HorizontalRightFromQuat(cameraRot);
    Vec3 dir{
        fwd.x * longitudinal + rgt.x * lateral,
        fwd.y * longitudinal + rgt.y * lateral + vertical,
        fwd.z * longitudinal + rgt.z * lateral
    };
    const double mag = dir.Length();
    if (!std::isfinite(mag) || mag < 1e-9) return {};
    if (mag > 1.0) dir = dir * (1.0 / mag);

    const double speed = std::clamp(speedMetersPerSec, static_cast<double>(kMinSpeed),
                                    static_cast<double>(kMaxSpeed)) *
                         (input.boost ? kBoostMultiplier : 1.0);
    return dir * (speed * dt);
}

// 0.0 means "game default (no override)". Positive values clamp to [15, 170].
inline double NormalizeFovSetting(double value) noexcept {
    if (!std::isfinite(value) || value <= 0.0) return 0.0;
    return std::clamp(value, kMinFov, kMaxFov);
}

inline double NormalizeDistanceSetting(double value) noexcept {
    if (!std::isfinite(value) || value <= 0.0) return 0.0;
    return std::clamp(value, kMinExtraDistance, kMaxExtraDistance);
}

inline bool IsValidFovSetting(double value) noexcept {
    return std::isfinite(value) && (value == 0.0 || (value >= kMinFov && value <= kMaxFov));
}

inline bool IsValidDistanceSetting(double value) noexcept {
    return std::isfinite(value) && value >= kMinExtraDistance && value <= kMaxExtraDistance;
}

inline bool ShouldUpdateAoi(const Vec3& lastAoiPos, bool hasLastAoiPos,
                            const Vec3& currentPos, double elapsedSeconds,
                            double minIntervalSeconds = 0.25,
                            double minMoveMeters = 2.0) noexcept {
    if (!currentPos.IsFinite()) return false;
    if (!hasLastAoiPos) return true;
    if (elapsedSeconds < minIntervalSeconds) return false;
    const Vec3 diff = currentPos - lastAoiPos;
    return diff.LengthSq() >= minMoveMeters * minMoveMeters;
}

}  // namespace cameratools

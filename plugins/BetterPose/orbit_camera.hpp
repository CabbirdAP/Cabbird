#pragma once

#include "pose_math.hpp"

#include <algorithm>
#include <cmath>

namespace betterpose {

// Unity Left-Handed Y-Up mouse-only orbit camera.
// Right-drag orbits around `pivot`, middle-drag pans `pivot` in screen plane, wheel zooms distance.
struct OrbitCameraState {
    Vec3 pivot{0.0, 1.0, 0.0};
    double yawDeg{0.0};
    double pitchDeg{10.0};
    double distance{2.5};
    float fovDeg{45.f};
    bool initialized{false};

    static constexpr double kMinDistance = 0.20;
    static constexpr double kMaxDistance = 30.0;
    static constexpr double kMinPitchDeg = -85.0;
    static constexpr double kMaxPitchDeg = 85.0;
    static constexpr float kMinFovDeg = 15.f;
    static constexpr float kMaxFovDeg = 110.f;

    // Camera orientation looking from camera position toward pivot along +Z.
    Quat Rotation() const noexcept {
        return Quat::FromUnityEuler(static_cast<float>(pitchDeg), static_cast<float>(yawDeg), 0.f);
    }

    Vec3 Forward() const noexcept { return Rotation().Rotate({0.0, 0.0, 1.0}).Normalized({0.0, 0.0, 1.0}); }
    Vec3 Right() const noexcept { return Rotation().Rotate({1.0, 0.0, 0.0}).Normalized({1.0, 0.0, 0.0}); }
    Vec3 Up() const noexcept { return Rotation().Rotate({0.0, 1.0, 0.0}).Normalized({0.0, 1.0, 0.0}); }

    Vec3 Position() const noexcept {
        return pivot - Forward() * distance;
    }

    void InitFromCamera(const Vec3& camPos, const Vec3& targetCenter, float currentFovDeg) noexcept {
        pivot = targetCenter;
        const Vec3 toTarget = targetCenter - camPos;
        const double dist = toTarget.Length();
        if (dist > 1e-4) {
            distance = std::clamp(dist, kMinDistance, kMaxDistance);
            const Vec3 fwd = toTarget / dist;
            // Forward = Qy(yaw) * Qx(pitch) * (0, 0, 1) = (sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch)).
            pitchDeg = std::clamp(std::asin(std::clamp(-fwd.y, -1.0, 1.0)) * kRadToDeg,
                                  kMinPitchDeg, kMaxPitchDeg);
            yawDeg = std::atan2(fwd.x, fwd.z) * kRadToDeg;
        } else {
            distance = 2.5;
            yawDeg = 0.0;
            pitchDeg = 10.0;
        }
        fovDeg = std::clamp(std::isfinite(currentFovDeg) ? currentFovDeg : 45.f, kMinFovDeg, kMaxFovDeg);
        initialized = true;
    }

    void Orbit(float deltaX, float deltaY, double sensitivityDegPerPx = 0.32) noexcept {
        yawDeg = WrapAngle180(static_cast<float>(yawDeg + static_cast<double>(deltaX) * sensitivityDegPerPx));
        pitchDeg = std::clamp(pitchDeg + static_cast<double>(deltaY) * sensitivityDegPerPx,
                              kMinPitchDeg, kMaxPitchDeg);
    }

    void Pan(float deltaX, float deltaY) noexcept {
        const double scale = distance * 0.0018;
        pivot -= Right() * (static_cast<double>(deltaX) * scale);
        pivot += Up() * (static_cast<double>(deltaY) * scale);
    }

    void Zoom(float wheelNotches) noexcept {
        if (!std::isfinite(wheelNotches) || wheelNotches == 0.f) return;
        distance = std::clamp(distance * std::pow(0.88, static_cast<double>(wheelNotches)),
                              kMinDistance, kMaxDistance);
    }

    void FocusOn(const Vec3& point, double newDistance = -1.0) noexcept {
        pivot = point;
        if (newDistance > 0.0) {
            distance = std::clamp(newDistance, kMinDistance, kMaxDistance);
        }
    }
};

}  // namespace betterpose

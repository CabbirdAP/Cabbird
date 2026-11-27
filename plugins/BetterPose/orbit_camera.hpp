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

    // Start from the live camera pose without jumping the view: keeps the camera's exact
    // position, viewing direction, and FOV, placing `pivot` at `orbitDistance` directly ahead.
    void InitFromView(const Vec3& camPos, const Quat& camRot, double orbitDistance,
                      float currentFovDeg) noexcept {
        const Vec3 fwd = camRot.Rotate({0.0, 0.0, 1.0}).Normalized({0.0, 0.0, 1.0});
        // Forward = Qy(yaw) * Qx(pitch) * (0, 0, 1) = (sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch)).
        pitchDeg = std::clamp(std::asin(std::clamp(-fwd.y, -1.0, 1.0)) * kRadToDeg,
                              kMinPitchDeg, kMaxPitchDeg);
        yawDeg = std::atan2(fwd.x, fwd.z) * kRadToDeg;
        distance = std::clamp(std::isfinite(orbitDistance) ? orbitDistance : 2.5,
                              kMinDistance, kMaxDistance);
        pivot = camPos + Forward() * distance;
        fovDeg = std::clamp(std::isfinite(currentFovDeg) ? currentFovDeg : 45.f, kMinFovDeg, kMaxFovDeg);
        initialized = true;
    }

    // Aim directly from `camPos` toward `targetCenter`.
    void InitFromCamera(const Vec3& camPos, const Vec3& targetCenter, float currentFovDeg) noexcept {
        pivot = targetCenter;
        const Vec3 toTarget = targetCenter - camPos;
        const double dist = toTarget.Length();
        if (dist > 1e-4) {
            distance = std::clamp(dist, kMinDistance, kMaxDistance);
            const Vec3 fwd = toTarget / dist;
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

    void Orbit(float deltaX, float deltaY, double sensitivityDegPerPx = 0.30) noexcept {
        yawDeg = WrapAngle180(static_cast<float>(yawDeg + static_cast<double>(deltaX) * sensitivityDegPerPx));
        pitchDeg = std::clamp(pitchDeg + static_cast<double>(deltaY) * sensitivityDegPerPx,
                              kMinPitchDeg, kMaxPitchDeg);
    }

    // Screen-plane pan matched 1:1 to viewport pixel scale using the active vertical FOV.
    void Pan(float deltaX, float deltaY, float viewportH = 1080.f) noexcept {
        const double h = (std::isfinite(viewportH) && viewportH > 1.f) ? static_cast<double>(viewportH) : 1080.0;
        const double tanHalf = std::max(std::tan(static_cast<double>(fovDeg) * 0.5 * kDegToRad), 1e-4);
        const double focalPx = (h * 0.5) / tanHalf;
        const double metersPerPx = distance / focalPx;
        pivot -= Right() * (static_cast<double>(deltaX) * metersPerPx);
        pivot += Up() * (static_cast<double>(deltaY) * metersPerPx);
    }

    void Zoom(float wheelNotches) noexcept {
        if (!std::isfinite(wheelNotches) || wheelNotches == 0.f) return;
        distance = std::clamp(distance * std::pow(0.88, static_cast<double>(wheelNotches)),
                              kMinDistance, kMaxDistance);
    }

    // Move pivot to `point` without changing view angle; if `maxDistance > 0`, pull in only when farther.
    void FocusOn(const Vec3& point, double maxDistance = -1.0) noexcept {
        pivot = point;
        if (maxDistance > 0.0 && distance > maxDistance) {
            distance = std::clamp(maxDistance, kMinDistance, kMaxDistance);
        }
    }
};

}  // namespace betterpose

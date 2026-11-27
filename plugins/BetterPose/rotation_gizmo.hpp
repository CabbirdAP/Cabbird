#pragma once

#include "pose_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace betterpose {

constexpr int kGizmoAxisNone = -1;
constexpr int kGizmoAxisX = 0;
constexpr int kGizmoAxisY = 1;
constexpr int kGizmoAxisZ = 2;

constexpr int kGizmoRingSegments = 48;
constexpr float kGizmoScreenRadiusPx = 56.f;
constexpr float kGizmoHitThresholdPx = 11.f;
constexpr float kGizmoArrowScreenLenPx = 68.f;
constexpr float kGizmoArrowHitThresholdPx = 14.f;
constexpr double kGizmoDragSensDegPerPx = 0.65;

inline Vec3 GizmoLocalAxis(int axis) noexcept {
    switch (axis) {
        case kGizmoAxisX: return {1.0, 0.0, 0.0};
        case kGizmoAxisY: return {0.0, 1.0, 0.0};
        case kGizmoAxisZ: return {0.0, 0.0, 1.0};
        default:          return {0.0, 0.0, 0.0};
    }
}

// Compute a world-space radius (in meters) around `origin` that projects to `desiredPx` on screen.
inline double ComputeGizmoWorldRadius(const CameraProjection& cam, const Vec3& origin,
                                      float desiredPx = kGizmoScreenRadiusPx) noexcept {
    Vec2 o2{};
    if (!cam.WorldToScreen(origin, o2)) return 0.15;

    const double probeMeters = 0.05;
    double pxPerMeter = 0.0;
    Vec2 p2{};
    if (cam.WorldToScreen(origin + cam.Right() * probeMeters, p2)) {
        const double d = std::hypot(static_cast<double>(p2.x - o2.x), static_cast<double>(p2.y - o2.y));
        if (d > 1e-3) pxPerMeter = d / probeMeters;
    }
    if (pxPerMeter <= 1e-3 && cam.WorldToScreen(origin + cam.Up() * probeMeters, p2)) {
        const double d = std::hypot(static_cast<double>(p2.x - o2.x), static_cast<double>(p2.y - o2.y));
        if (d > 1e-3) pxPerMeter = d / probeMeters;
    }
    if (pxPerMeter <= 1e-3) return 0.15;
    return std::clamp(static_cast<double>(desiredPx) / pxPerMeter, 0.02, 2.5);
}

inline double PointToSegmentDistSq(Vec2 p, Vec2 a, Vec2 b) noexcept {
    const double abx = static_cast<double>(b.x - a.x);
    const double aby = static_cast<double>(b.y - a.y);
    const double lenSq = abx * abx + aby * aby;
    if (lenSq <= 1e-9) {
        const double dx = static_cast<double>(p.x - a.x);
        const double dy = static_cast<double>(p.y - a.y);
        return dx * dx + dy * dy;
    }
    const double t = std::clamp(
        ((static_cast<double>(p.x - a.x) * abx + static_cast<double>(p.y - a.y) * aby) / lenSq),
        0.0, 1.0);
    const double cx = static_cast<double>(a.x) + t * abx;
    const double cy = static_cast<double>(a.y) + t * aby;
    const double dx = static_cast<double>(p.x) - cx;
    const double dy = static_cast<double>(p.y) - cy;
    return dx * dx + dy * dy;
}

// Build orthonormal basis (u, v) in the plane perpendicular to `axisWorld`.
inline void BuildRingBasis(const Quat& orientation, int axis,
                           Vec3& outAxisWorld, Vec3& outU, Vec3& outV) noexcept {
    const Vec3 lx = orientation.Rotate({1.0, 0.0, 0.0}).Normalized({1.0, 0.0, 0.0});
    const Vec3 ly = orientation.Rotate({0.0, 1.0, 0.0}).Normalized({0.0, 1.0, 0.0});
    const Vec3 lz = orientation.Rotate({0.0, 0.0, 1.0}).Normalized({0.0, 0.0, 1.0});
    if (axis == kGizmoAxisX) {
        outAxisWorld = lx; outU = ly; outV = lz;
    } else if (axis == kGizmoAxisY) {
        outAxisWorld = ly; outU = lz; outV = lx;
    } else {
        outAxisWorld = lz; outU = lx; outV = ly;
    }
}

struct GizmoScreenSegment {
    Vec2 a{};
    Vec2 b{};
    bool frontFacing{true};
};

inline void BuildRotationRingSegments(const CameraProjection& cam, const Vec3& origin,
                                      const Quat& orientation, int axis, double worldRadius,
                                      std::vector<GizmoScreenSegment>& outSegments) {
    outSegments.clear();
    outSegments.reserve(kGizmoRingSegments);
    Vec3 axisWorld{}, u{}, v{};
    BuildRingBasis(orientation, axis, axisWorld, u, v);
    const Vec3 toCam = (cam.cameraPos - origin).Normalized();

    std::array<Vec2, kGizmoRingSegments + 1> pts{};
    std::array<bool, kGizmoRingSegments + 1> ok{};
    std::array<bool, kGizmoRingSegments + 1> front{};
    for (int i = 0; i <= kGizmoRingSegments; ++i) {
        const double ang = (2.0 * kPi * static_cast<double>(i)) / static_cast<double>(kGizmoRingSegments);
        const Vec3 dir = u * std::cos(ang) + v * std::sin(ang);
        const Vec3 worldPt = origin + dir * worldRadius;
        ok[static_cast<size_t>(i)] = cam.WorldToScreen(worldPt, pts[static_cast<size_t>(i)]);
        front[static_cast<size_t>(i)] = dir.Dot(toCam) >= -0.12;
    }
    for (int i = 0; i < kGizmoRingSegments; ++i) {
        if (!ok[static_cast<size_t>(i)] || !ok[static_cast<size_t>(i + 1)]) continue;
        outSegments.push_back({
            pts[static_cast<size_t>(i)],
            pts[static_cast<size_t>(i + 1)],
            front[static_cast<size_t>(i)] || front[static_cast<size_t>(i + 1)]
        });
    }
}

struct GizmoPickResult {
    int axis{kGizmoAxisNone};
    Vec3 hitWorldPoint{};
    Vec2 tangent2D{1.f, 0.f};
    double distPx{1e9};
};

// Hit-test the three rotation rings against screen cursor `(mouseX, mouseY)`.
inline GizmoPickResult PickRotationGizmo(const CameraProjection& cam, const Vec3& origin,
                                         const Quat& orientation, float mouseX, float mouseY) noexcept {
    GizmoPickResult best;
    const double worldRadius = ComputeGizmoWorldRadius(cam, origin, kGizmoScreenRadiusPx);
    const Vec2 mouse{mouseX, mouseY};
    const double threshSq = static_cast<double>(kGizmoHitThresholdPx) * static_cast<double>(kGizmoHitThresholdPx);
    const Vec3 toCam = (cam.cameraPos - origin).Normalized();

    for (int axis = 0; axis < 3; ++axis) {
        Vec3 axisWorld{}, u{}, v{};
        BuildRingBasis(orientation, axis, axisWorld, u, v);

        Vec2 prev{};
        bool prevOk = false;
        for (int i = 0; i <= kGizmoRingSegments; ++i) {
            const double ang = (2.0 * kPi * static_cast<double>(i)) / static_cast<double>(kGizmoRingSegments);
            const Vec3 radial = u * std::cos(ang) + v * std::sin(ang);
            const Vec3 wp = origin + radial * worldRadius;
            Vec2 sp{};
            const bool curOk = cam.WorldToScreen(wp, sp);
            if (prevOk && curOk) {
                const double dSq = PointToSegmentDistSq(mouse, prev, sp);
                // Slightly prefer front-facing ring segments when two rings cross on screen.
                const double penalty = (radial.Dot(toCam) >= -0.15) ? 0.0 : 4.0;
                if (dSq <= threshSq && (dSq + penalty) < best.distPx * best.distPx) {
                    best.axis = axis;
                    best.distPx = std::sqrt(dSq + penalty);
                    best.hitWorldPoint = wp;
                    // Positive rotation around `axisWorld` moves `radial` along `axisWorld x radial`.
                    const Vec3 tangentWorld = axisWorld.Cross(radial).Normalized();
                    Vec2 sp2{};
                    if (cam.WorldToScreen(wp + tangentWorld * (worldRadius * 0.1), sp2)) {
                        const double tx = static_cast<double>(sp2.x - sp.x);
                        const double ty = static_cast<double>(sp2.y - sp.y);
                        const double tl = std::hypot(tx, ty);
                        if (tl > 1e-4) {
                            best.tangent2D = {static_cast<float>(tx / tl), static_cast<float>(ty / tl)};
                        }
                    }
                }
            }
            prev = sp;
            prevOk = curOk;
        }
    }
    return best;
}

// Compute signed rotation delta in degrees from screen-space drag projected onto the ring tangent.
inline double ComputeGizmoDragDegrees(Vec2 dragDeltaPx, Vec2 tangent2D) noexcept {
    const double projPx = static_cast<double>(dragDeltaPx.x) * static_cast<double>(tangent2D.x) +
                          static_cast<double>(dragDeltaPx.y) * static_cast<double>(tangent2D.y);
    return projPx * kGizmoDragSensDegPerPx;
}

struct RootArrowPick {
    int axis{kGizmoAxisNone};
    Vec2 screenAxisDir{1.f, 0.f};
    double pxPerMeter{100.0};
    double distPx{1e9};
};

// Hit-test the 3 world-space (or root-parent-space) translation arrows on the skeleton root.
inline RootArrowPick PickRootMoveArrows(const CameraProjection& cam, const Vec3& origin,
                                        const Quat& frameRot, float mouseX, float mouseY) noexcept {
    RootArrowPick best;
    Vec2 o2{};
    if (!cam.WorldToScreen(origin, o2)) return best;
    const double worldLen = ComputeGizmoWorldRadius(cam, origin, kGizmoArrowScreenLenPx);
    const Vec2 mouse{mouseX, mouseY};
    const double threshSq = static_cast<double>(kGizmoArrowHitThresholdPx) * static_cast<double>(kGizmoArrowHitThresholdPx);

    for (int axis = 0; axis < 3; ++axis) {
        const Vec3 dirWorld = frameRot.Rotate(GizmoLocalAxis(axis)).Normalized();
        Vec2 tip{};
        if (!cam.WorldToScreen(origin + dirWorld * worldLen, tip)) continue;
        const double segLenPx = std::hypot(static_cast<double>(tip.x - o2.x), static_cast<double>(tip.y - o2.y));
        if (segLenPx < 8.0) continue;
        const double dSq = PointToSegmentDistSq(mouse, o2, tip);
        if (dSq <= threshSq && dSq < best.distPx * best.distPx) {
            best.axis = axis;
            best.distPx = std::sqrt(dSq);
            best.screenAxisDir = {
                static_cast<float>((tip.x - o2.x) / segLenPx),
                static_cast<float>((tip.y - o2.y) / segLenPx)
            };
            best.pxPerMeter = segLenPx / worldLen;
        }
    }
    return best;
}

inline double ComputeRootArrowDragMeters(Vec2 dragDeltaPx, const RootArrowPick& pick) noexcept {
    if (pick.axis == kGizmoAxisNone || !(pick.pxPerMeter > 1e-3)) return 0.0;
    const double projPx = static_cast<double>(dragDeltaPx.x) * static_cast<double>(pick.screenAxisDir.x) +
                          static_cast<double>(dragDeltaPx.y) * static_cast<double>(pick.screenAxisDir.y);
    return projPx / pick.pxPerMeter;
}

}  // namespace betterpose

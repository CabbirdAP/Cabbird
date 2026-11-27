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
constexpr float kGizmoScreenRadiusPx = 48.f;
constexpr float kGizmoHitThresholdPx = 7.5f;
constexpr float kGizmoArrowScreenLenPx = 78.f;
constexpr float kGizmoArrowHitThresholdPx = 9.0f;
constexpr float kGizmoArrowInnerRatio = 0.35f;
constexpr float kGizmoDefaultCenterExclusionPx = 16.f;
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

inline double PointToSegmentClosest(Vec2 p, Vec2 a, Vec2 b, Vec2& outClosest) noexcept {
    const double abx = static_cast<double>(b.x - a.x);
    const double aby = static_cast<double>(b.y - a.y);
    const double lenSq = abx * abx + aby * aby;
    if (lenSq <= 1e-9) {
        outClosest = a;
        const double dx = static_cast<double>(p.x - a.x);
        const double dy = static_cast<double>(p.y - a.y);
        return dx * dx + dy * dy;
    }
    const double t = std::clamp(
        ((static_cast<double>(p.x - a.x) * abx + static_cast<double>(p.y - a.y) * aby) / lenSq),
        0.0, 1.0);
    const double cx = static_cast<double>(a.x) + t * abx;
    const double cy = static_cast<double>(a.y) + t * aby;
    outClosest = {static_cast<float>(cx), static_cast<float>(cy)};
    const double dx = static_cast<double>(p.x) - cx;
    const double dy = static_cast<double>(p.y) - cy;
    return dx * dx + dy * dy;
}

inline double PointToSegmentDistSq(Vec2 p, Vec2 a, Vec2 b) noexcept {
    Vec2 closest{};
    return PointToSegmentClosest(p, a, b, closest);
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
        front[static_cast<size_t>(i)] = dir.Dot(toCam) >= -1e-6;
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
    Vec3 axisWorld{};
    Vec2 originScreen{};
    Vec2 grabScreen{};
    Vec2 tangent2D{1.f, 0.f};
    double radiusPx{kGizmoScreenRadiusPx};
    double openness{0.0};
    double distPx{1e9};
};

// Hit-test the camera-facing halves of the three rotation rings against screen cursor `(mouseX, mouseY)`.
// The back halves are never pickable, and any click inside `centerExclusionPx` of the joint's
// projected center belongs to the joint dot (so edge-on rings cannot hijack position dragging).
inline GizmoPickResult PickRotationGizmo(const CameraProjection& cam, const Vec3& origin,
                                         const Quat& orientation, float mouseX, float mouseY,
                                         float centerExclusionPx = kGizmoDefaultCenterExclusionPx) noexcept {
    GizmoPickResult best;
    Vec2 o2{};
    if (!cam.WorldToScreen(origin, o2)) return best;

    const double exclPx = static_cast<double>(std::max(0.f, centerExclusionPx));
    const double exclSq = exclPx * exclPx;
    const double mdx = static_cast<double>(mouseX - o2.x);
    const double mdy = static_cast<double>(mouseY - o2.y);
    if (mdx * mdx + mdy * mdy <= exclSq) {
        return best;
    }

    const double worldRadius = ComputeGizmoWorldRadius(cam, origin, kGizmoScreenRadiusPx);
    const Vec2 mouse{mouseX, mouseY};
    const double threshSq = static_cast<double>(kGizmoHitThresholdPx) * static_cast<double>(kGizmoHitThresholdPx);
    const Vec3 toCam = (cam.cameraPos - origin).Normalized();

    for (int axis = 0; axis < 3; ++axis) {
        Vec3 axisWorld{}, u{}, v{};
        BuildRingBasis(orientation, axis, axisWorld, u, v);
        const double openness = std::fabs(axisWorld.Dot(toCam));

        Vec2 prev{};
        Vec3 prevWorld{};
        bool prevOk = false;
        bool prevFront = false;
        for (int i = 0; i <= kGizmoRingSegments; ++i) {
            const double ang = (2.0 * kPi * static_cast<double>(i)) / static_cast<double>(kGizmoRingSegments);
            const Vec3 radial = u * std::cos(ang) + v * std::sin(ang);
            const Vec3 wp = origin + radial * worldRadius;
            Vec2 sp{};
            const bool curOk = cam.WorldToScreen(wp, sp);
            const bool curFront = radial.Dot(toCam) >= -1e-6;
            if (prevOk && curOk && (prevFront || curFront)) {
                Vec2 closest{};
                const double dSq = PointToSegmentClosest(mouse, prev, sp, closest);
                if (dSq <= threshSq && dSq < best.distPx * best.distPx) {
                    const double cdx = static_cast<double>(closest.x - o2.x);
                    const double cdy = static_cast<double>(closest.y - o2.y);
                    if (cdx * cdx + cdy * cdy > exclSq) {
                        const double segDx = static_cast<double>(sp.x - prev.x);
                        const double segDy = static_cast<double>(sp.y - prev.y);
                        const double segLenSq = segDx * segDx + segDy * segDy;
                        double t = 0.5;
                        if (segLenSq > 1e-9) {
                            t = std::clamp(
                                ((static_cast<double>(closest.x - prev.x) * segDx +
                                  static_cast<double>(closest.y - prev.y) * segDy) / segLenSq),
                                0.0, 1.0);
                        }
                        const Vec3 hitWorld = prevWorld + (wp - prevWorld) * t;
                        const Vec3 hitRadial = (hitWorld - origin).Normalized(radial);
                        const Vec3 turnedWorld = origin + Quat::FromAxisAngle(axisWorld, 0.05).Rotate(hitRadial) * worldRadius;

                        best.axis = axis;
                        best.distPx = std::sqrt(dSq);
                        best.hitWorldPoint = hitWorld;
                        best.axisWorld = axisWorld;
                        best.originScreen = o2;
                        best.grabScreen = closest;
                        best.radiusPx = kGizmoScreenRadiusPx;
                        best.openness = openness;

                        Vec2 grabProj{}, turnedProj{};
                        if (cam.WorldToScreen(hitWorld, grabProj) && cam.WorldToScreen(turnedWorld, turnedProj)) {
                            const double tx = static_cast<double>(turnedProj.x - grabProj.x);
                            const double ty = static_cast<double>(turnedProj.y - grabProj.y);
                            const double tl = std::hypot(tx, ty);
                            if (tl > 1e-4) {
                                best.tangent2D = {static_cast<float>(tx / tl), static_cast<float>(ty / tl)};
                            }
                        }
                    }
                }
            }
            prev = sp;
            prevWorld = wp;
            prevOk = curOk;
            prevFront = curFront;
        }
    }
    return best;
}

// Tracks cursor motion on a grabbed rotation ring and turns it into signed radians around the
// ring's axis: blends circular sweep around the joint (when the ring faces the camera) with
// linear travel along the measured screen tangent (when viewed nearly edge-on).
struct GizmoDragTracker {
    Vec2 originScreen{};
    Vec2 startCursor{};
    Vec2 tangent2D{1.f, 0.f};
    double sweepSign{1.0};
    double radiusPx{kGizmoScreenRadiusPx};
    double openness{0.0};
    bool sweepReady{false};
    double sweepLast{0.0};
    double sweepAccum{0.0};
};

inline GizmoDragTracker BeginGizmoDrag(const GizmoPickResult& pick, Vec2 cursor) noexcept {
    GizmoDragTracker t;
    t.originScreen = pick.originScreen;
    t.startCursor = cursor;
    t.tangent2D = pick.tangent2D;
    t.radiusPx = std::max(pick.radiusPx, 20.0);
    t.openness = pick.openness;

    const double rx = static_cast<double>(pick.grabScreen.x - pick.originScreen.x);
    const double ry = static_cast<double>(pick.grabScreen.y - pick.originScreen.y);
    const double cross = rx * static_cast<double>(pick.tangent2D.y) -
                         ry * static_cast<double>(pick.tangent2D.x);
    t.sweepSign = (cross >= 0.0) ? 1.0 : -1.0;

    const double dx = static_cast<double>(cursor.x - pick.originScreen.x);
    const double dy = static_cast<double>(cursor.y - pick.originScreen.y);
    t.sweepReady = (dx * dx + dy * dy) > 16.0;
    t.sweepLast = t.sweepReady ? std::atan2(dy, dx) : 0.0;
    t.sweepAccum = 0.0;
    return t;
}

inline double UpdateGizmoDragRadians(GizmoDragTracker& t, Vec2 cursor) noexcept {
    const double dx = static_cast<double>(cursor.x - t.originScreen.x);
    const double dy = static_cast<double>(cursor.y - t.originScreen.y);
    if (dx * dx + dy * dy > 16.0) {
        const double raw = std::atan2(dy, dx);
        if (!t.sweepReady) {
            t.sweepReady = true;
            t.sweepLast = raw;
        } else {
            t.sweepAccum += std::remainder(raw - t.sweepLast, 2.0 * kPi);
            t.sweepLast = raw;
        }
    }
    const double around = t.sweepAccum * t.sweepSign;
    const double mx = static_cast<double>(cursor.x - t.startCursor.x);
    const double my = static_cast<double>(cursor.y - t.startCursor.y);
    const double along = (mx * static_cast<double>(t.tangent2D.x) +
                          my * static_cast<double>(t.tangent2D.y)) / std::max(t.radiusPx, 20.0);
    const double w = std::clamp((t.openness - 0.25) / 0.5, 0.0, 1.0);
    return around * w + along * (1.0 - w);
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

// Hit-test the outer portion of the 3 root translation arrows so the joint center dot belongs
// strictly to free joint/root dragging.
inline RootArrowPick PickRootMoveArrows(const CameraProjection& cam, const Vec3& origin,
                                        const Quat& frameRot, float mouseX, float mouseY,
                                        float centerExclusionPx = kGizmoDefaultCenterExclusionPx) noexcept {
    RootArrowPick best;
    Vec2 o2{};
    if (!cam.WorldToScreen(origin, o2)) return best;

    const double exclPx = static_cast<double>(std::max(0.f, centerExclusionPx));
    const double mdx = static_cast<double>(mouseX - o2.x);
    const double mdy = static_cast<double>(mouseY - o2.y);
    if (mdx * mdx + mdy * mdy <= exclPx * exclPx) {
        return best;
    }

    const double worldLen = ComputeGizmoWorldRadius(cam, origin, kGizmoArrowScreenLenPx);
    const Vec2 mouse{mouseX, mouseY};
    const double threshSq = static_cast<double>(kGizmoArrowHitThresholdPx) * static_cast<double>(kGizmoArrowHitThresholdPx);

    for (int axis = 0; axis < 3; ++axis) {
        const Vec3 dirWorld = frameRot.Rotate(GizmoLocalAxis(axis)).Normalized();
        Vec2 tip{};
        if (!cam.WorldToScreen(origin + dirWorld * worldLen, tip)) continue;
        const double ax = static_cast<double>(tip.x - o2.x);
        const double ay = static_cast<double>(tip.y - o2.y);
        const double segLenPx = std::hypot(ax, ay);
        if (segLenPx < 12.0) continue;
        const Vec2 shaftStart{
            static_cast<float>(static_cast<double>(o2.x) + ax * kGizmoArrowInnerRatio),
            static_cast<float>(static_cast<double>(o2.y) + ay * kGizmoArrowInnerRatio)
        };
        const double dSq = PointToSegmentDistSq(mouse, shaftStart, tip);
        if (dSq <= threshSq && dSq < best.distPx * best.distPx) {
            best.axis = axis;
            best.distPx = std::sqrt(dSq);
            best.screenAxisDir = {
                static_cast<float>(ax / segLenPx),
                static_cast<float>(ay / segLenPx)
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

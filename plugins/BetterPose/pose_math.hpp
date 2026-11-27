#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace betterpose {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

struct Vec2 {
    float x{};
    float y{};
};

struct Vec3 {
    double x{};
    double y{};
    double z{};

    Vec3 operator+(const Vec3& o) const noexcept { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const noexcept { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const noexcept { return {-x, -y, -z}; }
    Vec3 operator*(double s) const noexcept { return {x * s, y * s, z * s}; }
    Vec3 operator/(double s) const noexcept { return {x / s, y / s, z / s}; }
    Vec3& operator+=(const Vec3& o) noexcept { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) noexcept { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(double s) noexcept { x *= s; y *= s; z *= s; return *this; }

    double Dot(const Vec3& o) const noexcept { return x * o.x + y * o.y + z * o.z; }
    Vec3 Cross(const Vec3& o) const noexcept {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    double LengthSq() const noexcept { return x * x + y * y + z * z; }
    double Length() const noexcept { return std::sqrt(LengthSq()); }
    bool IsFinite() const noexcept {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }
    Vec3 Normalized(const Vec3& fallback = {0.0, 0.0, 0.0}) const noexcept {
        const double len = Length();
        if (!(len > 1e-12)) return fallback;
        return {x / len, y / len, z / len};
    }
};

inline Vec3 operator*(double s, const Vec3& v) noexcept { return v * s; }

// Unity left-handed Y-up Euler angles in degrees:
// pitch = rotation around X, yaw = rotation around Y, roll = rotation around Z.
// Unity applies intrinsic Z -> X -> Y (equivalently q = Qy(yaw) * Qx(pitch) * Qz(roll)).
struct EulerAngles {
    float pitch{};
    float yaw{};
    float roll{};

    bool IsNearZero(float eps = 1e-3f) const noexcept {
        return std::fabs(pitch) <= eps && std::fabs(yaw) <= eps && std::fabs(roll) <= eps;
    }
};

struct Quat {
    double w{1.0};
    double x{};
    double y{};
    double z{};

    static constexpr Quat Identity() noexcept { return {1.0, 0.0, 0.0, 0.0}; }

    double NormSq() const noexcept { return w * w + x * x + y * y + z * z; }
    double Dot(const Quat& o) const noexcept { return w * o.w + x * o.x + y * o.y + z * o.z; }
    bool IsFinite() const noexcept {
        return std::isfinite(w) && std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }

    Quat Normalized() const noexcept {
        const double n = std::sqrt(NormSq());
        if (!(n > 1e-12)) return Identity();
        return {w / n, x / n, y / n, z / n};
    }

    Quat Conjugate() const noexcept { return {w, -x, -y, -z}; }
    Quat Inverse() const noexcept {
        const double nsq = NormSq();
        if (!(nsq > 1e-18)) return Identity();
        return {w / nsq, -x / nsq, -y / nsq, -z / nsq};
    }

    Quat operator*(const Quat& o) const noexcept {
        return {
            w * o.w - x * o.x - y * o.y - z * o.z,
            w * o.x + x * o.w + y * o.z - z * o.y,
            w * o.y - x * o.z + y * o.w + z * o.x,
            w * o.z + x * o.y - y * o.x + z * o.w
        };
    }

    Vec3 Rotate(const Vec3& v) const noexcept {
        const Vec3 u{x, y, z};
        const Vec3 t = 2.0 * u.Cross(v);
        return v + w * t + u.Cross(t);
    }

    static Quat FromAxisAngle(const Vec3& axis, double radians) noexcept {
        const Vec3 n = axis.Normalized();
        if (n.LengthSq() < 0.5 || !std::isfinite(radians)) return Identity();
        const double half = 0.5 * radians;
        const double s = std::sin(half);
        return Quat{std::cos(half), n.x * s, n.y * s, n.z * s}.Normalized();
    }

    static Quat FromUnityEuler(const EulerAngles& e) noexcept {
        const double hx = static_cast<double>(e.pitch) * kDegToRad * 0.5;
        const double hy = static_cast<double>(e.yaw) * kDegToRad * 0.5;
        const double hz = static_cast<double>(e.roll) * kDegToRad * 0.5;
        const Quat qx{std::cos(hx), std::sin(hx), 0.0, 0.0};
        const Quat qy{std::cos(hy), 0.0, std::sin(hy), 0.0};
        const Quat qz{std::cos(hz), 0.0, 0.0, std::sin(hz)};
        return (qy * qx * qz).Normalized();
    }

    static Quat FromUnityEuler(float pitchDeg, float yawDeg, float rollDeg) noexcept {
        return FromUnityEuler(EulerAngles{pitchDeg, yawDeg, rollDeg});
    }

    // Exact inverse of Qy(yaw) * Qx(pitch) * Qz(roll).
    // Rotation matrix R = Ry * Rx * Rz has:
    //   R[1][2] = -sin(pitch)
    //   R[0][2] =  sin(yaw) * cos(pitch),  R[2][2] = cos(yaw) * cos(pitch)
    //   R[1][0] =  sin(roll) * cos(pitch), R[1][1] = cos(roll) * cos(pitch)
    EulerAngles ToUnityEuler() const noexcept {
        const Quat q = Normalized();
        const double r00 = 1.0 - 2.0 * (q.y * q.y + q.z * q.z);
        const double r02 = 2.0 * (q.x * q.z + q.w * q.y);
        const double r10 = 2.0 * (q.x * q.y + q.w * q.z);
        const double r11 = 1.0 - 2.0 * (q.x * q.x + q.z * q.z);
        const double r12 = 2.0 * (q.y * q.z - q.w * q.x);
        const double r20 = 2.0 * (q.x * q.z - q.w * q.y);
        const double r22 = 1.0 - 2.0 * (q.x * q.x + q.y * q.y);

        const double sinPitch = std::clamp(-r12, -1.0, 1.0);
        const double pitch = std::asin(sinPitch);
        double yaw = 0.0;
        double roll = 0.0;
        if (std::fabs(sinPitch) < 0.999995) {
            yaw = std::atan2(r02, r22);
            roll = std::atan2(r10, r11);
        } else {
            // Gimbal lock: pitch = +-90 deg. Put combined rotation into yaw and zero roll.
            yaw = std::atan2(-r20, r00);
            roll = 0.0;
        }
        return {
            static_cast<float>(pitch * kRadToDeg),
            static_cast<float>(yaw * kRadToDeg),
            static_cast<float>(roll * kRadToDeg)
        };
    }

    // Shortest-arc rotation taking unit direction `from` onto unit direction `to`.
    static Quat FromToRotation(const Vec3& fromDir, const Vec3& toDir) noexcept {
        const Vec3 a = fromDir.Normalized();
        const Vec3 b = toDir.Normalized();
        if (a.LengthSq() < 0.5 || b.LengthSq() < 0.5) return Identity();
        const double d = std::clamp(a.Dot(b), -1.0, 1.0);
        if (d > 1.0 - 1e-10) return Identity();
        if (d < -1.0 + 1e-10) {
            Vec3 ortho = (std::fabs(a.x) < 0.9) ? Vec3{1.0, 0.0, 0.0} : Vec3{0.0, 1.0, 0.0};
            ortho = a.Cross(ortho).Normalized();
            return Quat{0.0, ortho.x, ortho.y, ortho.z};
        }
        const Vec3 c = a.Cross(b);
        return Quat{1.0 + d, c.x, c.y, c.z}.Normalized();
    }

    // Build quaternion from orthonormal right/up/forward basis columns (X, Y, Z).
    static Quat FromBasis(const Vec3& right, const Vec3& up, const Vec3& forward) noexcept {
        const double m00 = right.x,   m01 = up.x,   m02 = forward.x;
        const double m10 = right.y,   m11 = up.y,   m12 = forward.y;
        const double m20 = right.z,   m21 = up.z,   m22 = forward.z;
        const double trace = m00 + m11 + m22;
        Quat q;
        if (trace > 0.0) {
            const double s = std::sqrt(trace + 1.0) * 2.0;
            q.w = 0.25 * s;
            q.x = (m21 - m12) / s;
            q.y = (m02 - m20) / s;
            q.z = (m10 - m01) / s;
        } else if (m00 > m11 && m00 > m22) {
            const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
            q.w = (m21 - m12) / s;
            q.x = 0.25 * s;
            q.y = (m01 + m10) / s;
            q.z = (m02 + m20) / s;
        } else if (m11 > m22) {
            const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
            q.w = (m02 - m20) / s;
            q.x = (m01 + m10) / s;
            q.y = 0.25 * s;
            q.z = (m12 + m21) / s;
        } else {
            const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
            q.w = (m10 - m01) / s;
            q.x = (m02 + m20) / s;
            q.y = (m12 + m21) / s;
            q.z = 0.25 * s;
        }
        return q.Normalized();
    }
};

struct BoneTransform {
    Vec3 position{};
    Quat rotation{Quat::Identity()};
    Vec3 scale{1.0, 1.0, 1.0};
};

// Row-major 4x4 matrix (m[row * 4 + col]) acting on column vectors.
struct Mat4 {
    std::array<float, 16> m{
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 1.f
    };

    float At(int row, int col) const noexcept { return m[static_cast<size_t>(row * 4 + col)]; }
    float& At(int row, int col) noexcept { return m[static_cast<size_t>(row * 4 + col)]; }

    bool IsFinite() const noexcept {
        for (float v : m) {
            if (!std::isfinite(v)) return false;
        }
        return true;
    }

    // Unity stores Matrix4x4 column-major (m00, m10, m20, m30, m01, ...).
    static Mat4 FromUnityColumnMajor(const float colMajor[16]) noexcept {
        Mat4 out;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                out.At(row, col) = colMajor[col * 4 + row];
            }
        }
        return out;
    }

    static Mat4 Multiply(const Mat4& lhs, const Mat4& rhs) noexcept {
        Mat4 out;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                double sum = 0.0;
                for (int k = 0; k < 4; ++k) {
                    sum += static_cast<double>(lhs.At(row, k)) * static_cast<double>(rhs.At(k, col));
                }
                out.At(row, col) = static_cast<float>(sum);
            }
        }
        return out;
    }

    // General 4x4 inverse (used for Unity Mesh.bindposes -> mesh-space bind transforms).
    bool Inverse(Mat4& out) const noexcept {
        double aug[4][8]{};
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                aug[r][c] = static_cast<double>(At(r, c));
                aug[r][c + 4] = (r == c) ? 1.0 : 0.0;
            }
        }
        for (int col = 0; col < 4; ++col) {
            int pivot = col;
            double best = std::fabs(aug[col][col]);
            for (int r = col + 1; r < 4; ++r) {
                const double v = std::fabs(aug[r][col]);
                if (v > best) { best = v; pivot = r; }
            }
            if (!(best > 1e-12)) return false;
            if (pivot != col) {
                for (int c = 0; c < 8; ++c) std::swap(aug[col][c], aug[pivot][c]);
            }
            const double invPivot = 1.0 / aug[col][col];
            for (int c = 0; c < 8; ++c) aug[col][c] *= invPivot;
            for (int r = 0; r < 4; ++r) {
                if (r == col) continue;
                const double factor = aug[r][col];
                if (factor == 0.0) continue;
                for (int c = 0; c < 8; ++c) aug[r][c] -= factor * aug[col][c];
            }
        }
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                const double v = aug[r][c + 4];
                if (!std::isfinite(v)) return false;
                out.At(r, c) = static_cast<float>(v);
            }
        }
        return true;
    }

    // Extract rotation quaternion from upper-left 3x3 after removing column scale.
    bool ExtractRotation(Quat& outRot) const noexcept {
        Vec3 cx{At(0, 0), At(1, 0), At(2, 0)};
        Vec3 cy{At(0, 1), At(1, 1), At(2, 1)};
        Vec3 cz{At(0, 2), At(1, 2), At(2, 2)};
        const double lx = cx.Length();
        const double ly = cy.Length();
        const double lz = cz.Length();
        if (!(lx > 1e-7) || !(ly > 1e-7) || !(lz > 1e-7)) return false;
        cx = cx / lx;
        cy = cy / ly;
        cz = cz / lz;
        // Handle mirrored scale (negative determinant) by flipping Z column.
        if (cx.Cross(cy).Dot(cz) < 0.0) cz = -cz;
        // Gram-Schmidt orthonormalization for numerical cleanliness.
        cy = (cy - cx * cx.Dot(cy)).Normalized();
        if (cy.LengthSq() < 0.5) return false;
        cz = cx.Cross(cy).Normalized();
        outRot = Quat::FromBasis(cx, cy, cz);
        return outRot.IsFinite();
    }

    Vec3 ExtractPosition() const noexcept {
        return {static_cast<double>(At(0, 3)), static_cast<double>(At(1, 3)), static_cast<double>(At(2, 3))};
    }

    Vec3 ExtractScale() const noexcept {
        const double sx = Vec3{At(0, 0), At(1, 0), At(2, 0)}.Length();
        const double sy = Vec3{At(0, 1), At(1, 1), At(2, 1)}.Length();
        const double sz = Vec3{At(0, 2), At(1, 2), At(2, 2)}.Length();
        return {sx, sy, sz};
    }

    static Mat4 Identity() noexcept { return Mat4{}; }

    static Mat4 FromTrs(const Vec3& pos, const Quat& rot, const Vec3& scale = {1.0, 1.0, 1.0}) noexcept {
        const Vec3 rx = rot.Rotate({scale.x, 0.0, 0.0});
        const Vec3 ry = rot.Rotate({0.0, scale.y, 0.0});
        const Vec3 rz = rot.Rotate({0.0, 0.0, scale.z});
        Mat4 out;
        out.At(0, 0) = static_cast<float>(rx.x); out.At(0, 1) = static_cast<float>(ry.x); out.At(0, 2) = static_cast<float>(rz.x); out.At(0, 3) = static_cast<float>(pos.x);
        out.At(1, 0) = static_cast<float>(rx.y); out.At(1, 1) = static_cast<float>(ry.y); out.At(1, 2) = static_cast<float>(rz.y); out.At(1, 3) = static_cast<float>(pos.y);
        out.At(2, 0) = static_cast<float>(rx.z); out.At(2, 1) = static_cast<float>(ry.z); out.At(2, 2) = static_cast<float>(rz.z); out.At(2, 3) = static_cast<float>(pos.z);
        return out;
    }
};

struct CameraProjection {
    Mat4 viewProjection{};
    Vec3 cameraPos{};
    Quat cameraRot{Quat::Identity()};
    float viewportW{};
    float viewportH{};
    float verticalFovDeg{50.f};
    bool valid{};

    Vec3 Forward() const noexcept { return cameraRot.Rotate({0.0, 0.0, 1.0}).Normalized({0.0, 0.0, 1.0}); }
    Vec3 Right() const noexcept { return cameraRot.Rotate({1.0, 0.0, 0.0}).Normalized({1.0, 0.0, 0.0}); }
    Vec3 Up() const noexcept { return cameraRot.Rotate({0.0, 1.0, 0.0}).Normalized({0.0, 1.0, 0.0}); }

    // Matches Cabbird's verified azur_unity_adapter.cpp ProjectWorldPoint convention.
    bool WorldToScreen(const Vec3& world, Vec2& outScreen, double* outDepth = nullptr) const noexcept {
        if (!valid || !(viewportW > 1.f) || !(viewportH > 1.f)) return false;
        const auto& vp = viewProjection.m;
        const double x = static_cast<double>(vp[0]) * world.x + static_cast<double>(vp[1]) * world.y +
                         static_cast<double>(vp[2]) * world.z + static_cast<double>(vp[3]);
        const double y = static_cast<double>(vp[4]) * world.x + static_cast<double>(vp[5]) * world.y +
                         static_cast<double>(vp[6]) * world.z + static_cast<double>(vp[7]);
        const double w = static_cast<double>(vp[12]) * world.x + static_cast<double>(vp[13]) * world.y +
                         static_cast<double>(vp[14]) * world.z + static_cast<double>(vp[15]);
        if (!(w > 1e-9)) return false;
        const double sx = (x / w * 0.5 + 0.5) * static_cast<double>(viewportW);
        const double sy = (0.5 - y / w * 0.5) * static_cast<double>(viewportH);
        if (!std::isfinite(sx) || !std::isfinite(sy)) return false;
        outScreen.x = static_cast<float>(sx);
        outScreen.y = static_cast<float>(sy);
        if (outDepth) *outDepth = w;
        return true;
    }

    // Build a ray from viewport pixel coordinates (origin top-left, +Y down) into world space.
    // Works directly from viewProjection inverse so it matches the exact camera frustum.
    bool ScreenToWorldRay(float screenX, float screenY, Vec3& outOrigin, Vec3& outDir) const noexcept {
        if (!valid || !(viewportW > 1.f) || !(viewportH > 1.f)) return false;
        Mat4 invVp;
        if (!viewProjection.Inverse(invVp)) return false;
        const double ndcX = (static_cast<double>(screenX) / static_cast<double>(viewportW)) * 2.0 - 1.0;
        const double ndcY = 1.0 - (static_cast<double>(screenY) / static_cast<double>(viewportH)) * 2.0;

        auto unproject = [&](double ndcZ, Vec3& pt) -> bool {
            const auto& im = invVp.m;
            const double rx = static_cast<double>(im[0]) * ndcX + static_cast<double>(im[1]) * ndcY +
                              static_cast<double>(im[2]) * ndcZ + static_cast<double>(im[3]);
            const double ry = static_cast<double>(im[4]) * ndcX + static_cast<double>(im[5]) * ndcY +
                              static_cast<double>(im[6]) * ndcZ + static_cast<double>(im[7]);
            const double rz = static_cast<double>(im[8]) * ndcX + static_cast<double>(im[9]) * ndcY +
                              static_cast<double>(im[10]) * ndcZ + static_cast<double>(im[11]);
            const double rw = static_cast<double>(im[12]) * ndcX + static_cast<double>(im[13]) * ndcY +
                              static_cast<double>(im[14]) * ndcZ + static_cast<double>(im[15]);
            if (!(std::fabs(rw) > 1e-12)) return false;
            pt = {rx / rw, ry / rw, rz / rw};
            return pt.IsFinite();
        };

        Vec3 pNear{}, pFar{};
        if (!unproject(-0.5, pNear) || !unproject(0.5, pFar)) return false;
        Vec3 dir = (pFar - pNear).Normalized();
        if (dir.LengthSq() < 0.5) return false;
        // Ensure ray points into the scene (same hemisphere as camera forward).
        if (dir.Dot(Forward()) < 0.0) dir = -dir;
        outOrigin = cameraPos;
        outDir = dir;
        return true;
    }

    // Intersect cursor ray with a plane through `planePoint` facing `planeNormal`.
    bool CursorOnPlane(float screenX, float screenY, const Vec3& planePoint,
                       const Vec3& planeNormal, Vec3& outPoint) const noexcept {
        Vec3 rayOrigin{}, rayDir{};
        if (!ScreenToWorldRay(screenX, screenY, rayOrigin, rayDir)) return false;
        const double denom = rayDir.Dot(planeNormal);
        if (std::fabs(denom) < 1e-6) return false;
        const double t = (planePoint - rayOrigin).Dot(planeNormal) / denom;
        if (!(t > 0.0) || !std::isfinite(t)) return false;
        outPoint = rayOrigin + rayDir * t;
        return outPoint.IsFinite();
    }
};

// Analytical 2-bone IK solver (Law of Cosines): solves the new middle-joint world position
// so the end joint moves toward `targetPos` while keeping `midPos` oriented toward `polePos`.
inline bool SolveTwoBoneIk(const Vec3& rootPos, const Vec3& midPos, const Vec3& endPos,
                           const Vec3& targetPos, const Vec3& polePos,
                           Vec3& outNewMidPos, Vec3& outClampedEndPos) noexcept {
    const double len1 = (midPos - rootPos).Length();
    const double len2 = (endPos - midPos).Length();
    if (!(len1 > 1e-5) || !(len2 > 1e-5)) return false;

    const Vec3 toTarget = targetPos - rootPos;
    const double rawDist = toTarget.Length();
    if (!(rawDist > 1e-6)) return false;

    const double minDist = std::fabs(len1 - len2) + 1e-4;
    const double maxDist = (len1 + len2) - 1e-4;
    const double dist = std::clamp(rawDist, minDist, maxDist);
    const Vec3 dir = toTarget / rawDist;

    // Bend plane normal from pole direction orthogonalized against `dir`.
    Vec3 toPole = polePos - rootPos;
    Vec3 bendDir = toPole - dir * toPole.Dot(dir);
    if (bendDir.LengthSq() < 1e-10) {
        const Vec3 oldMid = midPos - rootPos;
        bendDir = oldMid - dir * oldMid.Dot(dir);
    }
    if (bendDir.LengthSq() < 1e-10) {
        const Vec3 fallback = (std::fabs(dir.y) < 0.9) ? Vec3{0.0, 1.0, 0.0} : Vec3{1.0, 0.0, 0.0};
        bendDir = dir.Cross(fallback);
    }
    bendDir = bendDir.Normalized();
    if (bendDir.LengthSq() < 0.5) return false;

    const double cosAngle = std::clamp((len1 * len1 + dist * dist - len2 * len2) / (2.0 * len1 * dist), -1.0, 1.0);
    const double sinAngle = std::sqrt(std::max(0.0, 1.0 - cosAngle * cosAngle));
    outNewMidPos = rootPos + dir * (len1 * cosAngle) + bendDir * (len1 * sinAngle);
    outClampedEndPos = rootPos + dir * dist;
    return outNewMidPos.IsFinite() && outClampedEndPos.IsFinite();
}

struct SmrBindPoseSet {
    Mat4 smrInGo{};
    bool smrUnderBone{};
    std::vector<std::pair<int, Mat4>> entries;  // (boneIndex, Mesh.bindposes[i])
};

// Snap a non-zero vector to its nearest signed cardinal axis (+-X, +-Y, +-Z).
inline Vec3 NearestCardinalAxis(const Vec3& v) noexcept {
    const double ax = std::fabs(v.x);
    const double ay = std::fabs(v.y);
    const double az = std::fabs(v.z);
    if (ax >= ay && ax >= az) return {v.x >= 0.0 ? 1.0 : -1.0, 0.0, 0.0};
    if (ay >= ax && ay >= az) return {0.0, v.y >= 0.0 ? 1.0 : -1.0, 0.0};
    return {0.0, 0.0, v.z >= 0.0 ? 1.0 : -1.0};
}

// Compute unified bind-pose local rotations (and root/pelvis local positions) across ALL
// SkinnedMeshRenderers on a character, including root bones (parent == -1) and unskinned
// bridge bones (e.g. Root -> Pelvis where Root or Pelvis has no skin weights).
inline void ComputeBindLocalTransforms(const std::vector<int>& parents,
                                       const std::vector<Vec3>& capturedLocalPos,
                                       const std::vector<Vec3>& capturedLocalScale,
                                       const std::vector<Mat4>& rootParentInGo,
                                       const std::vector<SmrBindPoseSet>& smrSets,
                                       std::vector<Quat>& outLocalRot,
                                       std::vector<bool>& outRotValid,
                                       std::vector<Vec3>& outLocalPos,
                                       std::vector<bool>& outPosValid) {
    const size_t n = parents.size();
    outLocalRot.assign(n, Quat::Identity());
    outRotValid.assign(n, false);
    outLocalPos.assign(n, Vec3{});
    outPosValid.assign(n, false);
    if (n == 0) return;

    // Topological order (parents before children).
    std::vector<size_t> topo;
    topo.reserve(n);
    std::vector<uint8_t> visitState(n, 0);
    auto visit = [&](auto&& self, size_t idx) -> void {
        if (visitState[idx] != 0) return;
        visitState[idx] = 1;
        const int p = parents[idx];
        if (p >= 0 && static_cast<size_t>(p) < n && visitState[static_cast<size_t>(p)] == 0) {
            self(self, static_cast<size_t>(p));
        }
        visitState[idx] = 2;
        topo.push_back(idx);
    };
    for (size_t i = 0; i < n; ++i) visit(visit, i);

    // Pre-invert every valid bindpose matrix per SMR.
    struct PreppedSmr {
        Mat4 smrInGo{};
        bool smrUnderBone{};
        std::vector<int> boneIndices;
        std::vector<Mat4> bindposes;     // mesh -> bone
        std::vector<Mat4> invBindposes;  // bone -> mesh
        std::vector<int> slotOfBone;     // size n, -1 if absent
    };
    std::vector<PreppedSmr> prepped;
    prepped.reserve(smrSets.size());
    for (const auto& set : smrSets) {
        PreppedSmr ps;
        ps.smrInGo = set.smrInGo;
        ps.smrUnderBone = set.smrUnderBone;
        ps.slotOfBone.assign(n, -1);
        for (const auto& [bIdx, bp] : set.entries) {
            if (bIdx < 0 || static_cast<size_t>(bIdx) >= n || !bp.IsFinite()) continue;
            if (ps.slotOfBone[static_cast<size_t>(bIdx)] >= 0) continue;
            Mat4 invBp;
            if (!bp.Inverse(invBp)) continue;
            const int slot = static_cast<int>(ps.boneIndices.size());
            ps.slotOfBone[static_cast<size_t>(bIdx)] = slot;
            ps.boneIndices.push_back(bIdx);
            ps.bindposes.push_back(bp);
            ps.invBindposes.push_back(invBp);
        }
        if (!ps.boneIndices.empty()) prepped.push_back(std::move(ps));
    }

    // Step 1: Unify all SMRs into character GameObject space (`boneInGo[i]`).
    // Pick anchor SMR with the most skinned bones (preferring SMRs not parented under an animated bone).
    std::vector<Mat4> boneInGo(n, Mat4::Identity());
    std::vector<bool> hasBoneInGo(n, false);
    std::vector<bool> smrAligned(prepped.size(), false);

    if (!prepped.empty()) {
        size_t anchorIdx = 0;
        for (size_t s = 1; s < prepped.size(); ++s) {
            const bool curBetterStatic = (!prepped[s].smrUnderBone && prepped[anchorIdx].smrUnderBone);
            const bool sameStatic = (prepped[s].smrUnderBone == prepped[anchorIdx].smrUnderBone);
            if (curBetterStatic || (sameStatic && prepped[s].boneIndices.size() > prepped[anchorIdx].boneIndices.size())) {
                anchorIdx = s;
            }
        }
        auto applySmrToGo = [&](size_t s, const Mat4& smrToGo) {
            const auto& ps = prepped[s];
            for (size_t k = 0; k < ps.boneIndices.size(); ++k) {
                const size_t bi = static_cast<size_t>(ps.boneIndices[k]);
                if (!hasBoneInGo[bi]) {
                    const Mat4 m = Mat4::Multiply(smrToGo, ps.invBindposes[k]);
                    if (m.IsFinite()) {
                        boneInGo[bi] = m;
                        hasBoneInGo[bi] = true;
                    }
                }
            }
            smrAligned[s] = true;
        };

        applySmrToGo(anchorIdx, prepped[anchorIdx].smrInGo);

        // Iteratively register remaining SMRs through any shared bone (exact bind-time frame alignment!).
        bool progress = true;
        while (progress) {
            progress = false;
            for (size_t s = 0; s < prepped.size(); ++s) {
                if (smrAligned[s]) continue;
                const auto& ps = prepped[s];
                for (size_t k = 0; k < ps.boneIndices.size(); ++k) {
                    const size_t shared = static_cast<size_t>(ps.boneIndices[k]);
                    if (!hasBoneInGo[shared]) continue;
                    const Mat4 smrToGo = Mat4::Multiply(boneInGo[shared], ps.bindposes[k]);
                    if (smrToGo.IsFinite()) {
                        applySmrToGo(s, smrToGo);
                        progress = true;
                        break;
                    }
                }
            }
        }
        // Any disjoint SMR that shares zero bones falls back to its own static transform in goTransform space.
        for (size_t s = 0; s < prepped.size(); ++s) {
            if (!smrAligned[s] && !prepped[s].smrUnderBone) {
                applySmrToGo(s, prepped[s].smrInGo);
            }
        }
    }

    // Step 2: Reconstruct bind-pose transforms for unskinned bridge & root bones (reverse topo: children first).
    std::vector<std::vector<size_t>> children(n);
    for (size_t i = 0; i < n; ++i) {
        const int p = parents[i];
        if (p >= 0 && static_cast<size_t>(p) < n) {
            children[static_cast<size_t>(p)].push_back(i);
        }
    }

    std::vector<bool> wasSynthesized(n, false);
    for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
        const size_t u = *it;
        if (hasBoneInGo[u]) continue;

        std::vector<size_t> skinnedKids;
        for (size_t c : children[u]) {
            if (hasBoneInGo[c]) skinnedKids.push_back(c);
        }
        if (skinnedKids.empty()) continue;

        const Vec3 liveScale = (u < capturedLocalScale.size() && capturedLocalScale[u].IsFinite() &&
                                capturedLocalScale[u].x > 1e-5 && capturedLocalScale[u].y > 1e-5 &&
                                capturedLocalScale[u].z > 1e-5)
                                   ? capturedLocalScale[u]
                                   : Vec3{1.0, 1.0, 1.0};

        // Case 2A: >= 2 skinned children with non-collinear offsets (e.g. unskinned Pelvis -> Spine, Thigh_L, Thigh_R).
        bool solvedTriad = false;
        if (skinnedKids.size() >= 2 && capturedLocalPos.size() == n) {
            double bestArea = 1e-5;
            size_t bestA = 0, bestB = 0, bestC = 0;
            bool useOriginPivot = false;
            if (skinnedKids.size() >= 3) {
                for (size_t a = 0; a < skinnedKids.size(); ++a) {
                    for (size_t b = a + 1; b < skinnedKids.size(); ++b) {
                        for (size_t c = b + 1; c < skinnedKids.size(); ++c) {
                            const Vec3 v1 = capturedLocalPos[skinnedKids[b]] - capturedLocalPos[skinnedKids[a]];
                            const Vec3 v2 = capturedLocalPos[skinnedKids[c]] - capturedLocalPos[skinnedKids[a]];
                            const double area = v1.Cross(v2).Length();
                            if (area > bestArea) {
                                bestArea = area;
                                bestA = skinnedKids[a];
                                bestB = skinnedKids[b];
                                bestC = skinnedKids[c];
                            }
                        }
                    }
                }
            } else {
                // 2 children (e.g. Thigh_L + Thigh_R): check local vectors from u origin.
                const Vec3 v1 = capturedLocalPos[skinnedKids[0]];
                const Vec3 v2 = capturedLocalPos[skinnedKids[1]];
                if (v1.Cross(v2).Length() > bestArea) {
                    useOriginPivot = true;
                    bestA = skinnedKids[0];
                    bestB = skinnedKids[1];
                }
            }

            if (!useOriginPivot && bestArea > 1e-5) {
                const Vec3 l1 = capturedLocalPos[bestB] - capturedLocalPos[bestA];
                const Vec3 l2 = capturedLocalPos[bestC] - capturedLocalPos[bestA];
                const Vec3 b1 = boneInGo[bestB].ExtractPosition() - boneInGo[bestA].ExtractPosition();
                const Vec3 b2 = boneInGo[bestC].ExtractPosition() - boneInGo[bestA].ExtractPosition();
                const Vec3 eL1 = l1.Normalized();
                const Vec3 eL3 = eL1.Cross(l2).Normalized();
                const Vec3 eL2 = eL3.Cross(eL1).Normalized();
                const Vec3 eB1 = b1.Normalized();
                const Vec3 eB3 = eB1.Cross(b2).Normalized();
                const Vec3 eB2 = eB3.Cross(eB1).Normalized();
                if (eL2.LengthSq() > 0.5 && eB2.LengthSq() > 0.5 && l1.Length() > 1e-5) {
                    const Quat qL = Quat::FromBasis(eL1, eL2, eL3);
                    const Quat qB = Quat::FromBasis(eB1, eB2, eB3);
                    const Quat rU = (qB * qL.Conjugate()).Normalized();
                    const double uniScale = b1.Length() / l1.Length();
                    Vec3 posSum{};
                    for (size_t k : skinnedKids) {
                        posSum += boneInGo[k].ExtractPosition() - rU.Rotate(capturedLocalPos[k] * uniScale);
                    }
                    const Vec3 posU = posSum / static_cast<double>(skinnedKids.size());
                    boneInGo[u] = Mat4::FromTrs(posU, rU, {uniScale, uniScale, uniScale});
                    hasBoneInGo[u] = true;
                    wasSynthesized[u] = true;
                    solvedTriad = true;
                }
            }
        }

        // Case 2B: Unskinned root bone (e.g. floor Root / Bip001 -> Pelvis).
        if (!solvedTriad && parents[u] < 0) {
            const Mat4 parentMat = (u < rootParentInGo.size() && rootParentInGo[u].IsFinite())
                                       ? rootParentInGo[u]
                                       : Mat4::Identity();
            const Vec3 locPos = (u < capturedLocalPos.size() && capturedLocalPos[u].IsFinite())
                                    ? capturedLocalPos[u]
                                    : Vec3{};
            // Align child's cardinal local axis (e.g. +Z on 3dsMax FBX or +Y on Unity) straight to Unity +Y.
            Quat qAlign = Quat::Identity();
            const size_t primaryKid = skinnedKids.front();
            if (primaryKid < capturedLocalPos.size() && capturedLocalPos[primaryKid].LengthSq() > 1e-4) {
                const Vec3 cardLocal = NearestCardinalAxis(capturedLocalPos[primaryKid]);
                const Vec3 kidBindPos = boneInGo[primaryKid].ExtractPosition();
                const Vec3 rootBindPos = parentMat.ExtractPosition() + locPos;
                const Vec3 deltaBind = kidBindPos - rootBindPos;
                if (deltaBind.LengthSq() > 1e-4) {
                    const Vec3 cardBind = NearestCardinalAxis(deltaBind);
                    Quat parentRot = Quat::Identity();
                    parentMat.ExtractRotation(parentRot);
                    const Vec3 desiredInParent = parentRot.Inverse().Rotate(cardBind).Normalized();
                    qAlign = Quat::FromToRotation(cardLocal, desiredInParent);
                }
            }
            const Mat4 localRoot = Mat4::FromTrs(locPos, qAlign, liveScale);
            boneInGo[u] = Mat4::Multiply(parentMat, localRoot);
            hasBoneInGo[u] = true;
            wasSynthesized[u] = true;
        }
    }

    // Step 3: Extract final bindLocalRot (and root/pelvis bindLocalPos) for every resolved bone.
    for (size_t i = 0; i < n; ++i) {
        if (!hasBoneInGo[i]) continue;
        const int p = parents[i];

        if (p >= 0 && static_cast<size_t>(p) < n) {
            // Prefer exact same-SMR relative transform whenever both i and p are skinned in one SMR.
            bool doneSameSmr = false;
            for (const auto& ps : prepped) {
                const int slotI = ps.slotOfBone[i];
                const int slotP = ps.slotOfBone[static_cast<size_t>(p)];
                if (slotI >= 0 && slotP >= 0) {
                    const Mat4 localBind = Mat4::Multiply(ps.bindposes[static_cast<size_t>(slotP)],
                                                          ps.invBindposes[static_cast<size_t>(slotI)]);
                    Quat q;
                    if (localBind.ExtractRotation(q)) {
                        outLocalRot[i] = q;
                        outRotValid[i] = true;
                        outLocalPos[i] = localBind.ExtractPosition();
                        outPosValid[i] = outLocalPos[i].IsFinite();
                        doneSameSmr = true;
                        break;
                    }
                }
            }
            if (doneSameSmr) continue;

            if (hasBoneInGo[static_cast<size_t>(p)]) {
                Mat4 invParent;
                if (boneInGo[static_cast<size_t>(p)].Inverse(invParent)) {
                    const Mat4 localBind = Mat4::Multiply(invParent, boneInGo[i]);
                    Quat q;
                    if (localBind.ExtractRotation(q)) {
                        outLocalRot[i] = q;
                        outRotValid[i] = true;
                        outLocalPos[i] = localBind.ExtractPosition();
                        outPosValid[i] = outLocalPos[i].IsFinite();
                    }
                }
            }
        } else {
            // Root bone (parent == -1): extract bind rotation relative to its Unity parent in goTransform space!
            const Mat4 parentMat = (i < rootParentInGo.size() && rootParentInGo[i].IsFinite())
                                       ? rootParentInGo[i]
                                       : Mat4::Identity();
            Mat4 invParent;
            if (parentMat.Inverse(invParent)) {
                const Mat4 localBind = Mat4::Multiply(invParent, boneInGo[i]);
                Quat q;
                if (localBind.ExtractRotation(q)) {
                    outLocalRot[i] = q;
                    outRotValid[i] = true;
                    outLocalPos[i] = localBind.ExtractPosition();
                    outPosValid[i] = outLocalPos[i].IsFinite();
                }
            }
        }
    }
}

// Compute local bind-pose rotations from Unity Mesh.bindposes (single-SMR convenience wrapper).
inline void ComputeBindLocalRotations(const std::vector<int>& parents,
                                      const std::vector<Mat4>& bindPoses,
                                      const std::vector<bool>& hasBindPose,
                                      std::vector<Quat>& outLocalRot,
                                      std::vector<bool>& outValid) {
    const size_t n = parents.size();
    outLocalRot.assign(n, Quat::Identity());
    outValid.assign(n, false);
    if (bindPoses.size() != n || hasBindPose.size() != n) return;

    SmrBindPoseSet single;
    single.smrInGo = Mat4::Identity();
    for (size_t i = 0; i < n; ++i) {
        if (hasBindPose[i]) single.entries.emplace_back(static_cast<int>(i), bindPoses[i]);
    }
    std::vector<Vec3> dummyPos(n), dummyScale(n, Vec3{1.0, 1.0, 1.0}), outPos;
    std::vector<Mat4> rootParents(n, Mat4::Identity());
    std::vector<bool> outPosValid;
    ComputeBindLocalTransforms(parents, dummyPos, dummyScale, rootParents, {single},
                               outLocalRot, outValid, outPos, outPosValid);
}

// Wrap angle into [-180, 180] degrees.
inline float WrapAngle180(float deg) noexcept {
    if (!std::isfinite(deg)) return 0.f;
    float r = std::fmod(deg + 180.f, 360.f);
    if (r < 0.f) r += 360.f;
    return r - 180.f;
}

}  // namespace betterpose

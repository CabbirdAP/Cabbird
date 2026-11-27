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

// Compute local bind-pose rotations from Unity Mesh.bindposes (boneToMesh matrices) and parent indices.
// Returns true for each bone `i` whose parent `p` also has a valid bindpose matrix.
inline void ComputeBindLocalRotations(const std::vector<int>& parents,
                                      const std::vector<Mat4>& bindPoses,
                                      const std::vector<bool>& hasBindPose,
                                      std::vector<Quat>& outLocalRot,
                                      std::vector<bool>& outValid) {
    const size_t n = parents.size();
    outLocalRot.assign(n, Quat::Identity());
    outValid.assign(n, false);
    if (bindPoses.size() != n || hasBindPose.size() != n) return;

    std::vector<Mat4> meshFromBone(n);
    std::vector<bool> invOk(n, false);
    for (size_t i = 0; i < n; ++i) {
        if (hasBindPose[i] && bindPoses[i].IsFinite()) {
            invOk[i] = bindPoses[i].Inverse(meshFromBone[i]);
        }
    }
    for (size_t i = 0; i < n; ++i) {
        const int p = parents[i];
        if (!invOk[i] || p < 0 || static_cast<size_t>(p) >= n || !hasBindPose[static_cast<size_t>(p)]) {
            continue;
        }
        // localBind = inverse(meshFromBone[p]) * meshFromBone[i] = bindPoses[p] * meshFromBone[i]
        const Mat4 localBind = Mat4::Multiply(bindPoses[static_cast<size_t>(p)], meshFromBone[i]);
        Quat q;
        if (localBind.ExtractRotation(q)) {
            outLocalRot[i] = q;
            outValid[i] = true;
        }
    }
}

// Wrap angle into [-180, 180] degrees.
inline float WrapAngle180(float deg) noexcept {
    if (!std::isfinite(deg)) return 0.f;
    float r = std::fmod(deg + 180.f, 360.f);
    if (r < 0.f) r += 360.f;
    return r - 180.f;
}

}  // namespace betterpose

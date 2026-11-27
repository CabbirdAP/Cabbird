#pragma once

#include "pose_math.hpp"
#include "skeleton_filter.hpp"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace betterpose {

enum class MirrorMode : int {
    Flip = 0,
    CopyLeftToRight,
    CopyRightToLeft
};

// Swap left/right side tokens in a bone name across common Unity/Maya/3dsMax/Blender naming styles.
inline std::string SwapSideToken(std::string_view name) {
    struct Pair { std::string_view a; std::string_view b; };
    static constexpr Pair kExactPairs[]{
        {"Left", "Right"}, {"LEFT", "RIGHT"}, {"left", "right"},
        {"_L_", "_R_"}, {"_l_", "_r_"},
        {" L ", " R "}, {" l ", " r "},
        {"-L-", "-R-"}, {"-l-", "-r-"},
        {".L.", ".R."}, {".l.", ".r."}
    };
    for (const auto& p : kExactPairs) {
        if (const size_t pos = name.find(p.a); pos != std::string_view::npos) {
            std::string out(name);
            out.replace(pos, p.a.size(), p.b);
            return out;
        }
        if (const size_t pos = name.find(p.b); pos != std::string_view::npos) {
            std::string out(name);
            out.replace(pos, p.b.size(), p.a);
            return out;
        }
    }

    // Prefix tokens: "L_", "R_", "l_", "r_", "L-", "R-".
    static constexpr Pair kPrefixPairs[]{
        {"L_", "R_"}, {"l_", "r_"}, {"L-", "R-"}, {"l-", "r-"}, {"L ", "R "}
    };
    for (const auto& p : kPrefixPairs) {
        if (name.starts_with(p.a)) {
            return std::string(p.b) + std::string(name.substr(p.a.size()));
        }
        if (name.starts_with(p.b)) {
            return std::string(p.a) + std::string(name.substr(p.b.size()));
        }
    }

    // Suffix tokens: "_L", "_R", ".L", ".R", "-L", "-R", " L", " R".
    static constexpr Pair kSuffixPairs[]{
        {"_L", "_R"}, {"_l", "_r"}, {".L", ".R"}, {".l", ".r"},
        {"-L", "-R"}, {"-l", "-r"}, {" L", " R"}, {" l", " r"}
    };
    for (const auto& p : kSuffixPairs) {
        if (name.ends_with(p.a)) {
            return std::string(name.substr(0, name.size() - p.a.size())) + std::string(p.b);
        }
        if (name.ends_with(p.b)) {
            return std::string(name.substr(0, name.size() - p.b.size())) + std::string(p.a);
        }
    }
    return {};
}

// Reflect a rotation quaternion across the plane with unit normal `planeNormal`.
// If R' = S * R * S where S = I - 2 n n^T is reflection across planeNormal,
// the rotation axis component parallel to `planeNormal` is preserved and perpendicular components negate.
inline Quat MirrorQuatAcrossPlane(const Quat& qIn, const Vec3& planeNormal) noexcept {
    const Quat q = qIn.Normalized();
    const Vec3 n = planeNormal.Normalized({1.0, 0.0, 0.0});
    const Vec3 v{q.x, q.y, q.z};
    const Vec3 mirroredV = n * (2.0 * v.Dot(n)) - v;
    return Quat{q.w, mirroredV.x, mirroredV.y, mirroredV.z}.Normalized();
}

struct MirrorBoneInput {
    std::string name;
    int parent{-1};
    Quat restLocalRot{Quat::Identity()};
    EulerAngles offsetEuler{};
};

// Mirror pose across the character's sagittal plane (local X = right in Unity root space).
// Operates in rest-pose-relative root space so mirrored poses remain accurate even when left
// and right bones have oppositely handed or differently oriented rest axes.
inline int ApplyPoseMirror(const std::vector<MirrorBoneInput>& bones,
                           int rootBoneIndex,
                           MirrorMode mode,
                           std::vector<EulerAngles>& outOffsets,
                           Vec3& inoutRootOffset) {
    const size_t n = bones.size();
    outOffsets.resize(n);
    for (size_t i = 0; i < n; ++i) outOffsets[i] = bones[i].offsetEuler;
    if (n == 0) return 0;

    // 1. Compute rest and posed rotations in character-root space.
    std::vector<Quat> restRootRot(n, Quat::Identity());
    std::vector<Quat> posedRootRot(n, Quat::Identity());
    std::vector<bool> computed(n, false);

    auto computeBone = [&](auto&& self, size_t idx) -> void {
        if (computed[idx]) return;
        const int p = bones[idx].parent;
        const Quat localOffset = Quat::FromUnityEuler(bones[idx].offsetEuler);
        const Quat posedLocal = (bones[idx].restLocalRot * localOffset).Normalized();
        if (p >= 0 && static_cast<size_t>(p) < n && p != static_cast<int>(idx)) {
            self(self, static_cast<size_t>(p));
            restRootRot[idx] = (restRootRot[static_cast<size_t>(p)] * bones[idx].restLocalRot).Normalized();
            posedRootRot[idx] = (posedRootRot[static_cast<size_t>(p)] * posedLocal).Normalized();
        } else {
            restRootRot[idx] = bones[idx].restLocalRot.Normalized();
            posedRootRot[idx] = posedLocal;
        }
        computed[idx] = true;
    };
    for (size_t i = 0; i < n; ++i) computeBone(computeBone, i);

    const Quat rootBasis = (rootBoneIndex >= 0 && static_cast<size_t>(rootBoneIndex) < n)
        ? restRootRot[static_cast<size_t>(rootBoneIndex)]
        : Quat::Identity();
    const Quat invRootBasis = rootBasis.Inverse();
    // Unity character sagittal mirror plane normal in root space is +X (right).
    const Vec3 sagittalNormal{1.0, 0.0, 0.0};

    // Per-bone delta in character-root space: delta_i = posedParent^-1 * posed_i * rest_i^-1 * restParent
    // Equivalently, pure local delta expressed in root frame:
    //   localDelta_i = restRootRot[i] * offset_i * restRootRot[i]^-1  (in root frame: invRootBasis * ... * rootBasis)
    auto localOffsetToCharDelta = [&](size_t idx) -> Quat {
        const Quat offsetQ = Quat::FromUnityEuler(bones[idx].offsetEuler);
        const Quat inWorld = (restRootRot[idx] * offsetQ * restRootRot[idx].Inverse()).Normalized();
        return (invRootBasis * inWorld * rootBasis).Normalized();
    };
    auto charDeltaToLocalOffset = [&](size_t idx, const Quat& charDelta) -> EulerAngles {
        const Quat inWorld = (rootBasis * charDelta * invRootBasis).Normalized();
        const Quat localQ = (restRootRot[idx].Inverse() * inWorld * restRootRot[idx]).Normalized();
        return localQ.ToUnityEuler();
    };

    // Build case-insensitive name lookup.
    std::unordered_map<std::string, size_t> nameToIdx;
    nameToIdx.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        nameToIdx[ToLowerAscii(bones[i].name)] = i;
    }

    int changed = 0;
    std::vector<bool> visited(n, false);

    for (size_t i = 0; i < n; ++i) {
        if (visited[i]) continue;
        const std::string swapped = SwapSideToken(bones[i].name);
        size_t partner = n;
        if (!swapped.empty()) {
            const auto it = nameToIdx.find(ToLowerAscii(swapped));
            if (it != nameToIdx.end() && it->second != i) {
                partner = it->second;
            }
        }

        if (partner < n) {
            visited[i] = true;
            visited[partner] = true;
            const BoneSide sideI = DetectBoneSide(bones[i].name);
            const Quat deltaI = localOffsetToCharDelta(i);
            const Quat deltaP = localOffsetToCharDelta(partner);

            if (mode == MirrorMode::Flip) {
                outOffsets[i] = charDeltaToLocalOffset(i, MirrorQuatAcrossPlane(deltaP, sagittalNormal));
                outOffsets[partner] = charDeltaToLocalOffset(partner, MirrorQuatAcrossPlane(deltaI, sagittalNormal));
                changed += 2;
            } else if (mode == MirrorMode::CopyLeftToRight) {
                const size_t src = (sideI == BoneSide::Left) ? i : partner;
                const size_t dst = (src == i) ? partner : i;
                const Quat srcDelta = (src == i) ? deltaI : deltaP;
                outOffsets[dst] = charDeltaToLocalOffset(dst, MirrorQuatAcrossPlane(srcDelta, sagittalNormal));
                ++changed;
            } else if (mode == MirrorMode::CopyRightToLeft) {
                const size_t src = (sideI == BoneSide::Right) ? i : partner;
                const size_t dst = (src == i) ? partner : i;
                const Quat srcDelta = (src == i) ? deltaI : deltaP;
                outOffsets[dst] = charDeltaToLocalOffset(dst, MirrorQuatAcrossPlane(srcDelta, sagittalNormal));
                ++changed;
            }
        } else if (mode == MirrorMode::Flip) {
            visited[i] = true;
            if (!bones[i].offsetEuler.IsNearZero()) {
                const Quat deltaI = localOffsetToCharDelta(i);
                outOffsets[i] = charDeltaToLocalOffset(i, MirrorQuatAcrossPlane(deltaI, sagittalNormal));
                ++changed;
            }
        }
    }

    if (mode == MirrorMode::Flip) {
        inoutRootOffset.x = -inoutRootOffset.x;
    }
    return changed;
}

}  // namespace betterpose

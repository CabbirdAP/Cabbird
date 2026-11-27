#include "mmd_retarget.hpp"
#include "skeleton_filter.hpp"

#include <nlohmann/json.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace betterpose::mmd {

namespace {

using json = nlohmann::json;

constexpr uint32_t kMaxRetargetFrames = 36000;  // 20 minutes @ 30fps safety cap

// ---- Shift-JIS -> UTF-8 Conversion (Windows CP932) --------------------------

std::string ShiftJisToUtf8(const char* raw, size_t maxLen) {
    size_t len = 0;
    while (len < maxLen && raw[len] != '\0') ++len;
    if (len == 0) return {};

    const int wideLen = MultiByteToWideChar(932, 0, raw, static_cast<int>(len), nullptr, 0);
    if (wideLen <= 0) return std::string(raw, len);

    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(932, 0, raw, static_cast<int>(len), wide.data(), wideLen);

    const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return std::string(raw, len);

    std::string out(static_cast<size_t>(utf8Len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, out.data(), utf8Len, nullptr, nullptr);
    return out;
}

// ---- Math Helpers -----------------------------------------------------------

double AngleBetweenDeg(const Vec3& a, const Vec3& b) noexcept {
    const double la = a.Length();
    const double lb = b.Length();
    if (!(la > 1e-9) || !(lb > 1e-9)) return 0.0;
    const double c = std::clamp(a.Dot(b) / (la * lb), -1.0, 1.0);
    return std::acos(c) * kRadToDeg;
}

Quat SwingRotation(const Vec3& fromDir, const Vec3& toDir) noexcept {
    return Quat::FromToRotation(fromDir, toDir);
}

// Extract twist component around `unitAxis` from quaternion `q`.
Quat TwistAboutAxis(const Quat& qIn, const Vec3& unitAxis) noexcept {
    const Quat q = qIn.Normalized();
    const double proj = q.x * unitAxis.x + q.y * unitAxis.y + q.z * unitAxis.z;
    const Quat t{q.w, unitAxis.x * proj, unitAxis.y * proj, unitAxis.z * proj};
    if (t.NormSq() < 1e-12) return Quat::Identity();
    return t.Normalized();
}

// Intrinsic XYZ Euler decomposition and reconstruction for PMX IK link angle limits.
// R = Rx(x) * Ry(y) * Rz(z)
Vec3 QuatToEulerXyzRad(const Quat& qIn) noexcept {
    const Quat q = qIn.Normalized();
    const double r00 = 1.0 - 2.0 * (q.y * q.y + q.z * q.z);
    const double r01 = 2.0 * (q.x * q.y - q.w * q.z);
    const double r02 = 2.0 * (q.x * q.z + q.w * q.y);
    const double r10 = 2.0 * (q.x * q.y + q.w * q.z);
    const double r11 = 1.0 - 2.0 * (q.x * q.x + q.z * q.z);
    const double r12 = 2.0 * (q.y * q.z - q.w * q.x);
    const double r22 = 1.0 - 2.0 * (q.x * q.x + q.y * q.y);

    const double sy = std::clamp(r02, -1.0, 1.0);
    const double y = std::asin(sy);
    double x = 0.0;
    double z = 0.0;
    if (std::fabs(sy) < 0.999995) {
        x = std::atan2(-r12, r22);
        z = std::atan2(-r01, r00);
    } else {
        x = std::atan2(r10, r11);
        z = 0.0;
    }
    return {x, y, z};
}

Quat EulerXyzRadToQuat(const Vec3& e) noexcept {
    const Quat qx = Quat::FromAxisAngle({1.0, 0.0, 0.0}, e.x);
    const Quat qy = Quat::FromAxisAngle({0.0, 1.0, 0.0}, e.y);
    const Quat qz = Quat::FromAxisAngle({0.0, 0.0, 1.0}, e.z);
    return (qx * qy * qz).Normalized();
}

// ---- Binary Reader Helper ---------------------------------------------------

struct BinaryReader {
    const uint8_t* data{};
    size_t size{};
    size_t pos{};

    bool Has(size_t n) const noexcept { return n <= size && pos <= size - n; }

    bool Skip(size_t n) noexcept {
        if (!Has(n)) return false;
        pos += n;
        return true;
    }

    template <typename T>
    bool Read(T& out) noexcept {
        if (!Has(sizeof(T))) return false;
        std::memcpy(&out, data + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }

    bool ReadBytes(void* dst, size_t n) noexcept {
        if (!Has(n)) return false;
        std::memcpy(dst, data + pos, n);
        pos += n;
        return true;
    }
};

// ---- Source-Side MMD IK Pose Solver -----------------------------------------

struct IkPose {
    const ReferenceSkeleton* ref{};
    std::vector<uint32_t> topoOrder;
    std::vector<Vec3> offset;
    std::vector<Vec3> pos;
    std::vector<Quat> rot;
    std::vector<Vec3> wpos;
    std::vector<Quat> wrot;
    std::unordered_map<std::string, size_t> byCanonicalName;

    bool Init(const ReferenceSkeleton& reference) {
        ref = &reference;
        const size_t n = reference.bones.size();
        if (n == 0) return false;

        offset.resize(n);
        pos.assign(n, Vec3{});
        rot.assign(n, Quat::Identity());
        wpos.assign(n, Vec3{});
        wrot.assign(n, Quat::Identity());
        byCanonicalName.clear();
        byCanonicalName.reserve(n);

        for (size_t i = 0; i < n; ++i) {
            const auto& b = reference.bones[i];
            byCanonicalName.emplace(CanonicalMmdBoneName(b.name), i);
            if (b.parent >= 0 && static_cast<size_t>(b.parent) < n) {
                offset[i] = b.position - reference.bones[static_cast<size_t>(b.parent)].position;
            } else {
                offset[i] = b.position;
            }
        }

        // Compute parent-before-child update order.
        topoOrder.clear();
        topoOrder.reserve(n);
        std::vector<uint8_t> state(n, 0);
        auto visit = [&](auto&& self, size_t idx) -> void {
            if (state[idx] != 0) return;
            state[idx] = 1;
            const int32_t p = reference.bones[idx].parent;
            if (p >= 0 && static_cast<size_t>(p) < n && state[static_cast<size_t>(p)] == 0) {
                self(self, static_cast<size_t>(p));
            }
            state[idx] = 2;
            topoOrder.push_back(static_cast<uint32_t>(idx));
        };
        for (size_t i = 0; i < n; ++i) visit(visit, i);
        Update();
        return true;
    }

    void Update() noexcept {
        const size_t n = ref->bones.size();
        for (uint32_t idx : topoOrder) {
            const auto& b = ref->bones[idx];
            const Vec3 localP = offset[idx] + pos[idx];
            if (b.parent >= 0 && static_cast<size_t>(b.parent) < n) {
                const size_t p = static_cast<size_t>(b.parent);
                wrot[idx] = (wrot[p] * rot[idx]).Normalized();
                wpos[idx] = wpos[p] + wrot[p].Rotate(localP);
            } else {
                wrot[idx] = rot[idx].Normalized();
                wpos[idx] = localP;
            }
        }
    }

    void SetFrame(const std::vector<const VmdBoneTrack*>& trackByBone, double frame) noexcept {
        const size_t n = ref->bones.size();
        for (size_t i = 0; i < n; ++i) {
            rot[i] = Quat::Identity();
            pos[i] = {};
            if (i < trackByBone.size() && trackByBone[i] != nullptr) {
                SampleBoneTrack(*trackByBone[i], frame, rot[i], pos[i]);
            }
        }
        Update();
    }

    void Solve(const std::unordered_set<std::string>& disabledIkNames, bool fkFallbackAvailable) {
        const size_t n = ref->bones.size();
        const auto isKneeBone = [](const std::string& boneName) {
            return boneName == "左ひざ" || boneName == "右ひざ" || // i18n-data
                   boneName == "左ひざD" || boneName == "右ひざD"; // i18n-data
        };

        for (size_t ei = 0; ei < n; ++ei) {
            const auto& effBone = ref->bones[ei];
            if (!effBone.ik.valid || effBone.ik.links.empty()) continue;
            if (effBone.ik.target >= n) continue;

            // Only solve leg/toe IK chains (skip hair/necktie decorative IK chains).
            const std::string canonName = CanonicalMmdBoneName(effBone.name);
            const bool isLegIk =
                canonName == CanonicalMmdBoneName("左足ＩＫ") || canonName == CanonicalMmdBoneName("右足ＩＫ") || // i18n-data
                canonName == CanonicalMmdBoneName("左つま先ＩＫ") || canonName == CanonicalMmdBoneName("右つま先ＩＫ"); // i18n-data
            if (!isLegIk) continue;
            if (disabledIkNames.count(canonName) != 0) continue;

            // Save pre-IK rotations for optional FK fallback guard.
            std::vector<Quat> savedLinkRots;
            savedLinkRots.reserve(effBone.ik.links.size());
            for (const auto& link : effBone.ik.links) {
                savedLinkRots.push_back(link.bone < n ? rot[link.bone] : Quat::Identity());
            }

            const Vec3 goal = wpos[ei];
            const uint32_t targetIdx = effBone.ik.target;
            const double beforeGap = (goal - wpos[targetIdx]).Length();
            const uint32_t maxLoops = std::min<uint32_t>(effBone.ik.loop, 80u);
            const double stepLimit = (effBone.ik.limitRadian > 1e-6) ? effBone.ik.limitRadian : 0.5;

            for (uint32_t iter = 0; iter < maxLoops; ++iter) {
                if ((goal - wpos[targetIdx]).LengthSq() < 1e-6) break;
                for (const auto& link : effBone.ik.links) {
                    const uint32_t li = link.bone;
                    if (li >= n) continue;

                    const Vec3 linkPos = wpos[li];
                    const Vec3 toEff = (wpos[targetIdx] - linkPos).Normalized();
                    const Vec3 toGoal = (goal - linkPos).Normalized();
                    if (toEff.LengthSq() < 0.5 || toGoal.LengthSq() < 0.5) continue;

                    const double dot = std::clamp(toEff.Dot(toGoal), -1.0, 1.0);
                    double angle = std::acos(dot);
                    if (!(angle > 1e-5)) continue;
                    angle = std::min(angle, stepLimit);

                    Vec3 axisWorld = toEff.Cross(toGoal);
                    if (axisWorld.LengthSq() < 1e-10) {
                        // Near 180 deg singularity: nudge knee along MMD -X (backward bend).
                        if (isKneeBone(ref->bones[li].name)) {
                            axisWorld = wrot[li].Rotate({-1.0, 0.0, 0.0});
                        } else {
                            continue;
                        }
                    } else {
                        axisWorld = axisWorld.Normalized();
                    }

                    const int32_t p = ref->bones[li].parent;
                    const Quat parentWrot = (p >= 0 && static_cast<size_t>(p) < n)
                                                ? wrot[static_cast<size_t>(p)]
                                                : Quat::Identity();
                    const Vec3 axisLocal = parentWrot.Conjugate().Rotate(axisWorld).Normalized();
                    const Quat deltaLocal = Quat::FromAxisAngle(axisLocal, angle);
                    rot[li] = (deltaLocal * rot[li]).Normalized();

                    if (link.hasLimit) {
                        Vec3 e = QuatToEulerXyzRad(rot[li]);
                        e.x = std::clamp(e.x, link.minRad.x, link.maxRad.x);
                        e.y = std::clamp(e.y, link.minRad.y, link.maxRad.y);
                        e.z = std::clamp(e.z, link.minRad.z, link.maxRad.z);
                        rot[li] = EulerXyzRadToQuat(e);
                    } else if (isKneeBone(ref->bones[li].name)) {
                        // Standard PMD/PMX knee hinge bends strictly backward (negative X in MMD).
                        Vec3 e = QuatToEulerXyzRad(rot[li]);
                        e.x = std::clamp(e.x, -kPi, -0.00872665);
                        e.y = 0.0;
                        e.z = 0.0;
                        rot[li] = EulerXyzRadToQuat(e);
                    }
                    Update();
                }
            }

            // Guard: if IK failed badly while complete leg FK is available, revert to FK.
            const double afterGap = (goal - wpos[targetIdx]).Length();
            const double residualLimit = std::max(0.5, beforeGap * 0.35);
            if (fkFallbackAvailable && beforeGap > 0.5 && afterGap > residualLimit) {
                for (size_t k = 0; k < effBone.ik.links.size(); ++k) {
                    const uint32_t li = effBone.ik.links[k].bone;
                    if (li < n) rot[li] = savedLinkRots[k];
                }
                Update();
            }
        }
    }
};

// ---- Universal Unity Target Skeleton Role Resolver --------------------------

enum class TargetRole : int {
    Root = 0,
    Pelvis,
    Spine,
    Spine1,
    Spine2,
    Spine3,
    Neck,
    Head,
    EyeL,
    EyeR,

    ClavicleL,
    UpperArmL,
    ForearmL,
    HandL,
    UpperArmTwistL,
    ForearmTwistL,

    ClavicleR,
    UpperArmR,
    ForearmR,
    HandR,
    UpperArmTwistR,
    ForearmTwistR,

    ThighL,
    CalfL,
    FootL,
    ToeL,

    ThighR,
    CalfR,
    FootR,
    ToeR,

    // Fingers: 5 fingers x 4 joints (0..3) for L and R
    ThumbL0, ThumbL1, ThumbL2, ThumbL3,
    IndexL0, IndexL1, IndexL2, IndexL3,
    MiddleL0, MiddleL1, MiddleL2, MiddleL3,
    RingL0, RingL1, RingL2, RingL3,
    PinkyL0, PinkyL1, PinkyL2, PinkyL3,

    ThumbR0, ThumbR1, ThumbR2, ThumbR3,
    IndexR0, IndexR1, IndexR2, IndexR3,
    MiddleR0, MiddleR1, MiddleR2, MiddleR3,
    RingR0, RingR1, RingR2, RingR3,
    PinkyR0, PinkyR1, PinkyR2, PinkyR3,

    Count
};

TargetRole FingerRole(int side, int finger, int joint) noexcept {
    const int base = (side == 0) ? static_cast<int>(TargetRole::ThumbL0)
                                 : static_cast<int>(TargetRole::ThumbR0);
    return static_cast<TargetRole>(base + finger * 4 + joint);
}

struct ResolvedTargetRig {
    std::array<int, static_cast<size_t>(TargetRole::Count)> roleBone{};
    std::vector<Vec3> restPos;   // character GameObject space
    std::vector<Quat> restRot;   // character GameObject space
    Quat mmdToChar{0.0, 0.0, 1.0, 0.0};  // default Ry(180 deg)

    ResolvedTargetRig() { roleBone.fill(-1); }

    int BoneOf(TargetRole r) const noexcept { return roleBone[static_cast<size_t>(r)]; }
    void SetRole(TargetRole r, int idx) noexcept {
        if (idx >= 0) roleBone[static_cast<size_t>(r)] = idx;
    }
};

bool IsNonStructuralBone(std::string_view name) {
    const std::string low = ToLowerAscii(name);
    static constexpr std::string_view kSkip[]{
        "hair", "bang", "ahoge", "ponytail", "twintail", "braid", "kami",
        "cloth", "skirt", "dress", "cape", "cloak", "coat", "ribbon", "bow_",
        "scarf", "sleeve", "cuff", "hem", "belt", "sash", "apron",
        "qun", "xiu", "piaodai", "sode",
        "tail", "wing", "feather", "breast", "bust", "boob", "oppai", "mune", "xiong",
        "chest_l", "chest_r", "l_chest", "r_chest", "shoulderpad",
        "weapon", "sword", "gun", "prop", "attach", "socket", "mount", "fx_", "eff_",
        "spring", "jiggle", "dynamic", "magica", "kawaii", "pendulum", "physics",
        "camera", "ik_", "_ik", "pole", "collider", "hitbox", "dummy", "nub", "_end", "point_",
        "eyelid", "eyelash", "eyebrow", "brow", "lash", "lid"
    };
    for (std::string_view tok : kSkip) {
        if (ContainsAscii(low, tok)) return true;
    }
    return false;
}

bool HasDelimitedWord(std::string_view low, std::string_view word) {
    size_t pos = 0;
    while ((pos = low.find(word, pos)) != std::string_view::npos) {
        const bool startOk = (pos == 0) || !std::isalpha(static_cast<unsigned char>(low[pos - 1]));
        const size_t end = pos + word.size();
        const bool endOk = (end == low.size()) || !std::isalpha(static_cast<unsigned char>(low[end]));
        if (startOk && endOk) return true;
        pos += 1;
    }
    return false;
}

// Compute topological order for target skeleton (handles any parent index ordering).
std::vector<size_t> BuildTargetTopoOrder(const TargetSkeletonInput& target) {
    const size_t n = target.bones.size();
    std::vector<size_t> order;
    order.reserve(n);
    std::vector<uint8_t> state(n, 0);
    auto visit = [&](auto&& self, size_t i) -> void {
        if (state[i] != 0) return;
        state[i] = 1;
        const int p = target.bones[i].parent;
        if (p >= 0 && static_cast<size_t>(p) < n && state[static_cast<size_t>(p)] == 0) {
            self(self, static_cast<size_t>(p));
        }
        state[i] = 2;
        order.push_back(i);
    };
    for (size_t i = 0; i < n; ++i) visit(visit, i);
    return order;
}

ResolvedTargetRig ResolveTargetRig(const TargetSkeletonInput& target) {
    ResolvedTargetRig rig;
    const size_t n = target.bones.size();
    if (n == 0) return rig;

    rig.restPos.assign(n, Vec3{});
    rig.restRot.assign(n, Quat::Identity());
    std::vector<Mat4> restMat(n, Mat4::Identity());

    const auto topo = BuildTargetTopoOrder(target);
    const Mat4 rootParentMat = Mat4::FromTrs({}, target.goToRootParentRot, {1.0, 1.0, 1.0});
    for (size_t i : topo) {
        const auto& b = target.bones[i];
        const Vec3 sc = (b.localScale.IsFinite() && b.localScale.x > 1e-5 &&
                         b.localScale.y > 1e-5 && b.localScale.z > 1e-5)
                            ? b.localScale
                            : Vec3{1.0, 1.0, 1.0};
        const Mat4 localM = Mat4::FromTrs(b.localPos, b.restLocalRot, sc);
        if (b.parent >= 0 && static_cast<size_t>(b.parent) < n) {
            const size_t p = static_cast<size_t>(b.parent);
            restMat[i] = Mat4::Multiply(restMat[p], localM);
            rig.restRot[i] = (rig.restRot[p] * b.restLocalRot).Normalized();
        } else {
            restMat[i] = Mat4::Multiply(rootParentMat, localM);
            rig.restRot[i] = (target.goToRootParentRot * b.restLocalRot).Normalized();
        }
        rig.restPos[i] = restMat[i].ExtractPosition();
    }

    std::vector<uint32_t> descendantCount(n, 0U);
    for (size_t i = 0; i < n; ++i) {
        int cur = target.bones[i].parent;
        for (int guard = 0; guard < 64 && cur >= 0 && static_cast<size_t>(cur) < n; ++guard) {
            ++descendantCount[static_cast<size_t>(cur)];
            cur = target.bones[static_cast<size_t>(cur)].parent;
        }
    }

    auto isDescendantOf = [&](int node, int ancestor) -> bool {
        if (node < 0 || ancestor < 0) return false;
        int cur = node;
        for (int guard = 0; guard < 64 && cur >= 0 && static_cast<size_t>(cur) < n; ++guard) {
            if (cur == ancestor) return true;
            cur = target.bones[static_cast<size_t>(cur)].parent;
        }
        return false;
    };

    auto climbNonTwistParent = [&](int idx) -> int {
        if (idx < 0 || static_cast<size_t>(idx) >= n) return -1;
        int cur = target.bones[static_cast<size_t>(idx)].parent;
        for (int guard = 0; guard < 8 && cur >= 0 && static_cast<size_t>(cur) < n; ++guard) {
            if (!IsTwistOrHelperBone(target.bones[static_cast<size_t>(cur)].name)) return cur;
            cur = target.bones[static_cast<size_t>(cur)].parent;
        }
        return -1;
    };

    // Pass 1: match primary semantic bone names (ignoring secondary cloth/hair/props).
    struct Candidate {
        int index{-1};
        int score{0};
    };
    std::array<Candidate, static_cast<size_t>(TargetRole::Count)> best{};
    auto consider = [&](TargetRole role, int idx, int score) {
        auto& slot = best[static_cast<size_t>(role)];
        if (score > slot.score) {
            slot.index = idx;
            slot.score = score;
        }
    };

    for (size_t i = 0; i < n; ++i) {
        const std::string& raw = target.bones[i].name;
        const std::string low = ToLowerAscii(raw);
        if (IsNonStructuralBone(raw)) continue;

        const bool isTwist = IsTwistOrHelperBone(raw);
        const BoneSide side = DetectBoneSide(raw);
        const bool isCorrectiveAdj = ContainsAscii(low, "butt") || ContainsAscii(low, "glute") ||
                                     ContainsAscii(low, "adjust") || ContainsAscii(low, "_adj") ||
                                     ContainsAscii(low, "adj_");

        if (!isTwist && !isCorrectiveAdj) {
            // Pelvis / Hips (must be center so Hip_L / Hip_R helpers never hijack Pelvis).
            if (side == BoneSide::Center &&
                (ContainsAscii(low, "pelvis") || HasDelimitedWord(low, "hips") || HasDelimitedWord(low, "hip"))) {
                int sc = ContainsAscii(low, "pelvis") ? 100 : 90;
                if (ContainsAscii(low, "bip001")) sc += 20;
                if (descendantCount[i] >= 10) sc += 20;
                consider(TargetRole::Pelvis, static_cast<int>(i), sc);
            }
            // Neck / Head / Eyes
            if (HasDelimitedWord(low, "neck") || ContainsAscii(low, "neck")) {
                int sc = HasDelimitedWord(low, "neck") ? 100 : 80;
                if (descendantCount[i] >= 1) sc += 15;
                consider(TargetRole::Neck, static_cast<int>(i), sc);
            }
            if ((HasDelimitedWord(low, "head") || ContainsAscii(low, "head")) &&
                !ContainsAscii(low, "headtop") && !ContainsAscii(low, "head_end")) {
                int sc = HasDelimitedWord(low, "head") ? 100 : 70;
                if (ContainsAscii(low, "bip001")) sc += 20;
                consider(TargetRole::Head, static_cast<int>(i), sc);
            }
            if ((HasDelimitedWord(low, "eye") || ContainsAscii(low, "eyeball")) && side != BoneSide::Center) {
                consider(side == BoneSide::Left ? TargetRole::EyeL : TargetRole::EyeR,
                         static_cast<int>(i), 80);
            }

            // Arms & Legs
            if (side != BoneSide::Center) {
                const bool left = (side == BoneSide::Left);
                const bool isFinger = ContainsAscii(low, "finger") || ContainsAscii(low, "thumb") ||
                                      ContainsAscii(low, "index") || ContainsAscii(low, "middle") ||
                                      ContainsAscii(low, "ring") || ContainsAscii(low, "pinky") ||
                                      ContainsAscii(low, "little");
                if (!isFinger) {
                    const int nonLeafBonus = (descendantCount[i] >= 1) ? 25 : 0;
                    if (ContainsAscii(low, "clavicle") || ContainsAscii(low, "collar") ||
                        (ContainsAscii(low, "shoulder") && !ContainsAscii(low, "pad"))) {
                        int sc = ContainsAscii(low, "clavicle") ? 100 : (ContainsAscii(low, "collar") ? 90 : 75);
                        consider(left ? TargetRole::ClavicleL : TargetRole::ClavicleR,
                                 static_cast<int>(i), sc + nonLeafBonus);
                    }
                    if (ContainsAscii(low, "upperarm") || ContainsAscii(low, "uparm") ||
                        HasDelimitedWord(low, "arm")) {
                        int sc = ContainsAscii(low, "upperarm") ? 100 : (ContainsAscii(low, "uparm") ? 95 : 80);
                        consider(left ? TargetRole::UpperArmL : TargetRole::UpperArmR,
                                 static_cast<int>(i), sc + nonLeafBonus);
                    }
                    if (ContainsAscii(low, "forearm") || ContainsAscii(low, "lowerarm") ||
                        ContainsAscii(low, "elbow")) {
                        int sc = ContainsAscii(low, "forearm") ? 100 : (ContainsAscii(low, "lowerarm") ? 90 : 70);
                        consider(left ? TargetRole::ForearmL : TargetRole::ForearmR,
                                 static_cast<int>(i), sc + nonLeafBonus);
                    }
                    if (HasDelimitedWord(low, "hand") || ContainsAscii(low, "wrist") ||
                        (ContainsAscii(low, "hand") && !ContainsAscii(low, "handle"))) {
                        int sc = HasDelimitedWord(low, "hand") ? 100 : 85;
                        if (descendantCount[i] >= 2) sc += 40;
                        consider(left ? TargetRole::HandL : TargetRole::HandR, static_cast<int>(i), sc);
                    }
                    // Legs
                    if (ContainsAscii(low, "thigh") || ContainsAscii(low, "upperleg") ||
                        ContainsAscii(low, "upleg")) {
                        int sc = ContainsAscii(low, "thigh") ? 100 : 90;
                        consider(left ? TargetRole::ThighL : TargetRole::ThighR,
                                 static_cast<int>(i), sc + nonLeafBonus);
                    }
                    if (ContainsAscii(low, "calf") || ContainsAscii(low, "lowerleg") ||
                        ContainsAscii(low, "shin") || ContainsAscii(low, "knee") ||
                        (HasDelimitedWord(low, "leg") && !ContainsAscii(low, "upper") && !ContainsAscii(low, "up"))) {
                        int sc = ContainsAscii(low, "calf") ? 100
                                 : (ContainsAscii(low, "lowerleg") || ContainsAscii(low, "shin") ? 90
                                    : (ContainsAscii(low, "knee") ? 70 : 85));
                        consider(left ? TargetRole::CalfL : TargetRole::CalfR,
                                 static_cast<int>(i), sc + nonLeafBonus);
                    }
                    if ((HasDelimitedWord(low, "foot") || ContainsAscii(low, "ankle") || ContainsAscii(low, "foot")) &&
                        !ContainsAscii(low, "toe")) {
                        int sc = HasDelimitedWord(low, "foot") ? 100 : 85;
                        if (descendantCount[i] >= 1) sc += 15;
                        consider(left ? TargetRole::FootL : TargetRole::FootR, static_cast<int>(i), sc);
                    }
                    if (ContainsAscii(low, "toe") || ContainsAscii(low, "ball")) {
                        int sc = ContainsAscii(low, "toe0") ? 100 : (ContainsAscii(low, "toe") ? 90 : 75);
                        consider(left ? TargetRole::ToeL : TargetRole::ToeR, static_cast<int>(i), sc);
                    }
                }
            }
        } else if (isTwist && side != BoneSide::Center) {
            const bool left = (side == BoneSide::Left);
            if (ContainsAscii(low, "upperarm") || ContainsAscii(low, "uparm") || ContainsAscii(low, "armtwist")) {
                consider(left ? TargetRole::UpperArmTwistL : TargetRole::UpperArmTwistR, static_cast<int>(i), 90);
            } else if (ContainsAscii(low, "forearm") || ContainsAscii(low, "foretwist") || ContainsAscii(low, "wristtwist")) {
                consider(left ? TargetRole::ForearmTwistL : TargetRole::ForearmTwistR, static_cast<int>(i), 90);
            }
        }
    }

    for (size_t r = 0; r < static_cast<size_t>(TargetRole::Count); ++r) {
        rig.roleBone[r] = best[r].index;
    }

    // Pass 2: hierarchy-verified limb chains. Ensures leaf corrective bones (e.g. Knee_L, Elbow_L,
    // Hip_L, Shoulder_L) NEVER hijack primary chain slots from the actual ancestors of Hand / Foot.
    for (int side = 0; side < 2; ++side) {
        const TargetRole rHand = side == 0 ? TargetRole::HandL : TargetRole::HandR;
        const TargetRole rFore = side == 0 ? TargetRole::ForearmL : TargetRole::ForearmR;
        const TargetRole rUp   = side == 0 ? TargetRole::UpperArmL : TargetRole::UpperArmR;
        const TargetRole rClav = side == 0 ? TargetRole::ClavicleL : TargetRole::ClavicleR;
        const int handIdx = rig.BoneOf(rHand);
        if (handIdx >= 0) {
            if (rig.BoneOf(rFore) < 0 || !isDescendantOf(handIdx, rig.BoneOf(rFore))) {
                rig.SetRole(rFore, climbNonTwistParent(handIdx));
            }
            const int foreIdx = rig.BoneOf(rFore);
            if (foreIdx >= 0 && (rig.BoneOf(rUp) < 0 || !isDescendantOf(foreIdx, rig.BoneOf(rUp)))) {
                rig.SetRole(rUp, climbNonTwistParent(foreIdx));
            }
            const int upIdx = rig.BoneOf(rUp);
            if (upIdx >= 0 && (rig.BoneOf(rClav) < 0 || !isDescendantOf(upIdx, rig.BoneOf(rClav)))) {
                const int cand = climbNonTwistParent(upIdx);
                if (cand >= 0 && cand != rig.BoneOf(TargetRole::Pelvis)) {
                    const std::string clow = ToLowerAscii(target.bones[static_cast<size_t>(cand)].name);
                    if (!ContainsAscii(clow, "spine") && !ContainsAscii(clow, "chest")) {
                        rig.SetRole(rClav, cand);
                    }
                }
            }
        }

        const TargetRole rFoot  = side == 0 ? TargetRole::FootL : TargetRole::FootR;
        const TargetRole rCalf  = side == 0 ? TargetRole::CalfL : TargetRole::CalfR;
        const TargetRole rThigh = side == 0 ? TargetRole::ThighL : TargetRole::ThighR;
        const int footIdx = rig.BoneOf(rFoot);
        if (footIdx >= 0) {
            if (rig.BoneOf(rCalf) < 0 || !isDescendantOf(footIdx, rig.BoneOf(rCalf))) {
                rig.SetRole(rCalf, climbNonTwistParent(footIdx));
            }
            const int calfIdx = rig.BoneOf(rCalf);
            if (calfIdx >= 0 && (rig.BoneOf(rThigh) < 0 || !isDescendantOf(calfIdx, rig.BoneOf(rThigh)))) {
                rig.SetRole(rThigh, climbNonTwistParent(calfIdx));
            }
        }
    }
    // Verify Pelvis is an ancestor of both thighs (handles rigs where a center helper had "hip" in its name).
    {
        const int thighL = rig.BoneOf(TargetRole::ThighL);
        const int thighR = rig.BoneOf(TargetRole::ThighR);
        const int pel = rig.BoneOf(TargetRole::Pelvis);
        if (thighL >= 0 && thighR >= 0 &&
            (pel < 0 || !isDescendantOf(thighL, pel) || !isDescendantOf(thighR, pel))) {
            const int pL = climbNonTwistParent(thighL);
            const int pR = climbNonTwistParent(thighR);
            if (pL >= 0 && pL == pR) {
                rig.SetRole(TargetRole::Pelvis, pL);
            }
        } else if (pel < 0) {
            const int anyThigh = thighL >= 0 ? thighL : thighR;
            if (anyThigh >= 0) rig.SetRole(TargetRole::Pelvis, climbNonTwistParent(anyThigh));
        }
    }

    // Pass 3: Root bone resolution (prefer Bip001/Root parent of Pelvis, else target.rootBoneIndex, else Pelvis).
    const int pelvisIdx = rig.BoneOf(TargetRole::Pelvis);
    if (pelvisIdx >= 0) {
        const int pelvisParent = target.bones[static_cast<size_t>(pelvisIdx)].parent;
        if (pelvisParent >= 0) {
            rig.SetRole(TargetRole::Root, pelvisParent);
        } else {
            rig.SetRole(TargetRole::Root, pelvisIdx);
        }
    } else if (target.rootBoneIndex >= 0 && static_cast<size_t>(target.rootBoneIndex) < n) {
        rig.SetRole(TargetRole::Root, target.rootBoneIndex);
        rig.SetRole(TargetRole::Pelvis, target.rootBoneIndex);
    } else {
        rig.SetRole(TargetRole::Root, 0);
        rig.SetRole(TargetRole::Pelvis, 0);
    }

    // Pass 4: Spine chain resolution (ordered from Pelvis upward toward Neck / Head!).
    {
        const int topAnchor = (rig.BoneOf(TargetRole::Neck) >= 0)
                                  ? rig.BoneOf(TargetRole::Neck)
                                  : rig.BoneOf(TargetRole::Head);
        const int pel = rig.BoneOf(TargetRole::Pelvis);
        std::vector<int> spineChain;
        if (topAnchor >= 0 && pel >= 0) {
            int cur = target.bones[static_cast<size_t>(topAnchor)].parent;
            for (int guard = 0; guard < 12 && cur >= 0 && cur != pel; ++guard) {
                if (!IsTwistOrHelperBone(target.bones[static_cast<size_t>(cur)].name)) {
                    spineChain.push_back(cur);
                }
                cur = target.bones[static_cast<size_t>(cur)].parent;
            }
            std::reverse(spineChain.begin(), spineChain.end());
        }
        if (spineChain.empty()) {
            // Fallback: collect spine/chest named bones sorted topologically.
            for (size_t i : topo) {
                if (static_cast<int>(i) == pel || static_cast<int>(i) == rig.BoneOf(TargetRole::Root)) continue;
                const std::string low = ToLowerAscii(target.bones[i].name);
                if (IsNonStructuralBone(target.bones[i].name) || IsTwistOrHelperBone(target.bones[i].name)) continue;
                if (ContainsAscii(low, "spine") || ContainsAscii(low, "chest") || ContainsAscii(low, "waist")) {
                    spineChain.push_back(static_cast<int>(i));
                }
            }
        }
        const TargetRole spineRoles[4]{TargetRole::Spine, TargetRole::Spine1, TargetRole::Spine2, TargetRole::Spine3};
        for (size_t k = 0; k < spineChain.size() && k < 4; ++k) {
            rig.SetRole(spineRoles[k], spineChain[k]);
        }
    }

    // Pass 5: Finger chains (supports both 3dsMax Bip001 Finger0..4 and named Thumb/Index/Middle/Ring/Pinky).
    for (int side = 0; side < 2; ++side) {
        const int handIdx = rig.BoneOf(side == 0 ? TargetRole::HandL : TargetRole::HandR);
        if (handIdx < 0) continue;

        static constexpr std::array<const char*, 5> kBipFinger{"finger0", "finger1", "finger2", "finger3", "finger4"};
        static constexpr std::array<const char*, 5> kNamedFinger{"thumb", "index", "middle", "ring", "pinky"};

        for (int finger = 0; finger < 5; ++finger) {
            std::vector<int> chain;
            for (size_t i : topo) {
                if (static_cast<int>(i) == handIdx || !isDescendantOf(static_cast<int>(i), handIdx)) continue;
                const std::string& raw = target.bones[i].name;
                if (IsNonStructuralBone(raw) || IsTwistOrHelperBone(raw)) continue;
                const std::string low = ToLowerAscii(raw);

                bool matched = false;
                if (ContainsAscii(low, kNamedFinger[finger]) ||
                    (finger == 4 && ContainsAscii(low, "little"))) {
                    matched = true;
                } else {
                    const size_t pos = low.find(kBipFinger[finger]);
                    if (pos != std::string::npos) {
                        const size_t after = pos + std::strlen(kBipFinger[finger]);
                        // Match "finger0", "finger01", "finger02", "finger03", "finger0_1" (but not "finger10" for finger1).
                        if (after == low.size() || !std::isdigit(static_cast<unsigned char>(low[after])) ||
                            (after + 1 == low.size() && low[after] >= '1' && low[after] <= '3') ||
                            (after + 1 < low.size() && !std::isdigit(static_cast<unsigned char>(low[after + 1])))) {
                            matched = true;
                        }
                    }
                }
                if (matched) chain.push_back(static_cast<int>(i));
            }
            for (size_t j = 0; j < chain.size() && j < 4; ++j) {
                rig.SetRole(FingerRole(side, finger, static_cast<int>(j)), chain[j]);
            }
        }
    }

    // Pass 6: Auto-detect MMD -> Character basis from target rest geometry.
    // Standard Unity character space: Left = (-1,0,0), Up = (0,1,0), Back = (-Front) = (0,0,-1) -> Ry(180 deg).
    const Quat kDefaultMmdToUnity{0.0, 0.0, 1.0, 0.0};  // Ry(180 deg)
    rig.mmdToChar = kDefaultMmdToUnity;

    Vec3 charLeft{-1.0, 0.0, 0.0};
    bool haveLeft = false;
    if (rig.BoneOf(TargetRole::UpperArmL) >= 0 && rig.BoneOf(TargetRole::UpperArmR) >= 0) {
        const Vec3 d = rig.restPos[static_cast<size_t>(rig.BoneOf(TargetRole::UpperArmL))] -
                       rig.restPos[static_cast<size_t>(rig.BoneOf(TargetRole::UpperArmR))];
        if (d.LengthSq() > 1e-4) {
            charLeft = d.Normalized();
            haveLeft = true;
        }
    } else if (rig.BoneOf(TargetRole::ThighL) >= 0 && rig.BoneOf(TargetRole::ThighR) >= 0) {
        const Vec3 d = rig.restPos[static_cast<size_t>(rig.BoneOf(TargetRole::ThighL))] -
                       rig.restPos[static_cast<size_t>(rig.BoneOf(TargetRole::ThighR))];
        if (d.LengthSq() > 1e-4) {
            charLeft = d.Normalized();
            haveLeft = true;
        }
    }

    Vec3 charUp{0.0, 1.0, 0.0};
    bool haveUp = false;
    if (rig.BoneOf(TargetRole::Head) >= 0 && rig.BoneOf(TargetRole::Pelvis) >= 0) {
        const Vec3 d = rig.restPos[static_cast<size_t>(rig.BoneOf(TargetRole::Head))] -
                       rig.restPos[static_cast<size_t>(rig.BoneOf(TargetRole::Pelvis))];
        if (d.LengthSq() > 1e-4) {
            charUp = d.Normalized();
            haveUp = true;
        }
    }

    if (haveLeft && haveUp) {
        // Orthonormalize [charLeft, charUp, charBack = charLeft x charUp].
        // Verify: for Unity Left=(-1,0,0), Up=(0,1,0) -> Left x Up = (0,0,-1) = Back = -Front!
        // And MMD (+X=Left, +Y=Up, +Z=Back) maps directly to [charLeft, charUp, charBack] with det = +1.
        charLeft = (charLeft - charUp * charLeft.Dot(charUp)).Normalized({-1.0, 0.0, 0.0});
        const Vec3 charBack = charLeft.Cross(charUp).Normalized({0.0, 0.0, -1.0});
        const Quat measured = Quat::FromBasis(charLeft, charUp, charBack);
        const double dotDef = std::fabs(measured.Dot(kDefaultMmdToUnity));
        // If within 18 deg of canonical Unity Ry(180 deg), lock to exact Ry(180 deg) to avoid tilt drift.
        if (dotDef >= std::cos(9.0 * kDegToRad)) {
            rig.mmdToChar = kDefaultMmdToUnity;
        } else {
            rig.mmdToChar = measured;
        }
    }

    return rig;
}

const ReferenceBone* FindRefBone(const ReferenceSkeleton& ref, std::string_view name) {
    const std::string canon = CanonicalMmdBoneName(name);
    for (const auto& b : ref.bones) {
        if (CanonicalMmdBoneName(b.name) == canon) return &b;
    }
    return nullptr;
}

}  // namespace

// ---- Public API Implementations ---------------------------------------------

const char* ReferencePresetFilename(int presetIndex) noexcept {
    switch (presetIndex) {
        case 1:  return "reference-miku-unity.json";
        case 2:  return "reference-endfield-standard-a.json";
        default: return "reference-pmx.json";
    }
}

std::string CanonicalMmdBoneName(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    size_t i = 0;
    while (i < name.size()) {
        const unsigned char b0 = static_cast<unsigned char>(name[i]);
        // Normalize fullwidth ASCII (U+FF01..U+FF5E: UTF-8 EF BC 81 .. EF BD 9E) to ASCII.
        if (b0 == 0xEF && i + 2 < name.size()) {
            const unsigned char b1 = static_cast<unsigned char>(name[i + 1]);
            const unsigned char b2 = static_cast<unsigned char>(name[i + 2]);
            if (b1 == 0xBC && b2 >= 0x81 && b2 <= 0xBF) {
                out.push_back(static_cast<char>(b2 - 0x60));
                i += 3;
                continue;
            }
            if (b1 == 0xBD && b2 >= 0x80 && b2 <= 0x9E) {
                out.push_back(static_cast<char>(b2 - 0x20));
                i += 3;
                continue;
            }
        }
        out.push_back(static_cast<char>(b0));
        ++i;
    }
    // Trim leading/trailing ASCII whitespace.
    size_t start = 0;
    while (start < out.size() && std::isspace(static_cast<unsigned char>(out[start]))) ++start;
    size_t end = out.size();
    while (end > start && std::isspace(static_cast<unsigned char>(out[end - 1]))) --end;
    return out.substr(start, end - start);
}

double SampleVmdCurve(const VmdBezier& curve, double t) noexcept {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    const double x1 = static_cast<double>(curve.x1) / 127.0;
    const double y1 = static_cast<double>(curve.y1) / 127.0;
    const double x2 = static_cast<double>(curve.x2) / 127.0;
    const double y2 = static_cast<double>(curve.y2) / 127.0;
    if (std::fabs(x1 - y1) < 1e-6 && std::fabs(x2 - y2) < 1e-6) return t;

    double low = 0.0;
    double high = 1.0;
    double u = t;
    for (int iter = 0; iter < 16; ++iter) {
        u = 0.5 * (low + high);
        const double inv = 1.0 - u;
        const double x = 3.0 * inv * inv * u * x1 + 3.0 * inv * u * u * x2 + u * u * u;
        if (x < t) low = u;
        else high = u;
    }
    const double inv = 1.0 - u;
    return 3.0 * inv * inv * u * y1 + 3.0 * inv * u * u * y2 + u * u * u;
}

Quat SlerpQuat(const Quat& aIn, const Quat& bIn, double t) noexcept {
    const Quat a = aIn.Normalized();
    Quat b = bIn.Normalized();
    double d = a.Dot(b);
    if (d < 0.0) {
        d = -d;
        b = {-b.w, -b.x, -b.y, -b.z};
    }
    if (d > 0.9995) {
        return Quat{
            a.w + (b.w - a.w) * t,
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t
        }.Normalized();
    }
    const double theta = std::acos(std::clamp(d, -1.0, 1.0));
    const double sinT = std::sin(theta);
    const double w0 = std::sin((1.0 - t) * theta) / sinT;
    const double w1 = std::sin(t * theta) / sinT;
    return Quat{
        a.w * w0 + b.w * w1,
        a.x * w0 + b.x * w1,
        a.y * w0 + b.y * w1,
        a.z * w0 + b.z * w1
    }.Normalized();
}

bool SampleBoneTrack(const VmdBoneTrack& track, double frame, Quat& outRot, Vec3& outPos) noexcept {
    if (track.keys.empty()) return false;
    if (frame <= static_cast<double>(track.keys.front().frame)) {
        outRot = track.keys.front().rotation;
        outPos = track.keys.front().position;
        return true;
    }
    if (frame >= static_cast<double>(track.keys.back().frame)) {
        outRot = track.keys.back().rotation;
        outPos = track.keys.back().position;
        return true;
    }
    const auto it = std::upper_bound(
        track.keys.begin(), track.keys.end(), frame,
        [](double f, const VmdBoneKey& k) { return f < static_cast<double>(k.frame); });
    const auto& b = *it;
    const auto& a = *(it - 1);
    const double span = static_cast<double>(b.frame - a.frame);
    if (!(span > 0.0)) {
        outRot = b.rotation;
        outPos = b.position;
        return true;
    }
    const double rawT = std::clamp((frame - static_cast<double>(a.frame)) / span, 0.0, 1.0);
    const double tx = SampleVmdCurve(b.interp[0], rawT);
    const double ty = SampleVmdCurve(b.interp[1], rawT);
    const double tz = SampleVmdCurve(b.interp[2], rawT);
    const double tr = SampleVmdCurve(b.interp[3], rawT);
    outPos = {
        a.position.x + (b.position.x - a.position.x) * tx,
        a.position.y + (b.position.y - a.position.y) * ty,
        a.position.z + (b.position.z - a.position.z) * tz
    };
    outRot = SlerpQuat(a.rotation, b.rotation, tr);
    return true;
}

float SampleMorphTrack(const VmdMorphTrack& track, double frame) noexcept {
    if (track.keys.empty()) return 0.f;
    if (frame <= static_cast<double>(track.keys.front().frame)) return track.keys.front().weight;
    if (frame >= static_cast<double>(track.keys.back().frame)) return track.keys.back().weight;
    const auto it = std::upper_bound(
        track.keys.begin(), track.keys.end(), frame,
        [](double f, const VmdMorphKey& k) { return f < static_cast<double>(k.frame); });
    const auto& b = *it;
    const auto& a = *(it - 1);
    const double span = static_cast<double>(b.frame - a.frame);
    if (!(span > 0.0)) return b.weight;
    const double t = std::clamp((frame - static_cast<double>(a.frame)) / span, 0.0, 1.0);
    return static_cast<float>(a.weight + (b.weight - a.weight) * t);
}

bool SampleCameraTrackRaw(const std::vector<VmdCameraKey>& keys, double frame, bool autoCut,
                          Vec3& outPos, Vec3& outRot, double& outDistance, double& outFovDeg) noexcept {
    if (keys.empty()) return false;
    if (frame <= static_cast<double>(keys.front().frame)) {
        const auto& k = keys.front();
        outPos = k.position;
        outRot = k.rotation;
        outDistance = k.distance;
        outFovDeg = static_cast<double>(k.viewAngleDeg);
        return true;
    }
    if (frame >= static_cast<double>(keys.back().frame)) {
        const auto& k = keys.back();
        outPos = k.position;
        outRot = k.rotation;
        outDistance = k.distance;
        outFovDeg = static_cast<double>(k.viewAngleDeg);
        return true;
    }
    const auto it = std::upper_bound(
        keys.begin(), keys.end(), frame,
        [](double f, const VmdCameraKey& k) { return f < static_cast<double>(k.frame); });
    const auto& b = *it;
    const auto& a = *(it - 1);
    const uint32_t gap = b.frame - a.frame;
    if (gap == 0 || (autoCut && gap == 1)) {
        outPos = a.position;
        outRot = a.rotation;
        outDistance = a.distance;
        outFovDeg = static_cast<double>(a.viewAngleDeg);
        return true;
    }
    const double rawT = std::clamp((frame - static_cast<double>(a.frame)) / static_cast<double>(gap), 0.0, 1.0);
    const double tx = SampleVmdCurve(b.interp[0], rawT);
    const double ty = SampleVmdCurve(b.interp[1], rawT);
    const double tz = SampleVmdCurve(b.interp[2], rawT);
    const double tr = SampleVmdCurve(b.interp[3], rawT);
    const double td = SampleVmdCurve(b.interp[4], rawT);
    const double tf = SampleVmdCurve(b.interp[5], rawT);

    outPos = {
        a.position.x + (b.position.x - a.position.x) * tx,
        a.position.y + (b.position.y - a.position.y) * ty,
        a.position.z + (b.position.z - a.position.z) * tz
    };
    outRot = {
        a.rotation.x + (b.rotation.x - a.rotation.x) * tr,
        a.rotation.y + (b.rotation.y - a.rotation.y) * tr,
        a.rotation.z + (b.rotation.z - a.rotation.z) * tr
    };
    outDistance = a.distance + (b.distance - a.distance) * td;
    outFovDeg = static_cast<double>(a.viewAngleDeg) +
                (static_cast<double>(b.viewAngleDeg) - static_cast<double>(a.viewAngleDeg)) * tf;
    return true;
}

// ---- VMD Binary Parser ------------------------------------------------------

bool ParseVmdBytes(const uint8_t* data, size_t size, VmdDocument& out, std::string& error) {
    out = {};
    error.clear();
    if (!data || size < 44) {
        error = "VMD data is too small";
        return false;
    }

    BinaryReader r{data, size, 0};
    char header[30]{};
    if (!r.ReadBytes(header, 30)) {
        error = "Failed to read VMD header";
        return false;
    }
    size_t modelNameBytes = 0;
    if (std::memcmp(header, "Vocaloid Motion Data 0002", 25) == 0) {
        modelNameBytes = 20;
    } else if (std::memcmp(header, "Vocaloid Motion Data file", 25) == 0) {
        modelNameBytes = 10;
    } else {
        error = "Unrecognized VMD signature";
        return false;
    }

    char modelRaw[20]{};
    if (!r.ReadBytes(modelRaw, modelNameBytes)) {
        error = "Truncated VMD model header";
        return false;
    }
    out.modelName = ShiftJisToUtf8(modelRaw, modelNameBytes);

    // 1. Bone keyframes
    uint32_t boneCount = 0;
    if (!r.Read(boneCount)) {
        error = "Missing VMD bone keyframe count";
        return false;
    }
    if (static_cast<uint64_t>(boneCount) * 111ULL > size - r.pos) {
        error = "Truncated VMD bone keyframe section";
        return false;
    }
    std::unordered_map<std::string, size_t> boneTrackIdx;
    for (uint32_t i = 0; i < boneCount; ++i) {
        char rawName[15]{};
        uint32_t frame = 0;
        float px = 0.f, py = 0.f, pz = 0.f;
        float qx = 0.f, qy = 0.f, qz = 0.f, qw = 1.f;
        uint8_t interp[64]{};
        if (!r.ReadBytes(rawName, 15) || !r.Read(frame) ||
            !r.Read(px) || !r.Read(py) || !r.Read(pz) ||
            !r.Read(qx) || !r.Read(qy) || !r.Read(qz) || !r.Read(qw) ||
            !r.ReadBytes(interp, 64)) {
            error = "Unexpected EOF in bone keyframes";
            return false;
        }
        std::string name = ShiftJisToUtf8(rawName, 15);
        if (name.empty()) continue;

        Quat q{qw, qx, qy, qz};
        if (!q.IsFinite() || q.NormSq() < 1e-12) q = Quat::Identity();
        else q = q.Normalized();

        VmdBoneKey key{};
        key.frame = frame;
        key.position = {
            std::isfinite(px) ? static_cast<double>(px) : 0.0,
            std::isfinite(py) ? static_cast<double>(py) : 0.0,
            std::isfinite(pz) ? static_cast<double>(pz) : 0.0
        };
        key.rotation = q;
        for (int ch = 0; ch < 4; ++ch) {
            key.interp[ch] = {interp[ch], interp[ch + 4], interp[ch + 8], interp[ch + 12]};
        }
        out.maxBoneFrame = std::max(out.maxBoneFrame, frame);

        auto it = boneTrackIdx.find(name);
        if (it == boneTrackIdx.end()) {
            const size_t idx = out.boneTracks.size();
            boneTrackIdx.emplace(name, idx);
            out.boneTracks.push_back({std::move(name), {key}});
        } else {
            out.boneTracks[it->second].keys.push_back(key);
        }
    }
    for (auto& track : out.boneTracks) {
        std::stable_sort(track.keys.begin(), track.keys.end(),
                         [](const VmdBoneKey& a, const VmdBoneKey& b) { return a.frame < b.frame; });
        // Deduplicate same-frame keys keeping the last one (matches MMD).
        std::vector<VmdBoneKey> dedup;
        dedup.reserve(track.keys.size());
        for (const auto& k : track.keys) {
            if (!dedup.empty() && dedup.back().frame == k.frame) dedup.back() = k;
            else dedup.push_back(k);
        }
        track.keys.swap(dedup);
    }

    // 2. Morph keyframes (optional if file ends here)
    if (r.Has(4)) {
        uint32_t morphCount = 0;
        r.Read(morphCount);
        if (static_cast<uint64_t>(morphCount) * 23ULL <= size - r.pos) {
            std::unordered_map<std::string, size_t> morphTrackIdx;
            for (uint32_t i = 0; i < morphCount; ++i) {
                char rawName[15]{};
                uint32_t frame = 0;
                float weight = 0.f;
                if (!r.ReadBytes(rawName, 15) || !r.Read(frame) || !r.Read(weight)) break;
                std::string name = ShiftJisToUtf8(rawName, 15);
                if (name.empty()) continue;
                if (!std::isfinite(weight)) weight = 0.f;
                out.maxMorphFrame = std::max(out.maxMorphFrame, frame);
                auto it = morphTrackIdx.find(name);
                if (it == morphTrackIdx.end()) {
                    const size_t idx = out.morphTracks.size();
                    morphTrackIdx.emplace(name, idx);
                    out.morphTracks.push_back({std::move(name), {{frame, weight}}});
                } else {
                    out.morphTracks[it->second].keys.push_back({frame, weight});
                }
            }
            for (auto& track : out.morphTracks) {
                std::stable_sort(track.keys.begin(), track.keys.end(),
                                 [](const VmdMorphKey& a, const VmdMorphKey& b) { return a.frame < b.frame; });
                std::vector<VmdMorphKey> dedup;
                dedup.reserve(track.keys.size());
                for (const auto& k : track.keys) {
                    if (!dedup.empty() && dedup.back().frame == k.frame) dedup.back() = k;
                    else dedup.push_back(k);
                }
                track.keys.swap(dedup);
            }
        }
    }

    // 3. Camera keyframes
    if (r.Has(4)) {
        uint32_t camCount = 0;
        r.Read(camCount);
        if (static_cast<uint64_t>(camCount) * 61ULL <= size - r.pos) {
            out.cameraKeys.reserve(camCount);
            for (uint32_t i = 0; i < camCount; ++i) {
                uint32_t frame = 0;
                float dist = 0.f;
                float px = 0.f, py = 0.f, pz = 0.f;
                float rx = 0.f, ry = 0.f, rz = 0.f;
                uint8_t interp[24]{};
                uint32_t fov = 30;
                uint8_t ortho = 0;
                if (!r.Read(frame) || !r.Read(dist) ||
                    !r.Read(px) || !r.Read(py) || !r.Read(pz) ||
                    !r.Read(rx) || !r.Read(ry) || !r.Read(rz) ||
                    !r.ReadBytes(interp, 24) || !r.Read(fov) || !r.Read(ortho)) {
                    break;
                }
                VmdCameraKey ck{};
                ck.frame = frame;
                ck.distance = std::isfinite(dist) ? dist : -45.f;
                ck.position = {
                    std::isfinite(px) ? static_cast<double>(px) : 0.0,
                    std::isfinite(py) ? static_cast<double>(py) : 0.0,
                    std::isfinite(pz) ? static_cast<double>(pz) : 0.0
                };
                ck.rotation = {
                    std::isfinite(rx) ? static_cast<double>(rx) : 0.0,
                    std::isfinite(ry) ? static_cast<double>(ry) : 0.0,
                    std::isfinite(rz) ? static_cast<double>(rz) : 0.0
                };
                for (int ch = 0; ch < 6; ++ch) {
                    ck.interp[ch] = {interp[ch * 4], interp[ch * 4 + 2], interp[ch * 4 + 1], interp[ch * 4 + 3]};
                }
                ck.viewAngleDeg = fov;
                ck.perspective = (ortho == 0);
                out.maxCameraFrame = std::max(out.maxCameraFrame, frame);
                out.cameraKeys.push_back(ck);
            }
            std::stable_sort(out.cameraKeys.begin(), out.cameraKeys.end(),
                             [](const VmdCameraKey& a, const VmdCameraKey& b) { return a.frame < b.frame; });
            std::vector<VmdCameraKey> dedup;
            dedup.reserve(out.cameraKeys.size());
            for (const auto& k : out.cameraKeys) {
                if (!dedup.empty() && dedup.back().frame == k.frame) dedup.back() = k;
                else dedup.push_back(k);
            }
            out.cameraKeys.swap(dedup);
        }
    }

    // 4. Light keyframes (skip 28 bytes each)
    if (r.Has(4)) {
        uint32_t lightCount = 0;
        r.Read(lightCount);
        if (static_cast<uint64_t>(lightCount) * 28ULL <= size - r.pos) {
            r.Skip(static_cast<size_t>(lightCount) * 28);
        } else {
            return true;
        }
    }

    // 5. Self-shadow keyframes (skip 9 bytes each)
    if (r.Has(4)) {
        uint32_t shadowCount = 0;
        r.Read(shadowCount);
        if (static_cast<uint64_t>(shadowCount) * 9ULL <= size - r.pos) {
            r.Skip(static_cast<size_t>(shadowCount) * 9);
        } else {
            return true;
        }
    }

    // 6. Visibility + IK switch keyframes
    if (r.Has(4)) {
        uint32_t visCount = 0;
        r.Read(visCount);
        std::map<std::string, std::vector<VmdIkSwitch>> ikMap;
        for (uint32_t i = 0; i < visCount; ++i) {
            uint32_t frame = 0;
            uint8_t visible = 1;
            uint32_t ikCount = 0;
            if (!r.Read(frame) || !r.Read(visible) || !r.Read(ikCount)) break;
            out.visibilityKeys.push_back({frame, visible != 0});
            if (static_cast<uint64_t>(ikCount) * 21ULL > size - r.pos) break;
            for (uint32_t j = 0; j < ikCount; ++j) {
                char ikNameRaw[20]{};
                uint8_t enabled = 1;
                if (!r.ReadBytes(ikNameRaw, 20) || !r.Read(enabled)) break;
                const std::string ikName = CanonicalMmdBoneName(ShiftJisToUtf8(ikNameRaw, 20));
                if (!ikName.empty()) {
                    ikMap[ikName].push_back({frame, enabled != 0});
                }
            }
        }
        for (auto& [ikName, list] : ikMap) {
            std::stable_sort(list.begin(), list.end(),
                             [](const VmdIkSwitch& a, const VmdIkSwitch& b) { return a.frame < b.frame; });
            out.ikStates.emplace_back(ikName, std::move(list));
        }
    }

    return true;
}

bool ParseVmdFile(const std::filesystem::path& path, VmdDocument& out, std::string& error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        error = "Could not open VMD file";
        return false;
    }
    const auto endPos = in.tellg();
    if (endPos <= 0 || endPos > 256LL * 1024LL * 1024LL) {
        error = "VMD file is empty or exceeds 256 MiB limit";
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(endPos));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in.good()) {
        error = "Failed to read VMD file";
        return false;
    }
    return ParseVmdBytes(bytes.data(), bytes.size(), out, error);
}

// ---- VPD Static Pose Text Parser --------------------------------------------

bool ParseVpdBytes(const uint8_t* data, size_t size, VmdDocument& out, std::string& error) {
    out = {};
    error.clear();
    if (!data || size < 20) {
        error = "VPD data is too small";
        return false;
    }
    const std::string utf8 = ShiftJisToUtf8(reinterpret_cast<const char*>(data), size);
    if (utf8.find("Vocaloid Pose Data file") == std::string::npos) {
        error = "Unrecognized VPD header";
        return false;
    }

    size_t pos = 0;
    while (pos < utf8.size()) {
        const size_t bonePos = utf8.find("Bone", pos);
        const size_t morphPos = utf8.find("Morph", pos);
        const bool isBone = (bonePos != std::string::npos) &&
                            (morphPos == std::string::npos || bonePos < morphPos);
        const size_t start = isBone ? bonePos : morphPos;
        if (start == std::string::npos) break;

        const size_t braceOpen = utf8.find('{', start);
        const size_t braceClose = (braceOpen != std::string::npos) ? utf8.find('}', braceOpen) : std::string::npos;
        if (braceOpen == std::string::npos || braceClose == std::string::npos) break;

        const size_t lineEnd = utf8.find('\n', braceOpen);
        if (lineEnd == std::string::npos || lineEnd >= braceClose) {
            pos = braceClose + 1;
            continue;
        }
        std::string name = utf8.substr(braceOpen + 1, lineEnd - braceOpen - 1);
        while (!name.empty() && (name.back() == '\r' || name.back() == ' ' || name.back() == '\t')) name.pop_back();
        const std::string body = utf8.substr(lineEnd + 1, braceClose - lineEnd - 1);

        if (isBone) {
            double vals[7]{0, 0, 0, 0, 0, 0, 1};
            int count = 0;
            std::string cleaned;
            cleaned.reserve(body.size());
            for (char c : body) {
                cleaned.push_back((c == ',' || c == ';') ? ' ' : c);
            }
            std::istringstream iss(cleaned);
            while (count < 7 && (iss >> vals[count])) ++count;
            if (!name.empty() && count == 7) {
                VmdBoneKey key{};
                key.frame = 0;
                key.position = {vals[0], vals[1], vals[2]};
                Quat q{vals[6], vals[3], vals[4], vals[5]};
                key.rotation = (q.IsFinite() && q.NormSq() > 1e-12) ? q.Normalized() : Quat::Identity();
                out.boneTracks.push_back({std::move(name), {key}});
            }
        } else {
            std::string cleaned = body;
            for (char& c : cleaned) if (c == ';') c = ' ';
            std::istringstream iss(cleaned);
            float weight = 0.f;
            if (!name.empty() && (iss >> weight) && std::isfinite(weight)) {
                out.morphTracks.push_back({std::move(name), {{0, weight}}});
            }
        }
        pos = braceClose + 1;
    }

    if (out.boneTracks.empty() && out.morphTracks.empty()) {
        error = "No Bone or Morph entries found in VPD";
        return false;
    }
    out.maxBoneFrame = 0;
    return true;
}

bool ParseVpdFile(const std::filesystem::path& path, VmdDocument& out, std::string& error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        error = "Could not open VPD file";
        return false;
    }
    const auto endPos = in.tellg();
    if (endPos <= 0 || endPos > 16LL * 1024LL * 1024LL) {
        error = "VPD file is empty or too large";
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(endPos));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in.good()) {
        error = "Failed to read VPD file";
        return false;
    }
    return ParseVpdBytes(bytes.data(), bytes.size(), out, error);
}

// ---- Reference PMX JSON Loader ----------------------------------------------

bool LoadReferenceSkeletonJson(std::string_view jsonText, ReferenceSkeleton& out, std::string& error) {
    out = {};
    error.clear();
    json doc = json::parse(jsonText.begin(), jsonText.end(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("bones") || !doc["bones"].is_array()) {
        error = "Invalid reference skeleton JSON";
        return false;
    }
    out.modelName = doc.value("modelName", std::string{});
    const auto& arr = doc["bones"];
    out.bones.reserve(arr.size());
    for (size_t i = 0; i < arr.size(); ++i) {
        const auto& item = arr[i];
        if (!item.is_object()) continue;
        ReferenceBone b{};
        b.name = item.value("name", std::string{});
        b.index = item.value("index", static_cast<uint32_t>(i));
        b.parent = item.value("parent", -1);
        b.tailBone = item.value("tailBone", -1);
        if (item.contains("position") && item["position"].is_array() && item["position"].size() == 3) {
            b.position = {
                item["position"][0].get<double>(),
                item["position"][1].get<double>(),
                item["position"][2].get<double>()
            };
        }
        if (item.contains("tailOffset") && item["tailOffset"].is_array() && item["tailOffset"].size() == 3) {
            b.tailOffset = {
                item["tailOffset"][0].get<double>(),
                item["tailOffset"][1].get<double>(),
                item["tailOffset"][2].get<double>()
            };
        }
        if (item.contains("ik") && item["ik"].is_object()) {
            const auto& ikObj = item["ik"];
            b.ik.valid = true;
            b.ik.target = ikObj.value("target", 0u);
            b.ik.loop = ikObj.value("loop", 40u);
            b.ik.limitRadian = ikObj.value("limitRadian", 0.5);
            if (ikObj.contains("links") && ikObj["links"].is_array()) {
                for (const auto& lk : ikObj["links"]) {
                    if (!lk.is_object()) continue;
                    ReferenceIkLink link{};
                    link.bone = lk.value("bone", 0u);
                    const char* minKey = lk.contains("limitMin") ? "limitMin" : "min";
                    const char* maxKey = lk.contains("limitMax") ? "limitMax" : "max";
                    if (lk.contains(minKey) && lk.contains(maxKey) &&
                        lk[minKey].is_array() && lk[minKey].size() == 3 &&
                        lk[maxKey].is_array() && lk[maxKey].size() == 3) {
                        link.hasLimit = true;
                        link.minRad = {lk[minKey][0].get<double>(), lk[minKey][1].get<double>(), lk[minKey][2].get<double>()};
                        link.maxRad = {lk[maxKey][0].get<double>(), lk[maxKey][1].get<double>(), lk[maxKey][2].get<double>()};
                    }
                    b.ik.links.push_back(link);
                }
            }
        }
        out.bones.push_back(std::move(b));
    }
    if (out.bones.empty()) {
        error = "Reference skeleton contains no bones";
        return false;
    }
    return true;
}

bool LoadReferenceSkeletonFile(const std::filesystem::path& path, ReferenceSkeleton& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        error = "Could not open reference skeleton file";
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return LoadReferenceSkeletonJson(ss.str(), out, error);
}

// ---- Compatibility Analysis -------------------------------------------------

VmdCompatibilityReport AnalyzeVmdCompatibility(const VmdDocument& vmd) {
    VmdCompatibilityReport rep{};
    rep.ok = true;
    rep.boneTrackCount = static_cast<uint32_t>(vmd.boneTracks.size());
    rep.morphTrackCount = static_cast<uint32_t>(vmd.morphTracks.size());
    rep.cameraKeyCount = static_cast<uint32_t>(vmd.cameraKeys.size());
    rep.frameCount = std::max(vmd.maxBoneFrame, vmd.maxMorphFrame) + 1;

    std::unordered_set<std::string> present;
    for (const auto& t : vmd.boneTracks) {
        rep.boneKeyCount += static_cast<uint32_t>(t.keys.size());
        present.insert(CanonicalMmdBoneName(t.name));
    }
    auto has = [&](const char* name) { return present.count(CanonicalMmdBoneName(name)) != 0; };

    rep.hasFootIk = has("左足ＩＫ") || has("右足ＩＫ"); // i18n-data
    rep.hasCompleteLegFk =
        (has("左足") || has("左足D")) && (has("左ひざ") || has("左ひざD")) && (has("左足首") || has("左足首D")) && // i18n-data
        (has("右足") || has("右足D")) && (has("右ひざ") || has("右ひざD")) && (has("右足首") || has("右足首D")); // i18n-data
    rep.hasLegD = has("左足D") || has("右足D") || has("左ひざD") || has("右ひざD") || // i18n-data
                  has("左足首D") || has("右足首D") || has("左足先EX") || has("右足先EX"); // i18n-data
    rep.hasShoulderCancel = has("左肩P") || has("右肩P") || has("左肩C") || has("右肩C"); // i18n-data
    rep.hasWaistCancel = has("腰キャンセル左") || has("腰キャンセル右"); // i18n-data
    rep.hasTwistTracks = has("左腕捩") || has("右腕捩") || has("左手捩") || has("右手捩"); // i18n-data
    rep.hasUpperBody1 = has("上半身1"); // i18n-data
    rep.hasUpperBody3 = has("上半身3"); // i18n-data

    int scorePmx = 0;
    int scoreUnity = 0;
    int scoreEndfield = 0;
    if (rep.hasLegD) { scoreUnity += 3; scoreEndfield += 2; }
    if (rep.hasShoulderCancel) { scoreUnity += 2; scoreEndfield += 2; }
    if (rep.hasTwistTracks) { scoreUnity += 2; scoreEndfield += 2; }
    if (rep.hasWaistCancel || rep.hasUpperBody3) { scoreEndfield += 4; }
    if (rep.hasUpperBody1) { scoreUnity += 2; }
    if (!rep.hasLegD && !rep.hasShoulderCancel && !rep.hasWaistCancel) { scorePmx += 3; }

    if (scoreEndfield > scoreUnity && scoreEndfield > scorePmx) {
        rep.recommendedReference = 2;
        rep.confidence = scoreEndfield - std::max(scoreUnity, scorePmx);
    } else if (scoreUnity > scorePmx) {
        rep.recommendedReference = 1;
        rep.confidence = scoreUnity - std::max(scorePmx, scoreEndfield);
    } else {
        rep.recommendedReference = 0;
        rep.confidence = scorePmx - std::max(scoreUnity, scoreEndfield);
    }
    return rep;
}

// ---- Unity LH Y-Up Motion Retargeter ----------------------------------------

bool BuildUnityMotion(const VmdDocument& vmd,
                      const ReferenceSkeleton& reference,
                      const TargetSkeletonInput& target,
                      const RetargetOptions& options,
                      LoadedMotion& out,
                      std::string& error) {
    out = {};
    error.clear();

    if (target.bones.empty()) {
        error = "Target skeleton is empty";
        return false;
    }
    if (reference.bones.empty()) {
        error = "Reference skeleton is empty";
        return false;
    }
    if (vmd.boneTracks.empty() && vmd.morphTracks.empty()) {
        error = "VMD contains no bone or morph tracks";
        return false;
    }

    const ResolvedTargetRig rig = ResolveTargetRig(target);
    const Quat A = rig.mmdToChar;
    const Quat Ainv = A.Conjugate();

    // Index VMD tracks by canonical Japanese name.
    std::unordered_map<std::string, const VmdBoneTrack*> trackMap;
    for (const auto& t : vmd.boneTracks) {
        trackMap.emplace(CanonicalMmdBoneName(t.name), &t);
    }
    auto findTrack = [&](std::string_view name) -> const VmdBoneTrack* {
        const auto it = trackMap.find(CanonicalMmdBoneName(name));
        return (it != trackMap.end()) ? it->second : nullptr;
    };

    // Build source-side IkPose solver.
    IkPose ikPose;
    const bool poseReady = ikPose.Init(reference);
    std::vector<const VmdBoneTrack*> ikTrackByBone(reference.bones.size(), nullptr);
    for (size_t i = 0; i < reference.bones.size(); ++i) {
        ikTrackByBone[i] = findTrack(reference.bones[i].name);
    }

    out.report = AnalyzeVmdCompatibility(vmd);
    const bool solveIk = poseReady &&
                         (options.ikMode == IkMode::ForceOn ||
                          (options.ikMode == IkMode::FollowMotion && out.report.hasFootIk));

    // Build mapping table: target bone index -> MMD source chain (prefixInverse, sources, inverse).
    struct BoneMappingEntry {
        int targetIndex{-1};
        TargetRole role{TargetRole::Count};
        MotionPart part{MotionPart::Other};
        std::vector<std::string> prefixInverse;
        std::vector<std::string> sources;
        std::vector<std::string> inverse;
        bool allowDirectionAlign{false};
    };
    std::vector<BoneMappingEntry> table;
    std::unordered_set<int> usedTargetBones;

    auto addMapping = [&](TargetRole role, MotionPart part,
                          std::vector<std::string> prefixInv,
                          std::vector<std::string> sources,
                          std::vector<std::string> inv,
                          bool alignDir) {
        const int idx = rig.BoneOf(role);
        if (idx < 0 || usedTargetBones.count(idx) != 0) return;
        usedTargetBones.insert(idx);
        table.push_back({idx, role, part, std::move(prefixInv), std::move(sources), std::move(inv), alignDir});
    };

    const bool hasGroove = findTrack("グルーブ") != nullptr; // i18n-data
    const bool hasWaist = findTrack("腰") != nullptr; // i18n-data
    const bool hasHip = findTrack("下半身") != nullptr; // i18n-data
    const bool distinctRootAndPelvis = (rig.BoneOf(TargetRole::Root) >= 0 &&
                                        rig.BoneOf(TargetRole::Pelvis) >= 0 &&
                                        rig.BoneOf(TargetRole::Root) != rig.BoneOf(TargetRole::Pelvis));

    std::vector<std::string> rootSources{"全ての親", "センター"}; // i18n-data
    if (hasGroove) rootSources.push_back("グルーブ"); // i18n-data

    if (distinctRootAndPelvis) {
        addMapping(TargetRole::Root, MotionPart::Root, {}, rootSources, {}, false);
        std::vector<std::string> pelvisSources;
        if (hasWaist) pelvisSources.push_back("腰"); // i18n-data
        if (options.hipOnPelvis && hasHip) pelvisSources.push_back("下半身"); // i18n-data
        addMapping(TargetRole::Pelvis, MotionPart::Pelvis, {}, pelvisSources, {}, false);
    } else {
        std::vector<std::string> combined = rootSources;
        if (hasWaist) combined.push_back("腰"); // i18n-data
        if (options.hipOnPelvis && hasHip) combined.push_back("下半身"); // i18n-data
        addMapping(TargetRole::Pelvis, MotionPart::Root, {}, combined, {}, false);
    }

    // Spine distribution across however many spine bones the Unity rig actually has (1..4).
    // Always register every spine bone in `table` (empty sources holds clean bind rest pose and
    // blocks live idle animation from twisting an unmapped upper-chest bone).
    {
        std::vector<TargetRole> availSpines;
        for (TargetRole sr : {TargetRole::Spine, TargetRole::Spine1, TargetRole::Spine2, TargetRole::Spine3}) {
            if (rig.BoneOf(sr) >= 0) availSpines.push_back(sr);
        }
        std::vector<std::string> hipCancel;
        if (options.hipOnPelvis && hasHip) hipCancel.push_back("下半身"); // i18n-data

        std::vector<std::string> upperSources;
        for (const char* s : {"上半身", "上半身1", "上半身2", "上半身3"}) { // i18n-data
            if (findTrack(s) != nullptr) upperSources.emplace_back(s);
        }
        if (upperSources.empty()) upperSources.emplace_back("上半身"); // i18n-data

        if (availSpines.size() == 1) {
            addMapping(availSpines[0], MotionPart::Spine, hipCancel, upperSources, {}, false);
        } else if (availSpines.size() == 2) {
            std::vector<std::string> s0{upperSources.front()};
            std::vector<std::string> s1(upperSources.begin() + 1, upperSources.end());
            addMapping(availSpines[0], MotionPart::Spine, hipCancel, s0, {}, false);
            addMapping(availSpines[1], MotionPart::Spine, {}, s1, {}, false);
        } else if (availSpines.size() >= 3) {
            // Map 上半身 -> Spine, 上半身1 (if present) -> Spine1, 上半身2/3 -> Spine1/Spine2
            std::vector<std::string> b0, b1, b2;
            b0.push_back("上半身"); // i18n-data
            if (findTrack("上半身1") != nullptr) { // i18n-data
                b1.push_back("上半身1"); // i18n-data
                if (findTrack("上半身2") != nullptr) b2.push_back("上半身2"); // i18n-data
                if (findTrack("上半身3") != nullptr) b2.push_back("上半身3"); // i18n-data
            } else {
                if (findTrack("上半身2") != nullptr) b1.push_back("上半身2"); // i18n-data
                if (findTrack("上半身3") != nullptr) b2.push_back("上半身3"); // i18n-data
            }
            addMapping(availSpines[0], MotionPart::Spine, hipCancel, b0, {}, false);
            addMapping(availSpines[1], MotionPart::Spine, {}, b1, {}, false);
            addMapping(availSpines[2], MotionPart::Spine, {}, b2, {}, false);
            for (size_t extra = 3; extra < availSpines.size(); ++extra) {
                addMapping(availSpines[extra], MotionPart::Spine, {}, {}, {}, false);
            }
        }
    }

    addMapping(TargetRole::Neck, MotionPart::Head, {}, {"首"}, {}, false); // i18n-data
    addMapping(TargetRole::Head, MotionPart::Head, {}, {"頭"}, {}, false); // i18n-data
    if (rig.BoneOf(TargetRole::EyeL) >= 0) {
        addMapping(TargetRole::EyeL, MotionPart::Head, {}, {"両目", "左目"}, {}, false); // i18n-data
    }
    if (rig.BoneOf(TargetRole::EyeR) >= 0) {
        addMapping(TargetRole::EyeR, MotionPart::Head, {}, {"両目", "右目"}, {}, false); // i18n-data
    }

    const size_t nBones = target.bones.size();
    const auto topo = BuildTargetTopoOrder(target);

    auto isAncestorOf = [&](int anc, int node) -> bool {
        if (anc < 0 || node < 0) return false;
        int cur = node;
        for (int guard = 0; guard < 64 && cur >= 0 && static_cast<size_t>(cur) < nBones; ++guard) {
            if (cur == anc) return true;
            cur = target.bones[static_cast<size_t>(cur)].parent;
        }
        return false;
    };

    // Discover leaf forearm twist helpers (parallel deform bones hanging off Forearm, NOT ancestors of Hand).
    std::array<std::vector<int>, 2> leafForearmTwists;
    for (int side = 0; side < 2; ++side) {
        const int foreIdx = rig.BoneOf(side == 0 ? TargetRole::ForearmL : TargetRole::ForearmR);
        const int handIdx = rig.BoneOf(side == 0 ? TargetRole::HandL : TargetRole::HandR);
        if (foreIdx < 0 || handIdx < 0) continue;
        for (size_t i : topo) {
            const int idx = static_cast<int>(i);
            if (idx == foreIdx || idx == handIdx) continue;
            if (!IsTwistOrHelperBone(target.bones[i].name) || IsNonStructuralBone(target.bones[i].name)) continue;
            if (isAncestorOf(foreIdx, idx) && !isAncestorOf(idx, handIdx)) {
                leafForearmTwists[static_cast<size_t>(side)].push_back(idx);
                if (leafForearmTwists[static_cast<size_t>(side)].size() >= 2) break;
            }
        }
    }

    // Arms + Legs + Fingers (Left & Right)
    for (int side = 0; side < 2; ++side) {
        const std::string jp = (side == 0) ? "左" : "右"; // i18n-data
        const TargetRole rClav  = side == 0 ? TargetRole::ClavicleL : TargetRole::ClavicleR;
        const TargetRole rUpArm = side == 0 ? TargetRole::UpperArmL : TargetRole::UpperArmR;
        const TargetRole rFore  = side == 0 ? TargetRole::ForearmL : TargetRole::ForearmR;
        const TargetRole rHand  = side == 0 ? TargetRole::HandL : TargetRole::HandR;

        const bool hasShoulderP = findTrack(jp + "肩P") != nullptr; // i18n-data
        const bool hasShoulderC = findTrack(jp + "肩C") != nullptr; // i18n-data

        // Keep clavicle's own anatomical rest slope (do NOT direction-align to Miku's -23.5 deg shoulder drop).
        std::vector<std::string> clavSources;
        if (hasShoulderP) clavSources.push_back(jp + "肩P"); // i18n-data
        clavSources.push_back(jp + "肩"); // i18n-data
        addMapping(rClav, MotionPart::Arm, {}, clavSources, {}, false);

        std::vector<std::string> armPrefixInv;
        if (hasShoulderP && hasShoulderC) armPrefixInv.push_back(jp + "肩P"); // i18n-data
        std::vector<std::string> armSources;
        if (hasShoulderC) armSources.push_back(jp + "肩C"); // i18n-data
        armSources.push_back(jp + "腕"); // i18n-data
        // Always fold 腕捩 into UpperArm so Forearm and Hand inherit 100% of authored upper-arm twist.
        if (findTrack(jp + "腕捩") != nullptr) { // i18n-data
            armSources.push_back(jp + "腕捩"); // i18n-data
        }
        addMapping(rUpArm, MotionPart::Arm, armPrefixInv, armSources, {}, true);

        const bool hasLeafForeTwist = !leafForearmTwists[static_cast<size_t>(side)].empty();
        const bool hasHandTwistTrack = findTrack(jp + "手捩") != nullptr; // i18n-data

        std::vector<std::string> foreSources{jp + "ひじ"}; // i18n-data
        if (!hasLeafForeTwist && hasHandTwistTrack) {
            foreSources.push_back(jp + "手捩"); // i18n-data
        }
        addMapping(rFore, MotionPart::Arm, {}, foreSources, {}, true);

        std::vector<std::string> handSources;
        if (hasLeafForeTwist && hasHandTwistTrack) {
            handSources.push_back(jp + "手捩"); // i18n-data
        }
        handSources.push_back(jp + "手首"); // i18n-data
        addMapping(rHand, MotionPart::Hand, {}, handSources, {}, false);

        // Legs: prefer 足D/ひざD/足首D/足先EX when present (without IK solving over base 足).
        const TargetRole rThigh = side == 0 ? TargetRole::ThighL : TargetRole::ThighR;
        const TargetRole rCalf  = side == 0 ? TargetRole::CalfL : TargetRole::CalfR;
        const TargetRole rFoot  = side == 0 ? TargetRole::FootL : TargetRole::FootR;
        const TargetRole rToe   = side == 0 ? TargetRole::ToeL : TargetRole::ToeR;

        const bool useLegD = !solveIk && (findTrack(jp + "足D") != nullptr); // i18n-data
        std::vector<std::string> thighSources;
        if (!options.hipOnPelvis && hasHip) thighSources.push_back("下半身"); // i18n-data
        thighSources.push_back(useLegD ? (jp + "足D") : (jp + "足")); // i18n-data
        addMapping(rThigh, MotionPart::Leg, {}, thighSources, {}, false);
        addMapping(rCalf, MotionPart::Leg, {}, {useLegD ? (jp + "ひざD") : (jp + "ひざ")}, {}, false); // i18n-data
        addMapping(rFoot, MotionPart::Leg, {}, {useLegD ? (jp + "足首D") : (jp + "足首")}, {}, false); // i18n-data

        std::vector<std::string> toeSources;
        if (findTrack(jp + "足先EX") != nullptr) toeSources.push_back(jp + "足先EX"); // i18n-data
        toeSources.push_back(jp + "つま先"); // i18n-data
        addMapping(rToe, MotionPart::Leg, {}, toeSources, {}, false);

        // 5 Fingers
        static constexpr std::array<const char*, 5> kFingerJp{"親指", "人指", "中指", "薬指", "小指"}; // i18n-data
        for (int f = 0; f < 5; ++f) {
            if (f == 0) {
                // Thumb: MMD uses 親指0, 親指1, 親指2 (some PMDs only have 親指1, 親指2).
                const bool hasT0 = findTrack(jp + "親指0") != nullptr || findTrack(jp + "親指０") != nullptr; // i18n-data
                const bool hasTarget2 = rig.BoneOf(FingerRole(side, 0, 2)) >= 0;
                if (hasTarget2) {
                    addMapping(FingerRole(side, 0, 0), MotionPart::Hand, {}, {jp + "親指0"}, {}, false); // i18n-data
                    addMapping(FingerRole(side, 0, 1), MotionPart::Hand, {}, {jp + "親指1"}, {}, false); // i18n-data
                    addMapping(FingerRole(side, 0, 2), MotionPart::Hand, {}, {jp + "親指2"}, {}, false); // i18n-data
                } else {
                    std::vector<std::string> j0;
                    if (hasT0) j0.push_back(jp + "親指0"); // i18n-data
                    j0.push_back(jp + "親指1"); // i18n-data
                    addMapping(FingerRole(side, 0, 0), MotionPart::Hand, {}, j0, {}, false);
                    addMapping(FingerRole(side, 0, 1), MotionPart::Hand, {}, {jp + "親指2"}, {}, false); // i18n-data
                }
            } else {
                addMapping(FingerRole(side, f, 0), MotionPart::Hand, {}, {jp + kFingerJp[f] + "1"}, {}, false);
                addMapping(FingerRole(side, f, 1), MotionPart::Hand, {}, {jp + kFingerJp[f] + "2"}, {}, false);
                addMapping(FingerRole(side, f, 2), MotionPart::Hand, {}, {jp + kFingerJp[f] + "3"}, {}, false);
            }
        }
    }

    if (table.empty()) {
        error = "Could not match any humanoid bones on the target skeleton";
        return false;
    }

    // ---- Rest-pose direction correction with full topological propagation ----
    // Only arms (UpperArm / Forearm) align from T-pose to MMD A-pose, using exact anatomical child
    // roles (never averaging sleeves/cloth/breast children), and every downstream bone (Hand,
    // Fingers, twist helpers, etc.) propagates `restCrot[i] = restCrot[parent] * restLocal[i]` so
    // zero reverse kink is ever introduced at wrists, ankles, or neck.
    const std::vector<Quat> restCrotCap = rig.restRot;
    std::vector<Quat> restCrot = rig.restRot;
    std::vector<Quat> restLocal(nBones, Quat::Identity());
    for (size_t i = 0; i < nBones; ++i) {
        restLocal[i] = target.bones[i].restLocalRot;
    }

    // Precompute reference MMD bone directions in character space.
    std::unordered_map<std::string, Vec3> refDirInChar;
    for (const auto& rb : reference.bones) {
        Vec3 dir{};
        if (rb.tailBone >= 0 && static_cast<size_t>(rb.tailBone) < reference.bones.size()) {
            dir = reference.bones[static_cast<size_t>(rb.tailBone)].position - rb.position;
        } else if (rb.tailOffset.LengthSq() > 1e-6) {
            dir = rb.tailOffset;
        } else {
            for (const auto& child : reference.bones) {
                if (child.parent == static_cast<int32_t>(rb.index)) {
                    dir = child.position - rb.position;
                    if (dir.LengthSq() > 1e-6) break;
                }
            }
        }
        if (dir.LengthSq() > 1e-6) {
            refDirInChar.emplace(CanonicalMmdBoneName(rb.name), A.Rotate(dir.Normalized()));
        }
    }

    std::unordered_map<int, size_t> targetToTableIdx;
    for (size_t ti = 0; ti < table.size(); ++ti) {
        targetToTableIdx.emplace(table[ti].targetIndex, ti);
    }

    auto armDirectionSpec = [&](TargetRole role, int& outChildBone, const char*& outMmdBone) -> bool {
        switch (role) {
            case TargetRole::UpperArmL:
                outChildBone = rig.BoneOf(TargetRole::ForearmL);
                outMmdBone = "左腕"; // i18n-data
                return outChildBone >= 0;
            case TargetRole::ForearmL:
                outChildBone = rig.BoneOf(TargetRole::HandL);
                outMmdBone = "左ひじ"; // i18n-data
                return outChildBone >= 0;
            case TargetRole::UpperArmR:
                outChildBone = rig.BoneOf(TargetRole::ForearmR);
                outMmdBone = "右腕"; // i18n-data
                return outChildBone >= 0;
            case TargetRole::ForearmR:
                outChildBone = rig.BoneOf(TargetRole::HandR);
                outMmdBone = "右ひじ"; // i18n-data
                return outChildBone >= 0;
            default:
                return false;
        }
    };

    for (size_t i : topo) {
        const int p = target.bones[i].parent;
        const Quat parentCorrected = (p >= 0 && static_cast<size_t>(p) < nBones)
                                         ? restCrot[static_cast<size_t>(p)]
                                         : target.goToRootParentRot;
        bool aligned = false;
        const auto tit = targetToTableIdx.find(static_cast<int>(i));
        if (tit != targetToTableIdx.end()) {
            const auto& entry = table[tit->second];
            int childBone = -1;
            const char* refName = nullptr;
            if (entry.allowDirectionAlign && armDirectionSpec(entry.role, childBone, refName)) {
                const Vec3 tVec = rig.restPos[static_cast<size_t>(childBone)] - rig.restPos[i];
                const auto rit = refDirInChar.find(CanonicalMmdBoneName(refName));
                if (tVec.LengthSq() > 1e-6 && rit != refDirInChar.end()) {
                    const Vec3 tDir = tVec.Normalized();
                    const Vec3 sDir = rit->second;
                    const double ang = AngleBetweenDeg(tDir, sDir);
                    if (ang > 1e-3 && ang <= 90.0) {
                        const Quat fix = SwingRotation(tDir, sDir);
                        const Quat corrected = (fix * restCrotCap[i]).Normalized();
                        restCrot[i] = corrected;
                        restLocal[i] = (parentCorrected.Conjugate() * corrected).Normalized();
                        aligned = true;
                    }
                }
            }
        }
        if (!aligned) {
            restCrot[i] = (parentCorrected * restLocal[i]).Normalized();
        }
    }

    // ---- Unit Scale (Target Leg Length / MMD Reference Leg Length) ----
    auto refDist = [&](const char* a1, const char* a2, const char* b1, const char* b2) -> double {
        const ReferenceBone* a = FindRefBone(reference, a1);
        if (!a && a2) a = FindRefBone(reference, a2);
        const ReferenceBone* b = FindRefBone(reference, b1);
        if (!b && b2) b = FindRefBone(reference, b2);
        if (!a || !b) return 0.0;
        return (b->position - a->position).Length();
    };
    const double mmdLegLen = refDist("左足", "左足D", "左ひざ", "左ひざD") + // i18n-data
                             refDist("左ひざ", "左ひざD", "左足首", "左足首D"); // i18n-data
    double targetLegLen = 0.0;
    if (rig.BoneOf(TargetRole::ThighL) >= 0 && rig.BoneOf(TargetRole::CalfL) >= 0 && rig.BoneOf(TargetRole::FootL) >= 0) {
        targetLegLen = (rig.restPos[static_cast<size_t>(rig.BoneOf(TargetRole::CalfL))] -
                        rig.restPos[static_cast<size_t>(rig.BoneOf(TargetRole::ThighL))]).Length() +
                       (rig.restPos[static_cast<size_t>(rig.BoneOf(TargetRole::FootL))] -
                        rig.restPos[static_cast<size_t>(rig.BoneOf(TargetRole::CalfL))]).Length();
    } else if (rig.BoneOf(TargetRole::ThighR) >= 0 && rig.BoneOf(TargetRole::CalfR) >= 0 && rig.BoneOf(TargetRole::FootR) >= 0) {
        targetLegLen = (rig.restPos[static_cast<size_t>(rig.BoneOf(TargetRole::CalfR))] -
                        rig.restPos[static_cast<size_t>(rig.BoneOf(TargetRole::ThighR))]).Length() +
                       (rig.restPos[static_cast<size_t>(rig.BoneOf(TargetRole::FootR))] -
                        rig.restPos[static_cast<size_t>(rig.BoneOf(TargetRole::CalfR))]).Length();
    }
    const double unitScale = (mmdLegLen > 1e-4 && targetLegLen > 0.1) ? (targetLegLen / mmdLegLen) : 0.08;

    // ---- Root Chain Selection (matches Anomaly's placement-layer rules) ----
    std::vector<std::pair<std::string, uint32_t>> namedChain;
    for (const char* nm : {"全ての親", "センター", "グルーブ", "腰", "下半身"}) { // i18n-data
        if (const ReferenceBone* rb = FindRefBone(reference, nm)) {
            namedChain.emplace_back(nm, rb->index);
        }
    }
    auto posSpan = [&](const std::string& name) -> double {
        const VmdBoneTrack* tr = findTrack(name);
        if (!tr || tr->keys.size() < 2) return 0.0;
        Vec3 lo = tr->keys.front().position;
        Vec3 hi = lo;
        for (const auto& k : tr->keys) {
            lo.x = std::min(lo.x, k.position.x); hi.x = std::max(hi.x, k.position.x);
            lo.y = std::min(lo.y, k.position.y); hi.y = std::max(hi.y, k.position.y);
            lo.z = std::min(lo.z, k.position.z); hi.z = std::max(hi.z, k.position.z);
        }
        return std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    };
    size_t rootEnd = 0;
    for (size_t s = 0; s < namedChain.size(); ++s) {
        if (namedChain[s].first == "センター") { rootEnd = s; break; } // i18n-data
    }
    auto deepestMoving = [&](std::initializer_list<const char*> names) -> size_t {
        size_t slot = namedChain.size();
        for (const char* nm : names) {
            if (posSpan(nm) <= 1e-6) continue;
            for (size_t i = 0; i < namedChain.size(); ++i) {
                if (namedChain[i].first == nm) slot = i;
            }
        }
        return slot;
    };
    size_t placementEnd = deepestMoving({"全ての親", "センター", "グルーブ"}); // i18n-data
    if (placementEnd == namedChain.size()) placementEnd = deepestMoving({"腰", "下半身"}); // i18n-data
    if (placementEnd != namedChain.size()) rootEnd = placementEnd;

    // If pelvis bone is 腰/下半身 and upper root chain rotates or moves, track pelvis world position.
    {
        bool upperMoves = false;
        for (const char* nm : {"全ての親", "センター", "グルーブ"}) { // i18n-data
            if (posSpan(nm) > 1e-6) upperMoves = true;
            if (const VmdBoneTrack* tr = findTrack(nm)) {
                for (const auto& k : tr->keys) {
                    if (std::fabs(k.rotation.x) > 1e-5 || std::fabs(k.rotation.y) > 1e-5 || std::fabs(k.rotation.z) > 1e-5) {
                        upperMoves = true;
                        break;
                    }
                }
            }
        }
        const char* pelSrc = hasWaist ? "腰" : (hasHip ? "下半身" : nullptr); // i18n-data
        if (pelSrc && (upperMoves || posSpan(pelSrc) > 1e-6)) {
            for (size_t s = 0; s < namedChain.size(); ++s) {
                if (namedChain[s].first == pelSrc) { rootEnd = s; break; }
            }
        }
    }
    const uint32_t rootTrackRefIdx = namedChain.empty() ? 0u : namedChain[rootEnd].second;
    const std::string rootTrackName = namedChain.empty() ? std::string{} : namedChain[rootEnd].first;

    // ---- Target-Side 2-Bone Leg IK Setup ----
    struct LegSolve {
        int thigh{-1}, calf{-1}, foot{-1};
        int thighParent{-1};
        std::vector<int> thighToCalfMid;
        std::vector<int> calfToFootMid;
        size_t thighTrackIdx{0}, calfTrackIdx{0}, footTrackIdx{0};
        size_t srcHip{0}, srcKnee{0}, srcAnkle{0};
        size_t waistIdx{0};
        bool haveWaist{};
        Vec3 hipFromWaistRest{};
        double thighLen{};
        double shinLen{};
        Vec3 thighLocalAxis{0.0, -1.0, 0.0};
        Vec3 calfLocalAxis{0.0, -1.0, 0.0};
    };
    std::vector<LegSolve> legSolves;
    const int pelvisIdx = rig.BoneOf(TargetRole::Pelvis);
    const int rootIdx = rig.BoneOf(TargetRole::Root);

    auto collectMidChain = [&](int upper, int lower) -> std::vector<int> {
        std::vector<int> mid;
        int cur = target.bones[static_cast<size_t>(lower)].parent;
        for (int guard = 0; guard < 16 && cur >= 0 && cur != upper; ++guard) {
            mid.push_back(cur);
            cur = target.bones[static_cast<size_t>(cur)].parent;
        }
        if (cur != upper) mid.clear();
        else std::reverse(mid.begin(), mid.end());
        return mid;
    };

    if (options.targetLegIk && poseReady && pelvisIdx >= 0) {
        for (int side = 0; side < 2; ++side) {
            const std::string jp = (side == 0) ? "左" : "右"; // i18n-data
            const int thigh = rig.BoneOf(side == 0 ? TargetRole::ThighL : TargetRole::ThighR);
            const int calf  = rig.BoneOf(side == 0 ? TargetRole::CalfL : TargetRole::CalfR);
            const int foot  = rig.BoneOf(side == 0 ? TargetRole::FootL : TargetRole::FootR);
            if (thigh < 0 || calf < 0 || foot < 0) continue;
            if (targetToTableIdx.count(thigh) == 0 || targetToTableIdx.count(calf) == 0 ||
                targetToTableIdx.count(foot) == 0) {
                continue;
            }
            if (!isAncestorOf(thigh, calf) || !isAncestorOf(calf, foot)) continue;
            const ReferenceBone* srcHip   = FindRefBone(reference, jp + "足"); // i18n-data
            if (!srcHip) srcHip = FindRefBone(reference, jp + "足D"); // i18n-data
            const ReferenceBone* srcKnee  = FindRefBone(reference, jp + "ひざ"); // i18n-data
            if (!srcKnee) srcKnee = FindRefBone(reference, jp + "ひざD"); // i18n-data
            const ReferenceBone* srcAnkle = FindRefBone(reference, jp + "足首"); // i18n-data
            if (!srcAnkle) srcAnkle = FindRefBone(reference, jp + "足首D"); // i18n-data
            if (!srcHip || !srcKnee || !srcAnkle) continue;

            // Measure actual segment lengths & local bone axes directly from the Unity target skeleton.
            const Vec3 thighToCalfWorld = rig.restPos[static_cast<size_t>(calf)] - rig.restPos[static_cast<size_t>(thigh)];
            const Vec3 calfToFootWorld  = rig.restPos[static_cast<size_t>(foot)] - rig.restPos[static_cast<size_t>(calf)];
            const double tLen = thighToCalfWorld.Length();
            const double sLen = calfToFootWorld.Length();
            if (!(tLen > 1e-4) || !(sLen > 1e-4)) continue;

            LegSolve ls{};
            ls.thigh = thigh;
            ls.calf = calf;
            ls.foot = foot;
            ls.thighParent = target.bones[static_cast<size_t>(thigh)].parent;
            ls.thighToCalfMid = collectMidChain(thigh, calf);
            ls.calfToFootMid = collectMidChain(calf, foot);
            ls.thighTrackIdx = targetToTableIdx[thigh];
            ls.calfTrackIdx = targetToTableIdx[calf];
            ls.footTrackIdx = targetToTableIdx[foot];
            ls.srcHip = srcHip->index;
            ls.srcKnee = srcKnee->index;
            ls.srcAnkle = srcAnkle->index;
            ls.thighLen = tLen;
            ls.shinLen = sLen;
            // Local axis along the bone in the bone's rest frame (rig-agnostic!).
            ls.thighLocalAxis = rig.restRot[static_cast<size_t>(thigh)].Conjugate().Rotate(thighToCalfWorld.Normalized()).Normalized();
            ls.calfLocalAxis  = rig.restRot[static_cast<size_t>(calf)].Conjugate().Rotate(calfToFootWorld.Normalized()).Normalized();

            if (!rootTrackName.empty()) {
                if (const ReferenceBone* waist = FindRefBone(reference, rootTrackName)) {
                    ls.waistIdx = waist->index;
                    ls.haveWaist = true;
                    ls.hipFromWaistRest = srcHip->position - waist->position;
                }
            }
            legSolves.push_back(ls);
        }
    }

    // ---- Leaf Forearm Twist Helpers (distribute wrist twist cleanly without over-twisting) ----
    struct TwistSlot {
        int twistIdx{-1};
        int foreIdx{-1};
        int handIdx{-1};
        Vec3 parentAxis{1.0, 0.0, 0.0};    // forearm axis in twist bone's parent local frame
        Vec3 handLocalAxis{1.0, 0.0, 0.0}; // forearm axis in hand's local frame
        Vec3 srcMmdAxis{1.0, 0.0, 0.0};    // MMD forearm axis in MMD frame
        std::string sourceTwistName;
        double weight{0.5};
    };
    std::vector<TwistSlot> twistSlots;
    for (int side = 0; side < 2; ++side) {
        const std::string jp = (side == 0) ? "左" : "右"; // i18n-data
        const int foreIdx = rig.BoneOf(side == 0 ? TargetRole::ForearmL : TargetRole::ForearmR);
        const int handIdx = rig.BoneOf(side == 0 ? TargetRole::HandL : TargetRole::HandR);
        if (foreIdx < 0 || handIdx < 0) continue;
        const Vec3 foreVec = rig.restPos[static_cast<size_t>(handIdx)] - rig.restPos[static_cast<size_t>(foreIdx)];
        if (foreVec.LengthSq() < 1e-6) continue;
        const Vec3 foreLocalAxis = rig.restRot[static_cast<size_t>(foreIdx)].Conjugate().Rotate(foreVec.Normalized()).Normalized();
        const Vec3 twistWorldAxis = restCrot[static_cast<size_t>(foreIdx)].Rotate(foreLocalAxis).Normalized();
        const Vec3 handLocalAxis = restCrot[static_cast<size_t>(handIdx)].Conjugate().Rotate(twistWorldAxis).Normalized();
        Vec3 srcMmdAxis = Ainv.Rotate(twistWorldAxis).Normalized();
        if (const auto rit = refDirInChar.find(CanonicalMmdBoneName(jp + "ひじ")); rit != refDirInChar.end()) { // i18n-data
            srcMmdAxis = Ainv.Rotate(rit->second).Normalized();
        }
        const auto& twList = leafForearmTwists[static_cast<size_t>(side)];
        for (size_t k = 0; k < twList.size(); ++k) {
            const int tw = twList[k];
            if (usedTargetBones.count(tw) != 0) continue;
            const int twParent = target.bones[static_cast<size_t>(tw)].parent;
            const Quat twParentRest = (twParent >= 0 && static_cast<size_t>(twParent) < nBones)
                                          ? restCrot[static_cast<size_t>(twParent)]
                                          : target.goToRootParentRot;
            const Vec3 parentAxis = twParentRest.Conjugate().Rotate(twistWorldAxis).Normalized({1.0, 0.0, 0.0});
            usedTargetBones.insert(tw);
            twistSlots.push_back({
                tw, foreIdx, handIdx, parentAxis, handLocalAxis, srcMmdAxis,
                jp + "手捩", (k == 0) ? 0.5 : 0.25 // i18n-data
            });
        }
    }

    // ---- Unmapped Body / Corrective / Twist Hold List ----
    // Hold remaining structural body/twist/corrective bones at clean bindLocalRot so the game's
    // live idle animation never fights MMD poses while secondary hair/cloth/skirt/breast physics stay free.
    for (size_t i = 0; i < nBones; ++i) {
        if (usedTargetBones.count(static_cast<int>(i)) != 0) continue;
        const auto& b = target.bones[i];
        if (!b.hasBindPose || IsNonStructuralBone(b.name)) continue;
        if (IsBodyBoneName(b.name, false) || IsTwistOrHelperBone(b.name)) {
            out.rigidHoldBones.push_back(static_cast<int>(i));
        }
    }

    // ---- Prepare Output Frame Buffers ----
    const double fps = (options.fps > 1.0 && options.fps <= 120.0) ? options.fps : 30.0;
    const uint32_t maxVmdFrame = std::max(vmd.maxBoneFrame, vmd.maxMorphFrame);
    const double durationSec = static_cast<double>(maxVmdFrame) / 30.0;
    const uint32_t frameCount = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::llround(durationSec * fps)) + 1u, 1u, kMaxRetargetFrames);

    const Quat rootParentRestRot = (rootIdx >= 0 && static_cast<size_t>(rootIdx) < nBones &&
                                    target.bones[static_cast<size_t>(rootIdx)].parent >= 0)
                                       ? restCrot[static_cast<size_t>(target.bones[static_cast<size_t>(rootIdx)].parent)]
                                       : target.goToRootParentRot;
    const Quat charToRootParent = rootParentRestRot.Conjugate();

    out.fps = fps;
    out.frameCount = frameCount;
    out.unitMetersPerMmdUnit = unitScale;
    out.mmdLegLength = mmdLegLen;
    out.targetLegLength = targetLegLen;
    out.rootBoneIndex = rootIdx;
    out.rootRestLocalPos = (rootIdx >= 0 && static_cast<size_t>(rootIdx) < nBones)
                               ? target.bones[static_cast<size_t>(rootIdx)].localPos
                               : Vec3{};
    out.charToRootParentRot = charToRootParent;
    out.rootTranslationsLocal.reserve(frameCount);
    out.rootTranslationsChar.reserve(frameCount);
    out.morphTracks = vmd.morphTracks;
    out.embeddedCameraKeys = vmd.cameraKeys;

    out.boneTracks.reserve(table.size() + twistSlots.size());
    for (const auto& entry : table) {
        MappedBoneTrack mt{};
        mt.targetBoneIndex = entry.targetIndex;
        mt.targetBoneName = target.bones[static_cast<size_t>(entry.targetIndex)].name;
        mt.restLocalRot = restLocal[static_cast<size_t>(entry.targetIndex)];
        mt.part = entry.part;
        mt.frames.reserve(frameCount);
        out.boneTracks.push_back(std::move(mt));
    }
    const size_t twistTrackStart = out.boneTracks.size();
    for (const auto& ts : twistSlots) {
        MappedBoneTrack mt{};
        mt.targetBoneIndex = ts.twistIdx;
        mt.targetBoneName = target.bones[static_cast<size_t>(ts.twistIdx)].name;
        mt.restLocalRot = restLocal[static_cast<size_t>(ts.twistIdx)];
        mt.part = MotionPart::Arm;
        mt.frames.reserve(frameCount);
        out.boneTracks.push_back(std::move(mt));
    }

    // Optional Spine base offset when hipOnPelvis is enabled (prevents spine base dragging with 下半身).
    const int spineIdx = rig.BoneOf(TargetRole::Spine);
    const bool useSpineOffset = options.hipOnPelvis && hasHip && poseReady &&
                                pelvisIdx >= 0 && spineIdx >= 0 &&
                                target.bones[static_cast<size_t>(spineIdx)].parent == pelvisIdx;
    size_t spineOffsetTrackIdx = std::numeric_limits<size_t>::max();
    if (useSpineOffset) {
        spineOffsetTrackIdx = out.boneOffsetTracks.size();
        BoneOffsetTrack bot{};
        bot.targetBoneIndex = spineIdx;
        bot.restLocalPos = target.bones[static_cast<size_t>(spineIdx)].localPos;
        bot.frames.reserve(frameCount);
        out.boneOffsetTracks.push_back(std::move(bot));
    }
    // Hold Pelvis localPosition at its clean bind offset when distinct from floor Root so
    // live game idle animation never shifts/bobs Pelvis away from the Root->Pelvis lever.
    if (distinctRootAndPelvis && pelvisIdx >= 0 && target.bones[static_cast<size_t>(pelvisIdx)].hasBindPose) {
        BoneOffsetTrack pelOff{};
        pelOff.targetBoneIndex = pelvisIdx;
        pelOff.restLocalPos = target.bones[static_cast<size_t>(pelvisIdx)].localPos;
        pelOff.frames.assign(frameCount, pelOff.restLocalPos);
        out.boneOffsetTracks.push_back(std::move(pelOff));
    }

    const auto hipRefIt = ikPose.byCanonicalName.find(CanonicalMmdBoneName("下半身")); // i18n-data
    const std::optional<size_t> hipRefIdx = (hipRefIt != ikPose.byCanonicalName.end())
                                                ? std::optional<size_t>{hipRefIt->second}
                                                : std::nullopt;

    // Root -> Pelvis lever subtraction if Pelvis is a direct child of Root.
    const bool hasRootPelvisLever = distinctRootAndPelvis &&
                                    target.bones[static_cast<size_t>(pelvisIdx)].parent == rootIdx;
    const Vec3 rootPelvisLocalPos = hasRootPelvisLever
                                        ? target.bones[static_cast<size_t>(pelvisIdx)].localPos
                                        : Vec3{};
    Vec3 rootBaseMmd{};
    bool haveRootBase = false;
    Vec3 rootLeverBase{};
    bool haveRootLeverBase = false;

    std::vector<Quat> frameLocal = restLocal;
    std::vector<Quat> frameWorldRot(nBones, Quat::Identity());

    // ---- Main Per-Frame Retarget Loop ----
    for (uint32_t fi = 0; fi < frameCount; ++fi) {
        const double vmdFrame = (frameCount > 1)
                                    ? (static_cast<double>(fi) * 30.0 / fps)
                                    : 0.0;

        if (poseReady) {
            ikPose.SetFrame(ikTrackByBone, vmdFrame);
            if (solveIk) {
                std::unordered_set<std::string> disabledIk;
                if (options.ikMode == IkMode::FollowMotion) {
                    for (const auto& [ikName, records] : vmd.ikStates) {
                        bool on = true;
                        bool seen = false;
                        for (const auto& rec : records) {
                            if (static_cast<double>(rec.frame) > vmdFrame) break;
                            on = rec.enabled;
                            seen = true;
                        }
                        if (seen && !on) disabledIk.insert(ikName);
                    }
                }
                ikPose.Solve(disabledIk, out.report.hasCompleteLegFk);
            }
        }

        auto sampleSource = [&](const std::string& srcName, Quat& qOut) -> bool {
            const std::string canon = CanonicalMmdBoneName(srcName);
            if (solveIk) {
                if (const auto it = ikPose.byCanonicalName.find(canon); it != ikPose.byCanonicalName.end()) {
                    qOut = ikPose.rot[it->second];
                    return true;
                }
            }
            if (const VmdBoneTrack* tr = findTrack(srcName)) {
                Vec3 unusedPos{};
                return SampleBoneTrack(*tr, vmdFrame, qOut, unusedPos);
            }
            return false;
        };

        // Reset unmapped bones to aligned restLocal every frame.
        frameLocal = restLocal;

        // 1. Mapped bone local rotations.
        for (size_t ti = 0; ti < table.size(); ++ti) {
            const auto& entry = table[ti];
            Quat motion = Quat::Identity();
            bool any = false;
            auto compose = [&](const std::string& src, bool conj) {
                Quat q = Quat::Identity();
                if (!sampleSource(src, q)) return;
                motion = (motion * (conj ? q.Conjugate() : q)).Normalized();
                any = true;
            };
            for (const auto& s : entry.prefixInverse) compose(s, true);
            for (const auto& s : entry.sources) compose(s, false);
            for (const auto& s : entry.inverse) compose(s, true);
            if (!any) motion = Quat::Identity();

            const int boneIdx = entry.targetIndex;
            const int parentIdx = target.bones[static_cast<size_t>(boneIdx)].parent;
            const Quat parentRest = (parentIdx >= 0 && static_cast<size_t>(parentIdx) < nBones)
                                        ? restCrot[static_cast<size_t>(parentIdx)]
                                        : target.goToRootParentRot;
            const Quat delta = (A * motion * Ainv).Normalized();
            const Quat localRot = (parentRest.Conjugate() * delta * restCrot[static_cast<size_t>(boneIdx)]).Normalized();
            frameLocal[static_cast<size_t>(boneIdx)] = localRot;
            out.boneTracks[ti].frames.push_back(localRot);
        }

        // 2. Spine counter-offset when hipOnPelvis is active.
        if (useSpineOffset && hipRefIdx.has_value()) {
            const Quat hipLocal = ikPose.rot[*hipRefIdx];
            const Quat hipDelta = (A * hipLocal * Ainv).Normalized();
            const Quat pelRest = restCrot[static_cast<size_t>(pelvisIdx)];
            const Quat counter = (pelRest.Conjugate() * hipDelta.Conjugate() * pelRest).Normalized();
            const Vec3 spinePos = counter.Rotate(target.bones[static_cast<size_t>(spineIdx)].localPos);
            out.boneOffsetTracks[spineOffsetTrackIdx].frames.push_back(spinePos);
        }

        // 3. Root translation delta (scaled to Unity meters, relative to frame 0).
        Vec3 rootDeltaChar{};
        if (poseReady && !namedChain.empty()) {
            Vec3 absMmd = ikPose.wpos[rootTrackRefIdx];
            if (options.feetAnchor) {
                Vec3 sum{};
                int anchors = 0;
                for (const char* footNm : {"左足首", "右足首"}) { // i18n-data
                    if (const ReferenceBone* fb = FindRefBone(reference, footNm)) {
                        sum += ikPose.wpos[fb->index];
                        ++anchors;
                    }
                }
                if (anchors > 0) absMmd = sum / static_cast<double>(anchors);
            }
            if (!haveRootBase) {
                rootBaseMmd = absMmd;
                haveRootBase = true;
            }
            rootDeltaChar = A.Rotate((absMmd - rootBaseMmd) * unitScale);

            if (hasRootPelvisLever && rootIdx >= 0) {
                const Quat rootWorldThisFrame = (rootParentRestRot * frameLocal[static_cast<size_t>(rootIdx)]).Normalized();
                const Vec3 lever = rootWorldThisFrame.Rotate(rootPelvisLocalPos);
                if (!haveRootLeverBase) {
                    rootLeverBase = lever;
                    haveRootLeverBase = true;
                }
                rootDeltaChar -= (lever - rootLeverBase);
            }
        }
        out.rootTranslationsChar.push_back(rootDeltaChar);
        out.rootTranslationsLocal.push_back(charToRootParent.Rotate(rootDeltaChar));

        // 4. Build component-space world rotations topologically + run target-side 2-bone leg IK.
        for (size_t i : topo) {
            const int p = target.bones[i].parent;
            const Quat parentW = (p >= 0 && static_cast<size_t>(p) < nBones)
                                     ? frameWorldRot[static_cast<size_t>(p)]
                                     : target.goToRootParentRot;

            // Solve target leg right when we reach its thigh (so thigh's parent world rot is already live).
            for (const LegSolve& ls : legSolves) {
                if (static_cast<int>(i) != ls.thigh) continue;
                const Quat thighParentW = (ls.thighParent >= 0 && static_cast<size_t>(ls.thighParent) < nBones)
                                              ? frameWorldRot[static_cast<size_t>(ls.thighParent)]
                                              : target.goToRootParentRot;
                const Vec3 hipNow = ikPose.wpos[ls.srcHip];
                auto relChar = [&](size_t refBoneIdx) -> Vec3 {
                    return A.Rotate((ikPose.wpos[refBoneIdx] - hipNow) * unitScale);
                };
                Vec3 goal = relChar(ls.srcAnkle);
                const Vec3 kneeRef = relChar(ls.srcKnee);

                if (ls.haveWaist) {
                    const Vec3 waistNow = ikPose.wpos[ls.waistIdx];
                    // Subtract heading-rotated rest offset so pure 180-deg yaw turns never create lateral leg cross.
                    const Quat waistYaw = TwistAboutAxis(ikPose.wrot[ls.waistIdx], {0.0, 1.0, 0.0});
                    const Vec3 arc = A.Rotate(((hipNow - waistNow) - waistYaw.Rotate(ls.hipFromWaistRest)) * unitScale);
                    goal += arc;
                }

                const double reach = ls.thighLen + ls.shinLen;
                const double d = goal.Length();
                Vec3 thighDir{}, shinDir{};
                if (d < 1e-6) {
                    thighDir = {0.0, -1.0, 0.0};
                    shinDir = thighDir;
                } else if (d >= reach - 1e-6) {
                    thighDir = goal.Normalized();
                    shinDir = thighDir;
                } else {
                    const Vec3 u = goal / d;
                    const double cosT = std::clamp(
                        (ls.thighLen * ls.thighLen + d * d - ls.shinLen * ls.shinLen) /
                            (2.0 * ls.thighLen * d),
                        -1.0, 1.0);
                    const double sinT = std::sqrt(std::max(0.0, 1.0 - cosT * cosT));
                    Vec3 normal = kneeRef.Cross(goal);
                    normal = (normal.LengthSq() > 1e-10) ? normal.Normalized() : Vec3{1.0, 0.0, 0.0};
                    const Vec3 inPlane = normal.Cross(u).Normalized();
                    const Vec3 dir1 = (u * cosT + inPlane * sinT).Normalized();
                    const Vec3 dir2 = (u * cosT - inPlane * sinT).Normalized();
                    const Vec3 k1 = dir1 * ls.thighLen;
                    const Vec3 k2 = dir2 * ls.thighLen;
                    const Vec3 knee = ((k1 - kneeRef).LengthSq() <= (k2 - kneeRef).LengthSq()) ? k1 : k2;
                    thighDir = knee.Normalized();
                    shinDir = (goal - knee).Normalized();
                }

                Quat midThighToCalf = Quat::Identity();
                for (int midIdx : ls.thighToCalfMid) {
                    midThighToCalf = (midThighToCalf * frameLocal[static_cast<size_t>(midIdx)]).Normalized();
                }
                Quat midCalfToFoot = Quat::Identity();
                for (int midIdx : ls.calfToFootMid) {
                    midCalfToFoot = (midCalfToFoot * frameLocal[static_cast<size_t>(midIdx)]).Normalized();
                }

                const Quat mappedThigh = (thighParentW * frameLocal[static_cast<size_t>(ls.thigh)]).Normalized();
                const Quat mappedCalf  = (mappedThigh * midThighToCalf * frameLocal[static_cast<size_t>(ls.calf)]).Normalized();
                const Quat mappedFoot  = (mappedCalf * midCalfToFoot * frameLocal[static_cast<size_t>(ls.foot)]).Normalized();

                const Quat thighW = (SwingRotation(mappedThigh.Rotate(ls.thighLocalAxis), thighDir) * mappedThigh).Normalized();
                const Quat calfW  = (SwingRotation(mappedCalf.Rotate(ls.calfLocalAxis), shinDir) * mappedCalf).Normalized();

                const Quat calfParentW = (thighW * midThighToCalf).Normalized();
                const Quat footParentW = (calfW * midCalfToFoot).Normalized();

                frameLocal[static_cast<size_t>(ls.thigh)] = (thighParentW.Conjugate() * thighW).Normalized();
                frameLocal[static_cast<size_t>(ls.calf)]  = (calfParentW.Conjugate() * calfW).Normalized();
                frameLocal[static_cast<size_t>(ls.foot)]  = (footParentW.Conjugate() * mappedFoot).Normalized();

                out.boneTracks[ls.thighTrackIdx].frames.back() = frameLocal[static_cast<size_t>(ls.thigh)];
                out.boneTracks[ls.calfTrackIdx].frames.back()  = frameLocal[static_cast<size_t>(ls.calf)];
                out.boneTracks[ls.footTrackIdx].frames.back()  = frameLocal[static_cast<size_t>(ls.foot)];
            }

            frameWorldRot[i] = (parentW * frameLocal[i]).Normalized();
        }

        // 5. Leaf forearm twist helper bones (distribute wrist pronation/supination smoothly).
        for (size_t k = 0; k < twistSlots.size(); ++k) {
            const auto& ts = twistSlots[k];
            double twistRad = 0.0;
            bool authored = false;
            if (const VmdBoneTrack* twTrack = findTrack(ts.sourceTwistName)) {
                Quat qSrc = Quat::Identity();
                Vec3 unusedPos{};
                if (SampleBoneTrack(*twTrack, vmdFrame, qSrc, unusedPos)) {
                    const Quat qTw = TwistAboutAxis(qSrc, ts.srcMmdAxis);
                    const double sinHalf = qTw.x * ts.srcMmdAxis.x + qTw.y * ts.srcMmdAxis.y + qTw.z * ts.srcMmdAxis.z;
                    double ang = 2.0 * std::atan2(sinHalf, qTw.w);
                    while (ang > kPi) ang -= 2.0 * kPi;
                    while (ang < -kPi) ang += 2.0 * kPi;
                    twistRad = ang * ts.weight;
                    authored = true;
                }
            }
            if (!authored) {
                const Quat handDeltaLocal = (restLocal[static_cast<size_t>(ts.handIdx)].Conjugate() *
                                             frameLocal[static_cast<size_t>(ts.handIdx)]).Normalized();
                const double sinHalf = handDeltaLocal.x * ts.handLocalAxis.x +
                                       handDeltaLocal.y * ts.handLocalAxis.y +
                                       handDeltaLocal.z * ts.handLocalAxis.z;
                double ang = 2.0 * std::atan2(sinHalf, handDeltaLocal.w);
                while (ang > kPi) ang -= 2.0 * kPi;
                while (ang < -kPi) ang += 2.0 * kPi;
                twistRad = std::clamp(ang * ts.weight, -0.5, 0.5);
            }
            const Quat share = Quat::FromAxisAngle(ts.parentAxis, twistRad);
            const Quat localTw = (share * restLocal[static_cast<size_t>(ts.twistIdx)]).Normalized();
            out.boneTracks[twistTrackStart + k].frames.push_back(localTw);
        }
    }

    out.mappedBoneCount = static_cast<uint32_t>(out.boneTracks.size());
    out.matchedTargetRoles = static_cast<uint32_t>(table.size());
    return true;
}

// ---- Runtime Frame Sampling Helpers -----------------------------------------

Quat ApplyAmplitudeToLocalRot(const Quat& restLocal, const Quat& posedLocal, float scale) noexcept {
    if (std::fabs(scale - 1.f) < 1e-4f) return posedLocal;
    const Quat delta = (restLocal.Conjugate() * posedLocal).Normalized();
    Quat d = delta;
    if (d.w < 0.0) d = {-d.w, -d.x, -d.y, -d.z};
    const Vec3 v{d.x, d.y, d.z};
    const double sinHalf = v.Length();
    if (!(sinHalf > 1e-8)) return restLocal;
    const double angle = 2.0 * std::atan2(sinHalf, d.w) * static_cast<double>(std::clamp(scale, 0.f, 2.f));
    return (restLocal * Quat::FromAxisAngle(v / sinHalf, angle)).Normalized();
}

Quat SampleMappedBoneFrame(const MappedBoneTrack& track, double frame, const MotionAmplitude& amp) noexcept {
    if (track.frames.empty()) return track.restLocalRot;
    Quat sampled{};
    if (frame <= 0.0 || track.frames.size() == 1) {
        sampled = track.frames.front();
    } else {
        const double maxIdx = static_cast<double>(track.frames.size() - 1);
        if (frame >= maxIdx) {
            sampled = track.frames.back();
        } else {
            const size_t i0 = static_cast<size_t>(std::floor(frame));
            const size_t i1 = std::min(i0 + 1, track.frames.size() - 1);
            const double frac = frame - static_cast<double>(i0);
            sampled = SlerpQuat(track.frames[i0], track.frames[i1], frac);
        }
    }

    float gain = amp.master;
    switch (track.part) {
        case MotionPart::Spine:
        case MotionPart::Pelvis:
            gain *= amp.spine;
            break;
        case MotionPart::Arm:
        case MotionPart::Hand:
            gain *= amp.arms;
            break;
        case MotionPart::Leg:
            gain *= amp.legs;
            break;
        case MotionPart::Head:
            gain *= amp.head;
            break;
        case MotionPart::Root:
        default:
            break;
    }
    return ApplyAmplitudeToLocalRot(track.restLocalRot, sampled, gain);
}

Vec3 SampleVec3Track(const std::vector<Vec3>& frames, double frame) noexcept {
    if (frames.empty()) return {};
    if (frame <= 0.0 || frames.size() == 1) return frames.front();
    const double maxIdx = static_cast<double>(frames.size() - 1);
    if (frame >= maxIdx) return frames.back();
    const size_t i0 = static_cast<size_t>(std::floor(frame));
    const size_t i1 = std::min(i0 + 1, frames.size() - 1);
    const double frac = frame - static_cast<double>(i0);
    return frames[i0] + (frames[i1] - frames[i0]) * frac;
}

Vec3 FilterRootTravel(const Vec3& rawCharDelta, const MotionTravel& travel,
                      const Quat& charToRootParentRot) noexcept {
    if (!travel.applyRootMotion) return {};
    const double pScale = travel.lockPlanar ? 0.0 : static_cast<double>(std::clamp(travel.planarScale, 0.f, 3.f));
    const double vScale = static_cast<double>(std::clamp(travel.verticalScale, 0.f, 3.f));
    const Vec3 filteredChar{
        rawCharDelta.x * pScale,
        rawCharDelta.y * vScale,
        rawCharDelta.z * pScale
    };
    return charToRootParentRot.Rotate(filteredChar);
}

}  // namespace betterpose::mmd

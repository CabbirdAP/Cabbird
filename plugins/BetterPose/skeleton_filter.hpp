#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace betterpose {

inline std::string ToLowerAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

inline bool ContainsAscii(std::string_view haystackLower, std::string_view needleLower) {
    return haystackLower.find(needleLower) != std::string_view::npos;
}

// Identifies common facial bones so the user can toggle them independently of body-only mode.
inline bool IsFaceBoneName(std::string_view name) {
    const std::string low = ToLowerAscii(name);
    static constexpr std::string_view kFaceTokens[]{
        "eye", "brow", "eyelid", "lid_", "_lid", "lash", "pupil", "iris",
        "lip", "mouth", "cheek", "nose", "nostril", "tongue", "jaw",
        "tooth", "teeth", "chin", "face", "ear_inner", "ear_l", "ear_r", "smile"
    };
    for (std::string_view tok : kFaceTokens) {
        if (ContainsAscii(low, tok)) return true;
    }
    return false;
}

// Body-only filter: hides secondary physics, hair, cloth, skirts, props, twist helpers, and sockets.
// When `showFaceBones` is false, facial bones are also hidden.
inline bool IsBodyBoneName(std::string_view name, bool showFaceBones = false) {
    if (name.empty()) return false;
    const std::string low = ToLowerAscii(name);

    static constexpr std::string_view kExcludedTokens[]{
        "hair", "bang", "ahoge", "ponytail", "twintail", "braid", "fringe",
        "cloth", "skirt", "dress", "cape", "cloak", "coat", "ribbon", "bow_",
        "scarf", "sleeve", "cuff", "hem", "belt", "sash", "apron", "tie_",
        "tail", "wing", "feather", "fluff", "fur",
        "breast", "bust", "boob", "oppai", "mune",
        "weapon", "sword", "gun", "prop", "attach", "socket", "mount", "fx_", "eff_",
        "spring", "jiggle", "dynamic", "magica", "kawaii", "pendulum", "physics",
        "camera", "ik_", "_ik", "pole", "helper", "twist", "roll_", "corrective",
        "collider", "hitbox", "dummy", "nub", "_end", "point_"
    };
    for (std::string_view tok : kExcludedTokens) {
        if (ContainsAscii(low, tok)) return false;
    }

    // Animal ears / accessory ears often use tokens like ear_01, kemono, etc.
    if ((ContainsAscii(low, "ear") && (ContainsAscii(low, "01") || ContainsAscii(low, "02") ||
                                       ContainsAscii(low, "03") || ContainsAscii(low, "cat") ||
                                       ContainsAscii(low, "fox") || ContainsAscii(low, "animal")))) {
        return false;
    }

    if (!showFaceBones && IsFaceBoneName(low)) {
        return false;
    }
    return true;
}

enum class IkLimbKind : int {
    None = 0,
    HandL,
    HandR,
    FootL,
    FootR
};

enum class BoneSide : int {
    Center = 0,
    Left,
    Right
};

inline BoneSide DetectBoneSide(std::string_view name) {
    const std::string low = ToLowerAscii(name);

    // Check explicit words first.
    if (ContainsAscii(low, "left")) return BoneSide::Left;
    if (ContainsAscii(low, "right")) return BoneSide::Right;

    // Check delimited single-letter markers: "_l_", "_l", ".l", " l ", "l_", "-l".
    auto hasToken = [&](char ch) -> bool {
        const size_t n = low.size();
        for (size_t i = 0; i < n; ++i) {
            if (low[i] != ch) continue;
            const bool prevDelim = (i == 0) || low[i - 1] == '_' || low[i - 1] == '.' ||
                                   low[i - 1] == ' ' || low[i - 1] == '-';
            const bool nextDelim = (i + 1 == n) || low[i + 1] == '_' || low[i + 1] == '.' ||
                                   low[i + 1] == ' ' || low[i + 1] == '-' ||
                                   std::isdigit(static_cast<unsigned char>(low[i + 1]));
            if (prevDelim && nextDelim) return true;
        }
        return false;
    };
    if (hasToken('l')) return BoneSide::Left;
    if (hasToken('r')) return BoneSide::Right;
    return BoneSide::Center;
}

inline IkLimbKind DetectIkEndBone(std::string_view name) {
    const std::string low = ToLowerAscii(name);
    if (ContainsAscii(low, "finger") || ContainsAscii(low, "thumb") ||
        ContainsAscii(low, "index") || ContainsAscii(low, "middle") ||
        ContainsAscii(low, "ring") || ContainsAscii(low, "pinky") ||
        ContainsAscii(low, "little") || ContainsAscii(low, "toe") ||
        ContainsAscii(low, "twist") || ContainsAscii(low, "weapon") ||
        ContainsAscii(low, "prop") || ContainsAscii(low, "attach")) {
        return IkLimbKind::None;
    }

    const BoneSide side = DetectBoneSide(name);
    if (side == BoneSide::Center) return IkLimbKind::None;

    const bool isHand = ContainsAscii(low, "hand") || ContainsAscii(low, "wrist");
    const bool isFoot = ContainsAscii(low, "foot") || ContainsAscii(low, "ankle");
    if (isHand && !isFoot) {
        return side == BoneSide::Left ? IkLimbKind::HandL : IkLimbKind::HandR;
    }
    if (isFoot && !isHand) {
        return side == BoneSide::Left ? IkLimbKind::FootL : IkLimbKind::FootR;
    }
    return IkLimbKind::None;
}

struct IkChain {
    IkLimbKind kind{IkLimbKind::None};
    int upper{-1};
    int lower{-1};
    int end{-1};
    bool IsValid() const noexcept { return kind != IkLimbKind::None && upper >= 0 && lower >= 0 && end >= 0; }
};

// Skip intermediate twist/roll/helper bones when climbing the 2-bone limb chain.
inline bool IsTwistOrHelperBone(std::string_view name) {
    const std::string low = ToLowerAscii(name);
    return ContainsAscii(low, "twist") || ContainsAscii(low, "roll") ||
           ContainsAscii(low, "helper") || ContainsAscii(low, "corrective");
}

inline IkChain ResolveIkChain(int endBoneIndex,
                              const std::vector<std::string>& boneNames,
                              const std::vector<int>& parents) {
    IkChain chain;
    if (endBoneIndex < 0 || static_cast<size_t>(endBoneIndex) >= boneNames.size() ||
        boneNames.size() != parents.size()) {
        return chain;
    }
    const IkLimbKind kind = DetectIkEndBone(boneNames[static_cast<size_t>(endBoneIndex)]);
    if (kind == IkLimbKind::None) return chain;

    auto climbRealParent = [&](int idx) -> int {
        int cur = parents[static_cast<size_t>(idx)];
        int guard = 0;
        while (cur >= 0 && static_cast<size_t>(cur) < boneNames.size() && guard++ < 8) {
            if (!IsTwistOrHelperBone(boneNames[static_cast<size_t>(cur)])) return cur;
            cur = parents[static_cast<size_t>(cur)];
        }
        return -1;
    };

    const int lower = climbRealParent(endBoneIndex);
    if (lower < 0) return chain;
    const int upper = climbRealParent(lower);
    if (upper < 0) return chain;

    chain.kind = kind;
    chain.end = endBoneIndex;
    chain.lower = lower;
    chain.upper = upper;
    return chain;
}

enum class BoneCategoryFilter : int {
    All = 0,
    Spine,
    Head,
    Arms,
    Hands,
    Legs,
    Face
};

inline bool MatchesBoneCategory(std::string_view name, BoneCategoryFilter cat) {
    if (cat == BoneCategoryFilter::All) return true;
    const std::string low = ToLowerAscii(name);
    switch (cat) {
        case BoneCategoryFilter::Spine:
            return ContainsAscii(low, "pelvis") || ContainsAscii(low, "hips") ||
                   ContainsAscii(low, "hip") || ContainsAscii(low, "root") ||
                   ContainsAscii(low, "global") || ContainsAscii(low, "lowerback") ||
                   ContainsAscii(low, "upperback") || ContainsAscii(low, "midback") ||
                   ContainsAscii(low, "spine") || ContainsAscii(low, "chest") ||
                   ContainsAscii(low, "waist") || ContainsAscii(low, "torso");
        case BoneCategoryFilter::Head:
            return ContainsAscii(low, "neck") || ContainsAscii(low, "head");
        case BoneCategoryFilter::Arms:
            return ContainsAscii(low, "clavicle") || ContainsAscii(low, "shoulder") ||
                   ContainsAscii(low, "upperarm") || ContainsAscii(low, "arm") ||
                   ContainsAscii(low, "forearm") || ContainsAscii(low, "elbow");
        case BoneCategoryFilter::Hands:
            return ContainsAscii(low, "hand") || ContainsAscii(low, "wrist") ||
                   ContainsAscii(low, "finger") || ContainsAscii(low, "thumb") ||
                   ContainsAscii(low, "index") || ContainsAscii(low, "middle") ||
                   ContainsAscii(low, "ring") || ContainsAscii(low, "pinky") ||
                   ContainsAscii(low, "little");
        case BoneCategoryFilter::Legs:
            return ContainsAscii(low, "thigh") || ContainsAscii(low, "upperleg") ||
                   ContainsAscii(low, "leg") || ContainsAscii(low, "calf") ||
                   ContainsAscii(low, "knee") || ContainsAscii(low, "shin") ||
                   ContainsAscii(low, "foot") || ContainsAscii(low, "ankle") ||
                   ContainsAscii(low, "toe");
        case BoneCategoryFilter::Face:
            return IsFaceBoneName(low);
        default:
            return true;
    }
}

}  // namespace betterpose

#pragma once

#include "mmd_retarget.hpp"
#include "skeleton_filter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace betterpose::mmd {

struct MorphTargetSlot {
    int smrIndex{-1};
    int shapeIndex{-1};
    std::string name;
};

struct MorphBinding {
    size_t trackIndex{};
    int smrIndex{-1};
    int shapeIndex{-1};
    float scale{1.f};
};

// Normalize a BlendShape name by stripping common prefixes (Fcl_, BS_, Face_, M_) and separators.
inline std::string NormalizeMorphToken(std::string_view raw) {
    std::string low = ToLowerAscii(raw);
    static constexpr std::string_view kPrefixes[]{
        "blendshape.", "blendshapes.", "blendshape_", "fcl_", "face_", "bs_", "m_", "morph_", "exp_"
    };
    bool stripped = true;
    while (stripped) {
        stripped = false;
        for (std::string_view p : kPrefixes) {
            if (low.starts_with(p) && low.size() > p.size() + 1) {
                low.erase(0, p.size());
                stripped = true;
                break;
            }
        }
    }
    std::string compact;
    compact.reserve(low.size());
    for (unsigned char c : low) {
        if (c == '_' || c == '-' || c == ' ' || c == '.') continue;
        compact.push_back(static_cast<char>(c));
    }
    return compact;
}

struct MorphAliasEntry {
    std::string_view mmdName;
    std::string_view targetTokens[8];
    float scale{1.f};
};

// Standard MMD VMD facial morphs -> common Unity / VRM / ARKit / anime-game BlendShape aliases.
// Japanese strings below are VMD format identifiers (game/format data), not UI copy.
inline std::span<const MorphAliasEntry> StandardMorphAliasTable() noexcept {
    static constexpr MorphAliasEntry kTable[]{
        // Eyes: blink / smile / wink
        {"まばたき", {"eyeblink", "eyeblinkleft", "eyeblinkright", "eyeclose", "blink", "eye_close", "close", "eyelidclose"}, 1.f}, // i18n-data
        {"笑い", {"eyejoy", "eyesmile", "eyehappy", "joy", "smileeye", "eye_joy", "eye_smile", "happyeye"}, 1.f}, // i18n-data
        {"なごみ", {"eyerelax", "eyesmile", "eyeclose", "nagomi", "calm", "eye_joy"}, 0.85f}, // i18n-data
        {"ウィンク", {"eyeblinkleft", "eyewinkl", "eyewinkleft", "eyeclosel", "winkl", "winkleft", "eyeblinkl"}, 1.f}, // i18n-data
        {"ウィンク２", {"eyesmileleft", "eyejoyl", "eyewinkl", "eyeblinkleft", "winkl"}, 1.f}, // i18n-data
        {"ウィンク右", {"eyeblinkright", "eyewinkr", "eyewinkright", "eyecloser", "winkr", "winkright", "eyeblinkr"}, 1.f}, // i18n-data
        {"ｳｨﾝｸ２右", {"eyesmileright", "eyejoyr", "eyewinkr", "eyeblinkright", "winkr"}, 1.f}, // i18n-data
        {"ウィンク２右", {"eyesmileright", "eyejoyr", "eyewinkr", "eyeblinkright", "winkr"}, 1.f}, // i18n-data
        {"じと目", {"eyesquint", "eyesquintleft", "eyesquintright", "eyedown", "eyehalf", "eyefun", "eyejito"}, 1.f}, // i18n-data
        {"びっくり", {"eyewide", "eyewideleft", "eyewideright", "eyesurprised", "eyesurprise", "surprise"}, 1.f}, // i18n-data
        {"悲しい", {"eyesorrow", "eyesad", "sorrow", "sad", "eyecry"}, 1.f}, // i18n-data
        {"怒り", {"eyeangry", "angry", "eyeanger"}, 1.f}, // i18n-data
        {"きりっ", {"eyeangry", "eyesquint", "eyeserious", "serious"}, 0.7f}, // i18n-data
        {"はぅ", {"eyewince", "eyepain", "eyeclose", "eyeblink"}, 0.9f}, // i18n-data
        {"瞳小", {"eyepupilsmall", "pupilsmall", "irisshrink", "eyehighlight"}, 1.f}, // i18n-data

        // Mouth vowels (Japanese a/i/u/e/o)
        {"あ", {"mtha", "moutha", "aa", "a", "jawopen", "mouthopen", "vowel_a", "vrcva"}, 1.f}, // i18n-data
        {"あ２", {"mtha", "moutha", "jawopen", "mouthopen"}, 0.85f}, // i18n-data
        {"い", {"mthi", "mouthi", "ih", "ii", "i", "mouthsmile", "mouthstretch", "vrcvi"}, 1.f}, // i18n-data
        {"う", {"mthu", "mouthu", "ou", "uu", "u", "mouthfunnel", "mouthpucker", "vrcvu"}, 1.f}, // i18n-data
        {"え", {"mthe", "mouthe", "ee", "e", "mouthopen", "vrcve"}, 1.f}, // i18n-data
        {"お", {"mtho", "moutho", "oh", "oo", "o", "mouthfunnel", "vrcvo"}, 1.f}, // i18n-data
        {"ワ", {"mthjoy", "mouthsmile", "mouthhappy", "jawopen", "mtha", "moutha"}, 0.9f}, // i18n-data
        {"ω", {"mthcat", "mouthcat", "catmouth", "mthsmile", "mouthsmile"}, 1.f}, // i18n-data
        {"▲", {"mthangry", "mouthangry", "mthfrown", "mouthfrown", "mthsad"}, 1.f}, // i18n-data
        {"∧", {"mthsorrow", "mouthsad", "mouthfrown", "mouthfrownleft", "mouthfrownright"}, 1.f}, // i18n-data
        {"にやり", {"mthsmile", "mouthsmile", "mouthsmileleft", "mouthsmileright", "mthfun", "smile", "grin"}, 1.f}, // i18n-data
        {"にっこり", {"mthjoy", "mthsmile", "mouthsmile", "mouthsmileleft", "mouthsmileright", "happy", "smile"}, 1.f}, // i18n-data
        {"口角上げ", {"mouthsmileleft", "mouthsmileright", "mouthsmile", "mthsmile", "mthup"}, 0.85f}, // i18n-data
        {"口角下げ", {"mouthfrownleft", "mouthfrownright", "mouthfrown", "mthsad", "mthdown"}, 0.85f}, // i18n-data
        {"口広げ", {"mouthstretchleft", "mouthstretchright", "mouthwide", "mtha"}, 0.8f}, // i18n-data
        {"ぺろっ", {"tongueout", "mthtongue", "mouthtongue", "tongue"}, 1.f}, // i18n-data

        // Eyebrows
        {"真面目", {"brwserious", "browdown", "browdownleft", "browdownright", "browneutral"}, 0.8f}, // i18n-data
        {"困る", {"brwsorrow", "browsad", "browinnerup", "browtrouble", "sorrow"}, 1.f}, // i18n-data
        {"にこり", {"brwfun", "brwjoy", "brwhappy", "browsmile", "browup"}, 1.f}, // i18n-data
        {"怒り眉", {"brwangry", "browangry", "browdown", "browdownleft", "browdownright"}, 1.f}, // i18n-data
        {"上", {"brwup", "browup", "browinnerup", "browouterupleft", "browouterupright", "brwsurprised"}, 1.f}, // i18n-data
        {"下", {"brwdown", "browdown", "browdownleft", "browdownright"}, 1.f}, // i18n-data

        // Cheeks / Blush
        {"照れ", {"cheekblush", "blush", "cheek", "shy", "terere"}, 1.f}, // i18n-data
        {"赤面", {"cheekblush", "blush", "cheek", "redface"}, 1.f}, // i18n-data
    };
    return kTable;
}

inline std::vector<MorphBinding> BuildMorphBindings(const std::vector<VmdMorphTrack>& tracks,
                                                    const std::vector<MorphTargetSlot>& targetShapes) {
    std::vector<MorphBinding> bindings;
    if (tracks.empty() || targetShapes.empty()) return bindings;

    // Index target shapes by exact name, lowercase name, and normalized token.
    std::unordered_map<std::string, std::vector<size_t>> exactMap;
    std::unordered_map<std::string, std::vector<size_t>> lowerMap;
    std::unordered_map<std::string, std::vector<size_t>> normMap;
    for (size_t i = 0; i < targetShapes.size(); ++i) {
        const auto& s = targetShapes[i];
        if (s.smrIndex < 0 || s.shapeIndex < 0 || s.name.empty()) continue;
        exactMap[s.name].push_back(i);
        lowerMap[ToLowerAscii(s.name)].push_back(i);
        const std::string norm = NormalizeMorphToken(s.name);
        if (!norm.empty()) normMap[norm].push_back(i);
    }

    const auto aliases = StandardMorphAliasTable();
    std::unordered_map<std::string_view, const MorphAliasEntry*> aliasLookup;
    for (const auto& entry : aliases) aliasLookup.emplace(entry.mmdName, &entry);

    auto addSlot = [&](size_t trackIdx, size_t slotIdx, float scale) {
        const auto& s = targetShapes[slotIdx];
        // Avoid duplicate binding of same track -> same (smr, shape).
        for (const auto& b : bindings) {
            if (b.trackIndex == trackIdx && b.smrIndex == s.smrIndex && b.shapeIndex == s.shapeIndex) return;
        }
        bindings.push_back({trackIdx, s.smrIndex, s.shapeIndex, scale});
    };

    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        const std::string& name = tracks[ti].name;
        if (name.empty() || tracks[ti].keys.empty()) continue;

        // Tier 1: exact match (covers Japanese/PMX/VRM Unity BlendShapes directly).
        if (const auto it = exactMap.find(name); it != exactMap.end()) {
            for (size_t si : it->second) addSlot(ti, si, 1.f);
            continue;
        }
        // Tier 2: lowercase / normalized direct match (covers English-named VMD tracks).
        const std::string low = ToLowerAscii(name);
        if (const auto it = lowerMap.find(low); it != lowerMap.end()) {
            for (size_t si : it->second) addSlot(ti, si, 1.f);
            continue;
        }
        const std::string normName = NormalizeMorphToken(name);
        if (!normName.empty()) {
            if (const auto it = normMap.find(normName); it != normMap.end()) {
                for (size_t si : it->second) addSlot(ti, si, 1.f);
                continue;
            }
        }

        // Tier 3: canonical Japanese MMD morph alias table.
        const auto ait = aliasLookup.find(name);
        if (ait == aliasLookup.end()) continue;
        const MorphAliasEntry& entry = *ait->second;

        // Try paired left/right ARKit matches first (e.g., eyeBlinkLeft + eyeBlinkRight for まばたき).
        bool matchedAny = false;
        for (std::string_view tok : entry.targetTokens) {
            if (tok.empty()) break;
            const std::string normTok = NormalizeMorphToken(tok);
            const auto it = normMap.find(normTok);
            if (it == normMap.end()) continue;
            for (size_t si : it->second) addSlot(ti, si, entry.scale);
            matchedAny = true;
            // If this matched a Left ARKit token, keep scanning to pick up its Right partner too;
            // otherwise stop at the strongest canonical match.
            if (!ContainsAscii(normTok, "left")) break;
        }
        if (matchedAny) continue;

        // Tier 4: substring token fallback for prefixed/suffixed anime BlendShape names (e.g. "Head_Eye_Close_01").
        for (std::string_view tok : entry.targetTokens) {
            if (tok.size() < 4) continue;
            const std::string normTok = NormalizeMorphToken(tok);
            bool hit = false;
            for (size_t si = 0; si < targetShapes.size(); ++si) {
                const std::string targetNorm = NormalizeMorphToken(targetShapes[si].name);
                if (ContainsAscii(targetNorm, normTok)) {
                    addSlot(ti, si, entry.scale);
                    hit = true;
                }
            }
            if (hit) break;
        }
    }

    return bindings;
}

// Evaluates sampled Unity BlendShape weights (0..100) into `outWeights` (parallel to `targetShapes`).
// Writes `hasWeight[i] = true` for every BlendShape touched by the bindings.
inline void EvaluateMorphWeights(const std::vector<VmdMorphTrack>& tracks,
                                 const std::vector<MorphBinding>& bindings,
                                 double frame,
                                 size_t totalSlots,
                                 const std::vector<MorphTargetSlot>& targetShapes,
                                 std::vector<float>& outWeights0To100,
                                 std::vector<uint8_t>& outTouched) {
    outWeights0To100.assign(totalSlots, 0.f);
    outTouched.assign(totalSlots, 0U);
    if (bindings.empty() || totalSlots == 0) return;

    // Build quick reverse map (smrIndex, shapeIndex) -> slot index? Even simpler: bindings are small (< 128).
    for (const auto& b : bindings) {
        if (b.trackIndex >= tracks.size()) continue;
        const float raw = SampleMorphTrack(tracks[b.trackIndex], frame);
        const float contrib = std::clamp(raw * b.scale, 0.f, 1.f);
        for (size_t i = 0; i < totalSlots; ++i) {
            if (targetShapes[i].smrIndex == b.smrIndex && targetShapes[i].shapeIndex == b.shapeIndex) {
                outWeights0To100[i] = std::min(100.f, outWeights0To100[i] + contrib * 100.f);
                outTouched[i] = 1U;
            }
        }
    }
}

}  // namespace betterpose::mmd

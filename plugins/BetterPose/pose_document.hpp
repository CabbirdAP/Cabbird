#pragma once

#include "pose_math.hpp"
#include "skeleton_filter.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace betterpose {

struct PoseImportStats {
    int matchedByName{};
    int matchedByIndex{};
    int skipped{};
    bool hasRootOffset{};
};

inline nlohmann::json SerializePoseJson(const std::string& sourceName,
                                        const std::vector<std::string>& boneNames,
                                        const std::vector<EulerAngles>& boneOffsets,
                                        const Vec3& rootOffset) {
    nlohmann::json doc;
    doc["version"] = 1;
    doc["engine"] = "unity";
    doc["source"] = sourceName;
    doc["rootOffset"] = {rootOffset.x, rootOffset.y, rootOffset.z};
    nlohmann::json bones = nlohmann::json::array();
    const size_t n = std::min(boneNames.size(), boneOffsets.size());
    for (size_t i = 0; i < n; ++i) {
        if (boneOffsets[i].IsNearZero()) continue;
        bones.push_back({
            {"name", boneNames[i]},
            {"index", static_cast<int>(i)},
            {"euler", {boneOffsets[i].pitch, boneOffsets[i].yaw, boneOffsets[i].roll}}
        });
    }
    doc["bones"] = std::move(bones);
    return doc;
}

// Import pose JSON with name-first matching (exact, then case-insensitive) and index fallback.
inline bool DeserializePoseJson(const nlohmann::json& doc,
                                const std::vector<std::string>& boneNames,
                                std::vector<EulerAngles>& outOffsets,
                                Vec3& outRootOffset,
                                PoseImportStats& outStats) {
    outStats = {};
    if (!doc.is_object()) return false;
    const size_t n = boneNames.size();
    outOffsets.assign(n, EulerAngles{});
    outRootOffset = {};

    if (doc.contains("rootOffset") && doc["rootOffset"].is_array() && doc["rootOffset"].size() == 3) {
        const double rx = doc["rootOffset"][0].get<double>();
        const double ry = doc["rootOffset"][1].get<double>();
        const double rz = doc["rootOffset"][2].get<double>();
        if (std::isfinite(rx) && std::isfinite(ry) && std::isfinite(rz)) {
            outRootOffset = {rx, ry, rz};
            outStats.hasRootOffset = true;
        }
    }

    if (!doc.contains("bones") || !doc["bones"].is_array()) {
        return outStats.hasRootOffset;
    }

    std::unordered_map<std::string, size_t> exactMap;
    std::unordered_map<std::string, size_t> lowerMap;
    exactMap.reserve(n);
    lowerMap.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        exactMap.emplace(boneNames[i], i);
        lowerMap.emplace(ToLowerAscii(boneNames[i]), i);
    }

    struct PendingEntry {
        EulerAngles euler{};
        int fallbackIndex{-1};
    };
    std::vector<PendingEntry> unmatched;
    std::vector<bool> claimed(n, false);

    // Pass 1: Match by name first so name matches can never be overwritten by index fallbacks.
    for (const auto& entry : doc["bones"]) {
        if (!entry.is_object() || !entry.contains("euler") || !entry["euler"].is_array() ||
            entry["euler"].size() != 3) {
            ++outStats.skipped;
            continue;
        }
        const float pitch = entry["euler"][0].get<float>();
        const float yaw   = entry["euler"][1].get<float>();
        const float roll  = entry["euler"][2].get<float>();
        if (!std::isfinite(pitch) || !std::isfinite(yaw) || !std::isfinite(roll)) {
            ++outStats.skipped;
            continue;
        }
        const EulerAngles euler{WrapAngle180(pitch), WrapAngle180(yaw), WrapAngle180(roll)};

        size_t target = n;
        if (entry.contains("name") && entry["name"].is_string()) {
            const std::string name = entry["name"].get<std::string>();
            if (const auto it = exactMap.find(name); it != exactMap.end()) {
                target = it->second;
            } else if (const auto it2 = lowerMap.find(ToLowerAscii(name)); it2 != lowerMap.end()) {
                target = it2->second;
            }
        }
        if (target < n) {
            outOffsets[target] = euler;
            claimed[target] = true;
            ++outStats.matchedByName;
        } else {
            const int fallbackIdx = (entry.contains("index") && entry["index"].is_number_integer())
                ? entry["index"].get<int>() : -1;
            unmatched.push_back({euler, fallbackIdx});
        }
    }

    // Pass 2: Index fallback only for unclaimed target slots.
    for (const auto& item : unmatched) {
        if (item.fallbackIndex >= 0 && static_cast<size_t>(item.fallbackIndex) < n &&
            !claimed[static_cast<size_t>(item.fallbackIndex)]) {
            outOffsets[static_cast<size_t>(item.fallbackIndex)] = item.euler;
            claimed[static_cast<size_t>(item.fallbackIndex)] = true;
            ++outStats.matchedByIndex;
        } else {
            ++outStats.skipped;
        }
    }
    return true;
}

struct SkeletonExportBone {
    std::string name;
    int parent{-1};
    Vec3 localPos{};
    Quat localRot{Quat::Identity()};
    Vec3 worldPos{};
};

inline nlohmann::json SerializeSkeletonJson(const std::string& sourceName,
                                            const std::vector<SkeletonExportBone>& bones) {
    nlohmann::json doc;
    doc["version"] = 1;
    doc["engine"] = "unity";
    doc["source"] = sourceName;
    nlohmann::json arr = nlohmann::json::array();
    for (size_t i = 0; i < bones.size(); ++i) {
        const auto& b = bones[i];
        arr.push_back({
            {"index", static_cast<int>(i)},
            {"name", b.name},
            {"parent", b.parent},
            {"localPos", {b.localPos.x, b.localPos.y, b.localPos.z}},
            {"localRot", {b.localRot.w, b.localRot.x, b.localRot.y, b.localRot.z}},
            {"worldPos", {b.worldPos.x, b.worldPos.y, b.worldPos.z}}
        });
    }
    doc["bones"] = std::move(arr);
    return doc;
}

}  // namespace betterpose

#pragma once

#include "pose_math.hpp"

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace betterpose {

struct PoseSnapshot {
    std::vector<EulerAngles> boneOffsets;
    Vec3 rootOffset{};

    bool ApproxEquals(const PoseSnapshot& o, float degEps = 0.02f, double posEps = 1e-4) const noexcept {
        if (boneOffsets.size() != o.boneOffsets.size()) return false;
        if (std::fabs(rootOffset.x - o.rootOffset.x) > posEps ||
            std::fabs(rootOffset.y - o.rootOffset.y) > posEps ||
            std::fabs(rootOffset.z - o.rootOffset.z) > posEps) {
            return false;
        }
        for (size_t i = 0; i < boneOffsets.size(); ++i) {
            if (std::fabs(boneOffsets[i].pitch - o.boneOffsets[i].pitch) > degEps ||
                std::fabs(boneOffsets[i].yaw   - o.boneOffsets[i].yaw)   > degEps ||
                std::fabs(boneOffsets[i].roll  - o.boneOffsets[i].roll)  > degEps) {
                return false;
            }
        }
        return true;
    }
};

// Snapshot-based Undo/Redo history with settle-delay batching so dragging a slider
// or bone continuously commits a single clean history step once editing settles.
class PoseHistory {
public:
    static constexpr size_t kMaxEntries = 100;
    static constexpr uint64_t kSettleMs = 350;

    void Reset(const PoseSnapshot& initial) {
        entries_.clear();
        entries_.push_back(initial);
        cursor_ = 0;
        pendingDirty_ = false;
        lastEditMs_ = 0;
    }

    void Clear() {
        entries_.clear();
        cursor_ = 0;
        pendingDirty_ = false;
        lastEditMs_ = 0;
    }

    // Mark that the live pose was modified at `nowMs`.
    void MarkDirty(uint64_t nowMs) noexcept {
        pendingDirty_ = true;
        lastEditMs_ = nowMs;
    }

    // Commit immediately (e.g., on mouse button release, discrete button click, import, mirror).
    bool CommitNow(const PoseSnapshot& current) {
        pendingDirty_ = false;
        if (entries_.empty()) {
            entries_.push_back(current);
            cursor_ = 0;
            return true;
        }
        if (entries_[cursor_].ApproxEquals(current)) {
            return false;
        }
        // Discard any redo future past cursor.
        if (cursor_ + 1 < entries_.size()) {
            entries_.resize(cursor_ + 1);
        }
        entries_.push_back(current);
        if (entries_.size() > kMaxEntries) {
            entries_.erase(entries_.begin());
        }
        cursor_ = entries_.size() - 1;
        return true;
    }

    // Call periodically with current timestamp; commits once edits have settled for kSettleMs.
    bool TickSettle(uint64_t nowMs, bool draggingNow, const PoseSnapshot& current) {
        if (!pendingDirty_ || draggingNow) return false;
        if (nowMs >= lastEditMs_ && (nowMs - lastEditMs_) >= kSettleMs) {
            return CommitNow(current);
        }
        return false;
    }

    bool CanUndo() const noexcept {
        return !entries_.empty() && (cursor_ > 0 || pendingDirty_);
    }

    bool CanRedo() const noexcept {
        return !entries_.empty() && !pendingDirty_ && (cursor_ + 1 < entries_.size());
    }

    bool Undo(const PoseSnapshot& liveBeforeUndo, PoseSnapshot& outSnapshot) {
        if (entries_.empty()) return false;
        if (pendingDirty_) {
            // Save the uncommitted live edits onto the top of the stack so Redo can bring them back.
            CommitNow(liveBeforeUndo);
        }
        if (cursor_ == 0) return false;
        --cursor_;
        outSnapshot = entries_[cursor_];
        return true;
    }

    bool Redo(PoseSnapshot& outSnapshot) {
        if (!CanRedo()) return false;
        ++cursor_;
        outSnapshot = entries_[cursor_];
        return true;
    }

    size_t Size() const noexcept { return entries_.size(); }
    size_t Cursor() const noexcept { return cursor_; }

private:
    std::vector<PoseSnapshot> entries_;
    size_t cursor_{0};
    bool pendingDirty_{false};
    uint64_t lastEditMs_{0};
};

}  // namespace betterpose

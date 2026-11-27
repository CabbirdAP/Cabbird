#pragma once

#include "collection_runtime.hpp"
#include "cabbird/sdk/services/player_motion.h"
#include <mutex>

namespace collectibles {
// The host owns native hooks and state restoration. This action owns one
// generation-scoped intent until its interaction reaches a terminal state.
class CollectionHold final {
    const CabbirdUnityPlayerMotionServiceV1* service_{};
    mutable std::mutex mutex_;
    CabbirdGenerationHandleV1 handle_{};
    std::string failure_;
public:
    bool Bind(const CabbirdHostApiV1* host) {
        const auto service = Host(host).Query<CabbirdUnityPlayerMotionServiceV1>(CABBIRD_UNITY_PLAYER_MOTION_SERVICE_V1_ID);
        if (!service || service->struct_size < sizeof(*service.get()) || !service->acquire || !service->update || !service->release)
            return false;
        service_ = service.get();
        return true;
    }
    bool End() noexcept {
        std::scoped_lock lock(mutex_);
        if (!handle_.id) return true;
        const auto status = service_->release(service_->user, handle_);
        if (Succeeded(status) || status.code == CABBIRD_STATUS_V1_NOT_FOUND || status.code == CABBIRD_STATUS_V1_UNAVAILABLE) {
            handle_ = {}; return true;
        }
        return false;
    }
    void Cancel() noexcept { static_cast<void>(End()); }
    bool Release() noexcept { return End(); }
    bool Begin(const Runtime&, std::uint64_t entity_id) {
        if (!End()) return false;
        CabbirdUnityPlayerMotionRequestV1 request{sizeof(request)};
        request.mode = CABBIRD_UNITY_PLAYER_MOTION_V1_HOLD_POSITION;
        request.entity_id = entity_id;
        std::scoped_lock lock(mutex_);
        const auto status = service_->acquire(service_->user, &request, &handle_);
        if (!Succeeded(status)) {
            failure_ = "player-motion.acquire code=" + std::to_string(status.code);
            if (status.message.data && status.message.size) failure_ += " " + std::string(status.message.data, status.message.size);
            return false;
        }
        return true;
    }
    std::string_view LastFailure() const noexcept { return failure_; }
    std::string Diagnostics() const {
        std::scoped_lock lock(mutex_);
        return "motionHold=" + std::to_string(handle_.id != 0);
    }
};
}  // namespace collectibles

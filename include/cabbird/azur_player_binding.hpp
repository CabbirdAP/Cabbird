#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <string>
#include <string_view>
#include <memory>
#include <atomic>
#include <chrono>
#include "cabbird/sdk/services/player_motion.h"

namespace cabbird { class UnityBuildProfile; }

namespace cabbird::azur {

struct PlayerMotionState;
// One typed native-hook owner per adapter generation. Requests contain only
// host-owned values; releasing one never invokes Unity or plugin code.
class PlayerMotionCoordinator final {
public:
    bool Acquire(const UnityBuildProfile& profile, std::uint64_t generation,
        const CabbirdUnityPlayerMotionRequestV1& request, CabbirdGenerationHandleV1& handle,
        std::string& reason);
    bool Update(CabbirdGenerationHandleV1 handle,
        const CabbirdUnityPlayerMotionRequestV1& request, std::string& reason);
    bool Release(CabbirdGenerationHandleV1 handle) noexcept;
    void Invalidate() noexcept;
    bool Stop(std::chrono::milliseconds timeout) noexcept;
    bool BeginRecovery(const UnityBuildProfile& profile, std::uint64_t generation,
        std::uint64_t entity_id, CabbirdGenerationHandleV1& handle, std::string& reason);
private:
    std::atomic<std::shared_ptr<PlayerMotionState>> state_;
};

// Game policy belongs here, rather than in Unity's generic object/Transform services.
[[nodiscard]] constexpr bool IsPlayerDataClass(std::string_view name) noexcept {
    return name == "PlayerData" || name == "HeroData" || name == "PetData" || name == "MountData";
}

struct ControlledTypePair {
    std::uintptr_t entity{};
    std::uintptr_t data{};
};

struct ControlledPlayer {
    std::uintptr_t data{};
    std::uintptr_t klass{};
    std::uint64_t entity_id{};
    std::uintptr_t manager{};
    std::uint32_t object_index{};
    std::int32_t object_version{};
    std::uintptr_t entity{};
};

struct PlayerMotionBinding {
    std::uintptr_t alive_class{}, movement_class{}, get_movement{}, set_velocity{}, clear_forces{}, set_position{};
    std::uintptr_t get_transform{}, get_position{}, get_interpolation{}, set_interpolation{};
};

// Game domain only; matches the native Unstuck reset sequence without running
// its geometry search or disabling the game's later collision checks.
[[nodiscard]] bool TeleportControlledEntity(PlayerMotionBinding& binding,
    std::uintptr_t entity, std::uintptr_t observed_transform, const double position[3], std::string& reason);

// Metadata handles and offsets are immutable for one IL2CPP session. Cache these
// per adapter generation, while resolving the controlled object on every sample.
struct PlayerBinding {
    std::uintptr_t getter{};
    std::uintptr_t instance_field{};
    std::size_t handle_entity{};
    std::size_t entity_data{};
    std::size_t data_id{};
    std::size_t data_object_index{};
    std::size_t data_object_version{};
    std::array<ControlledTypePair, 3> controlled_types{};
};

// Game domain only. Missing world state or metadata is an unavailable binding.
[[nodiscard]] bool ResolveControlledPlayer(
    PlayerBinding& binding, ControlledPlayer& player, std::string& reason);

struct DataTransformLayout {
    std::size_t data_transform{};
    std::size_t relative_transform{};
};

// The BaseData -> RelativeTransform bridge is game knowledge, not a Unity object layout.
[[nodiscard]] bool ResolveDataTransformLayout(DataTransformLayout& layout, std::string& reason);

}  // namespace cabbird::azur

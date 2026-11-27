#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "cabbird/sdk/localization.hpp"
#include "cabbird/sdk/config_save_queue.hpp"
#include "cabbird/sdk/plugin_window.hpp"
#include "cabbird/plugin_support/entity_position.hpp"

namespace cabbird::plugins {
using Json = nlohmann::json;
using sdk::StringView;
using sdk::Succeeded;

inline CabbirdStatusV1 Status(std::uint32_t code) noexcept {
    return {code, 0, {}};
}

template <class Service>
const Service* Query(const CabbirdHostApiV1* host, std::string_view id) noexcept {
    const auto service = sdk::Host(host).Query<Service>(id);
    return service && service->struct_size >= sizeof(Service) ? service.get() : nullptr;
}

// Schema and reading use the host C ABI; durable writes use the shared Anomaly queue.
class ConfigStore final {
   public:
    CabbirdStatusV1 Bind(const CabbirdHostApiV1* host, std::string_view id, const Json& schema) {
        config_ = Query<CabbirdConfigServiceV1>(host, CABBIRD_CONFIG_SERVICE_V1_ID);
        const auto* scheduler = Query<CabbirdSchedulerServiceV1>(host, CABBIRD_SCHEDULER_SERVICE_V1_ID);
        if (!config_ || !config_->register_schema || !config_->read || !config_->write_atomic || !scheduler ||
            !scheduler->schedule)
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        id_ = id;
        const auto text = schema.dump();
        CabbirdGenerationHandleV1 handle{};
        const auto status = config_->register_schema(
            config_->user, StringView(id_), kSchemaVersion,
            {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, &handle);
        if (Succeeded(status))
            queue_.Bind(host, config_, id_, kSchemaVersion);
        return status;
    }
    std::optional<Json> Read() const {
        std::size_t size{};
        std::uint32_t version{};
        if (!Succeeded(config_->read(config_->user, StringView(id_), &version, {nullptr, 0}, &size)) ||
            version != kSchemaVersion || size == 0 || size > 64 * 1024)
            return {};
        std::vector<std::uint8_t> bytes(size);
        if (!Succeeded(config_->read(config_->user, StringView(id_), &version, {bytes.data(), bytes.size()},
                                     &size)) ||
            version != kSchemaVersion || size > bytes.size())
            return {};
        return Json::parse(bytes.begin(), bytes.begin() + size);
    }
    bool Queue(const Json& value) {
        return queue_.Queue(value.dump());
    }
    bool Stop() noexcept {
        return queue_.Flush();
    }

   private:
    static constexpr std::uint32_t kSchemaVersion = 2;
    const CabbirdConfigServiceV1* config_{};
    std::string id_;
    sdk::ConfigSaveQueue queue_;
};

inline Json ObjectSchema(Json properties) {
    Json required = Json::array();
    for (const auto& item : properties.items()) required.push_back(item.key());
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}
inline Json ToggleSchema() {
    return {{"type", "integer"}, {"minimum", 0}, {"maximum", 1}};
}
inline Json NumberSchema(double low, double high) {
    return {{"type", "number"}, {"minimum", low}, {"maximum", high}};
}
inline Json ArraySchema(std::size_t count, Json element) {
    return {{"type", "array"}, {"minItems", count}, {"maxItems", count}, {"items", std::move(element)}};
}
inline bool Toggle(int value) noexcept {
    return value == 0 || value == 1;
}
inline bool Range(double value, double low, double high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}
template <class Values>
bool InRange(const Values& values, double low, double high) noexcept {
    return std::all_of(values.begin(), values.end(), [=](double value) { return Range(value, low, high); });
}
inline bool ValidVector(const double value[3]) noexcept {
    return Range(value[0], -1e6, 1e6) && Range(value[1], -1e6, 1e6) && Range(value[2], -1e6, 1e6);
}
inline bool PlayerValid(const CabbirdUnityPlayerSnapshotV1& value) noexcept {
    return (value.flags & CABBIRD_UNITY_PLAYER_V1_VALID) != 0 && value.entity_id != 0 &&
           ValidVector(value.position);
}
inline CabbirdUnityPlayerSnapshotV1 ReadPlayer(const CabbirdUnityPlayerServiceV1* player) {
    CabbirdUnityPlayerSnapshotV1 value{sizeof(value)};
    if (!player || !player->snapshot || !Succeeded(player->snapshot(player->user, &value)) ||
        !PlayerValid(value)) {
        value.flags &= ~CABBIRD_UNITY_PLAYER_V1_VALID;
    }
    return value;
}
inline std::string Number(double value, int digits = 3) {
    char text[64]{};
    std::snprintf(text, sizeof(text), "%.*f", digits, value);
    return text;
}
inline std::string Format(const sdk::Localizer& text, std::string_view key, std::string_view english,
                          std::initializer_list<std::string_view> arguments) {
    return text.Format(key, english, std::span<const std::string_view>(arguments.begin(), arguments.size()));
}

// These are the UI entries shared by these four plugins. Optional presentation
// features are used only after checking their table entries.
inline bool UiAvailable(const CabbirdUiServiceV1* ui) noexcept {
    return ui && ui->begin_window && ui->end_window && ui->text && ui->button_enabled && ui->checkbox &&
           ui->input_double && ui->input_text && ui->slider_float && ui->separator &&
           ui->set_next_window_size && ui->get_window_size && ui->input_uint32;
}
using Window = sdk::PluginWindow;

struct EntityRow final {
    CabbirdUnityEntityV1 entity{sizeof(entity)};
    std::string label;
};
struct EntityFrame final {
    CabbirdUnityPlayerSnapshotV1 player{sizeof(player)};
    std::uint64_t revision{};
    std::uint32_t total{};
    std::vector<EntityRow> rows;
};
// Game-domain only. Borrowed labels are copied before the next service call.
inline void ReadEntities(const CabbirdUnityEntitiesServiceV1* service, EntityFrame& frame,
                         std::uint32_t limit = 512, bool rolesOnly = false) {
    if (!service || !service->entity_count || !service->entity_at)
        return;
    frame.revision = service->generation ? service->generation(service->user) : 0;
    frame.total = service->entity_count(service->user);
    frame.rows.reserve(std::min(frame.total, limit));
    for (std::uint32_t index = 0; index < frame.total && frame.rows.size() < limit; ++index) {
        EntityRow row;
        if (!Succeeded(service->entity_at(service->user, index, &row.entity)))
            continue;
        if (rolesOnly && row.entity.kind != CABBIRD_UNITY_ENTITY_V1_KIND_PLAYER)
            continue;
        if (row.entity.label && row.entity.label_size)
            row.label.assign(row.entity.label, row.entity.label_size);
        row.entity.label = nullptr;
        row.entity.label_size = 0;
        frame.rows.push_back(std::move(row));
    }
}
enum class MoveKind { Absolute, Relative, ToPlayer };
struct EntityMove final {
    MoveKind kind{};
    std::uint64_t entity{}, scene{}, player{};
    std::array<double, 3> position{};
    bool setRotation{};
    std::array<double, 3> rotation{};
    EntityIdentity identity;
};
enum class MoveState { Idle, Pending, Held, Reverted, Expired, Unavailable, Refused, Invalid };
struct EntityMoveResult final {
    MoveState state{MoveState::Idle};
    std::uint64_t entity{};
    std::array<double, 3> requested{}, measured{};
    double error{};
    bool measuredValid{};
};
// A request contains values and identities, never a retained Unity pointer.
// Resolve again for each operation, including delayed verification.
class EntityMover final {
   public:
    void Bind(const CabbirdHostApiV1* host) { access_.Bind(host); }
    bool Available() const noexcept { return access_.Available(); }
    bool Ready() const noexcept { return access_.Ready(); }
    EntityIdentities Capture(const EntityFrame& frame) {
        std::vector<std::uint64_t> addresses;
        addresses.reserve(frame.rows.size());
        for (const auto& row : frame.rows)
            if ((row.entity.flags & CABBIRD_UNITY_ENTITY_V1_VALID) &&
                !(row.entity.flags & CABBIRD_UNITY_ENTITY_V1_STALE) &&
                row.entity.entity_data == row.entity.entity_id && row.entity.entity_data)
                addresses.push_back(row.entity.entity_data);
        return access_.Capture(addresses);
    }
    bool Busy() const noexcept {
        return pending_.has_value();
    }
    EntityMoveResult Result() const noexcept {
        return result_;
    }
    void Apply(const EntityMove& request, const CabbirdUnityPlayerSnapshotV1& player,
               const CabbirdUnityEntitiesServiceV1* entities,
               const CabbirdUnityTransformServiceV1* transforms) {
        result_ = {};
        result_.entity = request.entity;
        if (!access_.Available() || !transforms || !transforms->resolve || !transforms->position || !entities ||
            !entities->lookup) {
            result_.state = MoveState::Unavailable;
            return;
        }
        if (!SameScene(request, player) || request.entity == player.entity_id || !request.identity.valid) {
            result_.state = MoveState::Expired;
            return;
        }
        CabbirdUnityTransformHandleV1 handle{sizeof(handle)};
        CabbirdUnityTransformPositionV1 before{sizeof(before)};
        if (!Resolve(request.entity, entities, transforms, handle) ||
            !Succeeded(transforms->position(transforms->user, &handle, &before)) ||
            !ValidVector(before.position)) {
            result_.state = MoveState::Expired;
            return;
        }
        auto target = request.position;
        if (request.kind != MoveKind::Absolute)
            for (std::size_t axis = 0; axis < 3; ++axis)
                target[axis] +=
                    request.kind == MoveKind::ToPlayer ? player.position[axis] : before.position[axis];
        if (!ValidVector(target.data()) || (request.setRotation && !ValidVector(request.rotation.data()))) {
            result_.state = MoveState::Invalid;
            return;
        }
        result_.requested = target;
        if (!access_.Write(handle.data, request.identity, target,
                           request.setRotation ? &request.rotation : nullptr)) {
            result_.state = MoveState::Refused;
            return;
        }
        CabbirdUnityTransformPositionV1 after{sizeof(after)};
        if (!Succeeded(transforms->position(transforms->user, &handle, &after)) ||
            !ValidVector(after.position)) {
            result_.state = MoveState::Refused;
            return;
        }
        Measure(after.position);
        if (result_.error > 0.75) {
            result_.state = MoveState::Refused;
            return;
        }
        result_.state = MoveState::Pending;
        pending_ = Pending{request, handle.transform, 4};
    }
    void Tick(const CabbirdUnityPlayerSnapshotV1& player, const CabbirdUnityEntitiesServiceV1* entities,
              const CabbirdUnityTransformServiceV1* transforms) {
        if (!pending_)
            return;
        if (!SameScene(pending_->request, player)) {
            result_.state = MoveState::Expired;
            pending_.reset();
            return;
        }
        if (--pending_->ticks != 0)
            return;
        CabbirdUnityTransformHandleV1 handle{sizeof(handle)};
        CabbirdUnityTransformPositionV1 position{sizeof(position)};
        if (!Resolve(pending_->request.entity, entities, transforms, handle) ||
            handle.transform != pending_->transform ||
            !access_.Check(handle.data, pending_->request.identity) ||
            !Succeeded(transforms->position(transforms->user, &handle, &position)) ||
            !ValidVector(position.position))
            result_.state = MoveState::Expired;
        else {
            Measure(position.position);
            result_.state = result_.error <= 0.75 ? MoveState::Held : MoveState::Reverted;
        }
        pending_.reset();
    }

   private:
    struct Pending {
        EntityMove request;
        std::uint64_t transform;
        int ticks;
    };
    static bool SameScene(const EntityMove& request, const CabbirdUnityPlayerSnapshotV1& player) noexcept {
        return PlayerValid(player) && request.scene == player.generation &&
               request.player == player.entity_id;
    }
    static bool Resolve(std::uint64_t id, const CabbirdUnityEntitiesServiceV1* entities,
                        const CabbirdUnityTransformServiceV1* transforms,
                        CabbirdUnityTransformHandleV1& handle) {
        CabbirdUnityEntitiesLookupV1 lookup{sizeof(lookup)};
        if (!Succeeded(entities->lookup(entities->user, id, &lookup)) || !lookup.data ||
            lookup.entity_id != id)
            return false;
        CabbirdUnityTransformResolveRequestV1 request{sizeof(request)};
        request.data = lookup.data;
        return Succeeded(transforms->resolve(transforms->user, &request, &handle)) && handle.transform != 0 &&
               handle.data == lookup.data;
    }
    void Measure(const double position[3]) noexcept {
        result_.measuredValid = true;
        std::copy(position, position + 3, result_.measured.begin());
        double square{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto d = position[axis] - result_.requested[axis];
            square += d * d;
        }
        result_.error = std::sqrt(square);
    }
    EntityMoveResult result_;
    std::optional<Pending> pending_;
    EntityPositionAccess access_;
};
}  // namespace cabbird::plugins

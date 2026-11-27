#include "cabbird/plugin_support/spatial.hpp"

namespace {
using namespace cabbird::plugins;
struct Settings {
    int relative{1}, setRotation{}, trackRoles{};
    std::array<double, 3> position{}, rotation{};
    std::uint32_t page{}, pageSize{8};
    float width{600}, height{670};
    bool operator==(const Settings&) const = default;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Settings, relative, setRotation, trackRoles, position, rotation, page,
                                   pageSize, width, height)
Json Schema() {
    return ObjectSchema({{"relative", ToggleSchema()},
                         {"setRotation", ToggleSchema()},
                         {"trackRoles", ToggleSchema()},
                         {"position", ArraySchema(3, NumberSchema(-1e6, 1e6))},
                         {"rotation", ArraySchema(3, NumberSchema(-1e6, 1e6))},
                         {"page", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1000000}}},
                         {"pageSize", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}},
                         {"width", NumberSchema(100, 8192)},
                         {"height", NumberSchema(100, 8192)}});
}
bool Valid(const Settings& s) {
    return Toggle(s.relative) && Toggle(s.setRotation) && Toggle(s.trackRoles) &&
           InRange(s.position, -1e6, 1e6) && InRange(s.rotation, -1e6, 1e6) && s.page <= 1000000 &&
           s.pageSize >= 1 && s.pageSize <= 64 && Range(s.width, 100, 8192) && Range(s.height, 100, 8192);
}
struct Intent {
    bool controlled{};
    EntityMove move;
};
struct Model {
    CabbirdUnityPlayerSnapshotV1 player{sizeof(player)};
    std::shared_ptr<const EntityFrame> roles;
    std::shared_ptr<const EntityIdentities> identities;
};
struct Context {
    explicit Context(const CabbirdHostApiV1* host) : text(host) {}
    cabbird::sdk::Localizer text;
    ConfigStore config;
    const CabbirdCoreServiceV1* core{};
    const CabbirdUiServiceV1* ui{};
    const CabbirdUnityPlayerServiceV1* player{};
    const CabbirdUnityPlayerTeleportServiceV1* teleport{};
    const CabbirdUnityEntitiesServiceV1* entities{};
    const CabbirdUnityTransformServiceV1* transforms{};
    Settings settings;
    std::atomic_bool tracking{}, rescan{}, busy{};
    std::atomic<std::shared_ptr<const Intent>> intent;
    std::atomic<std::shared_ptr<const Model>> model;
    std::shared_ptr<const EntityFrame> gameRoles;
    std::shared_ptr<const EntityIdentities> gameIdentities;
    EntityMover mover;
    std::optional<EntityMove> controlled;
    std::uint32_t baselineSequence{};
    std::uint32_t pollFailure{};
    double scanRemaining{}, scanStep{}, trackingElapsed{};
    std::uint64_t scene{};
    bool windowInitialized{}, worldActive{};
};
void LogFailure(Context& c, std::string_view message) {
    if (c.core && c.core->log)
        c.core->log(c.core->user, CABBIRD_CORE_LOG_LEVEL_V1_WARNING, StringView(message));
}
void ReportMoveFailure(Context& c, MoveState state) {
    std::string message;
    switch (state) {
        case MoveState::Reverted:
            message = c.text.Text("result.reverted", "The game moved the entity away from the target");
            break;
        case MoveState::Unavailable:
            message = c.text.Text("result.unavailable", "Teleport service unavailable");
            break;
        case MoveState::Refused:
            message = c.text.Text("result.refused", "The game or host refused the teleport");
            break;
        case MoveState::Invalid:
            message = c.text.Text("result.invalid", "Coordinates must be finite and within +/-1e6");
            break;
        default:
            return;
    }
    LogFailure(c, message);
}
void Queue(Context& c, const Model& model, std::uint64_t entity, bool controlled) {
    if (!controlled && (!model.identities || !model.identities->contains(entity)))
        return;
    bool expected = false;
    if (!c.busy.compare_exchange_strong(expected, true))
        return;
    auto intent = std::make_shared<Intent>();
    intent->controlled = controlled;
    intent->move.entity = entity;
    intent->move.scene = model.player.generation;
    intent->move.player = model.player.entity_id;
    intent->move.kind = c.settings.relative ? MoveKind::Relative : MoveKind::Absolute;
    intent->move.position = c.settings.position;
    intent->move.setRotation = c.settings.setRotation != 0;
    intent->move.rotation = c.settings.rotation;
    if (!controlled)
        intent->move.identity = model.identities->at(entity);
    c.intent.store(std::move(intent));
}
void PollControlled(Context& c, const CabbirdUnityPlayerSnapshotV1& player) {
    if (!c.controlled)
        return;
    if (!PlayerValid(player) || player.generation != c.controlled->scene ||
        player.entity_id != c.controlled->entity) {
        c.controlled.reset();
        c.busy.store(false);
        return;
    }
    CabbirdUnityPlayerWriteResultV1 result{sizeof(result)};
    const auto status = c.teleport->state(c.teleport->user, &result);
    if (!Succeeded(status)) {
        if (status.code != CABBIRD_STATUS_V1_UNAVAILABLE && status.code != CABBIRD_STATUS_V1_NOT_FOUND &&
            status.code != CABBIRD_STATUS_V1_CANCELLED && status.code != c.pollFailure)
            LogFailure(c, Format(c.text, "error.verification", "Player teleport verification failed (status {0})",
                                 {std::to_string(status.code)}));
        c.pollFailure = status.code;
        return;
    }
    c.pollFailure = 0;
    if (result.request_sequence == c.baselineSequence)
        return;
    if (result.entity_id != c.controlled->entity)
        return;
    if (result.result_code == CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_PENDING)
        return;
    if (result.result_code != CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_OK)
        LogFailure(c, Format(c.text, "error.result", "Player teleport failed (result {0}, request {1})",
                             {std::to_string(result.result_code), std::to_string(result.request_sequence)}));
    c.controlled.reset();
    c.busy.store(false);
}
void ApplyControlled(Context& c, const EntityMove& move, const CabbirdUnityPlayerSnapshotV1& player) {
    if (!c.teleport) {
        ReportMoveFailure(c, MoveState::Unavailable);
        c.busy.store(false);
        return;
    }
    if (!PlayerValid(player) || move.entity != player.entity_id || move.scene != player.generation) {
        c.busy.store(false);
        return;
    }
    auto target = move.position;
    if (move.kind == MoveKind::Relative)
        for (std::size_t axis = 0; axis < 3; ++axis) target[axis] += player.position[axis];
    if (!ValidVector(target.data()) || (move.setRotation && !ValidVector(move.rotation.data()))) {
        ReportMoveFailure(c, MoveState::Invalid);
        c.busy.store(false);
        return;
    }
    CabbirdUnityPlayerWriteResultV1 previous{sizeof(previous)};
    c.baselineSequence =
        Succeeded(c.teleport->state(c.teleport->user, &previous)) ? previous.request_sequence : 0;
    CabbirdUnityPlayerWriteRequestV1 request{sizeof(request)};
    request.entity_id = move.entity;
    request.flags = move.setRotation ? CABBIRD_UNITY_PLAYER_WRITE_V1_SET_ROTATION : 0;
    std::copy(target.begin(), target.end(), request.position);
    std::copy(move.rotation.begin(), move.rotation.end(), request.euler_degrees);
    const auto status = c.teleport->teleport(c.teleport->user, &request);
    if (!Succeeded(status)) {
        LogFailure(c, Format(c.text, "error.request", "Player teleport request failed (status {0})",
                             {std::to_string(status.code)}));
        c.busy.store(false);
        return;
    }
    c.controlled = move;
    c.pollFailure = 0;
}
void CABBIRD_CALL Update(void* raw, double delta) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        const auto player = ReadPlayer(c.player);
        const bool live = PlayerValid(player);
        if (c.scene != player.generation || !live) {
            c.gameRoles.reset();
            c.gameIdentities.reset();
            c.scanRemaining = 0;
            c.scene = player.generation;
        }
        if (live && (!c.worldActive || !c.gameRoles))
            c.rescan.store(true);
        c.worldActive = live;
        PollControlled(c, player);
        if (c.mover.Busy()) {
            c.mover.Tick(player, c.entities, c.transforms);
            if (!c.mover.Busy()) {
                ReportMoveFailure(c, c.mover.Result().state);
                c.busy.store(false);
            }
        }
        if (const auto intent = c.intent.exchange({})) {
            if (intent->controlled)
                ApplyControlled(c, intent->move, player);
            else {
                c.mover.Apply(intent->move, player, c.entities, c.transforms);
                if (!c.mover.Busy()) {
                    ReportMoveFailure(c, c.mover.Result().state);
                    c.busy.store(false);
                }
            }
        }
        if (PlayerValid(player) && c.entities) {
            if (c.rescan.exchange(false)) {
                c.scanRemaining = 0.8;
                c.scanStep = 0.4;
            }
            if (c.tracking.load()) {
                c.trackingElapsed += std::max(0.0, delta);
                if (c.trackingElapsed >= 5) {
                    c.trackingElapsed = 0;
                    c.scanRemaining = 0.8;
                    c.scanStep = 0.4;
                }
            }
            if (c.scanRemaining > 0) {
                if (c.entities->set_want_labels)
                    c.entities->set_want_labels(c.entities->user, 1);
                c.scanRemaining -= std::max(0.0, delta);
                c.scanStep += std::max(0.0, delta);
                if (c.scanStep >= 0.4) {
                    c.scanStep = 0;
                    auto frame = std::make_shared<EntityFrame>();
                    frame->player = player;
                    ReadEntities(c.entities, *frame, 128, true);
                    c.gameIdentities = std::make_shared<EntityIdentities>(c.mover.Capture(*frame));
                    c.gameRoles = std::move(frame);
                }
            }
        }
        auto model = std::make_shared<Model>();
        model->player = player;
        model->roles = c.gameRoles;
        model->identities = c.gameIdentities;
        c.model.store(std::move(model));
    } catch (...) {
    }
}
void DrawContent(Context& c) {
    auto& s = c.settings;
    const auto* ui = c.ui;
    Window window(ui, c.text.Label("window.title", "Player Teleport", "player-teleport"), s.width, s.height,
                  c.windowInitialized);
    if (!window)
        return;
    const auto model = c.model.load();
    const bool live = model && PlayerValid(model->player);
    const bool available = live && !c.busy.load();
    if (live)
        ui->text(ui->user,
                 StringView(Format(c.text, "snapshot.current", "Current world pivot: ({0}, {1}, {2})",
                                   {Number(model->player.position[0]), Number(model->player.position[1]),
                                    Number(model->player.position[2])})));
    ui->checkbox(ui->user,
                 StringView(c.text.Text("settings.relative", "Relative to the current world pivot")),
                 &s.relative);
    ui->input_double(ui->user, StringView(c.text.Text("settings.x", "X")), &s.position[0], 0, 0);
    ui->input_double(ui->user, StringView(c.text.Text("settings.y", "Y (up)")), &s.position[1], 0, 0);
    ui->input_double(ui->user, StringView(c.text.Text("settings.z", "Z")), &s.position[2], 0, 0);
    if (ui->button_enabled(ui->user,
                           StringView(c.text.Text("action.use_current", "Use current world position")), 0, 0,
                           live)) {
        s.relative = 0;
        std::copy(model->player.position, model->player.position + 3, s.position.begin());
    }
    ui->checkbox(ui->user, StringView(c.text.Text("settings.set_rotation", "Also set world rotation")),
                 &s.setRotation);
    if (s.setRotation) {
        ui->input_double(ui->user, StringView(c.text.Text("settings.rx", "Euler X (degrees)")),
                         &s.rotation[0], 0, 0);
        ui->input_double(ui->user, StringView(c.text.Text("settings.ry", "Euler Y (degrees)")),
                         &s.rotation[1], 0, 0);
        ui->input_double(ui->user, StringView(c.text.Text("settings.rz", "Euler Z (degrees)")),
                         &s.rotation[2], 0, 0);
    }
    if (ui->button_enabled(ui->user, StringView(c.text.Text("action.teleport", "Teleport controlled player")),
                           0, 0, available && c.teleport))
        Queue(c, *model, model->player.entity_id, true);
    ui->separator(ui->user);
    ui->text(ui->user, StringView(c.text.Text("roles.title", "Other player and role entities")));
    ui->checkbox(ui->user, StringView(c.text.Text("settings.track_roles", "Track roles every 5 seconds")),
                 &s.trackRoles);
    if (ui->button_enabled(ui->user, StringView(c.text.Text("roles.rescan", "Refresh role list")), 0, 0,
                           live && c.entities))
        c.rescan.store(true);
    ui->input_uint32(ui->user, StringView(c.text.Text("settings.page_size", "Rows per page")), &s.pageSize, 1,
                     4);
    s.pageSize = std::clamp(s.pageSize, 1u, 64u);
    const auto roles = model ? model->roles : nullptr;
    if (roles) {
        const auto count = roles->rows.size();
        const auto pages = std::max<std::size_t>(1, (count + s.pageSize - 1) / s.pageSize);
        const auto page = std::min<std::size_t>(s.page, pages - 1);
        ui->text(ui->user, StringView(Format(
                               c.text, "roles.page", "Page {0}/{1}; {2} role entities",
                               {std::to_string(page + 1), std::to_string(pages), std::to_string(count)})));
        for (std::size_t index = page * s.pageSize; index < std::min(count, (page + 1) * s.pageSize);
             ++index) {
            const auto& row = roles->rows[index];
            if (live && row.entity.entity_id == model->player.entity_id)
                continue;
            const auto id = std::to_string(row.entity.entity_id);
            const auto label = row.label.empty() ? c.text.Text("roles.unnamed", "Unnamed role") : row.label;
            ui->text(ui->user, StringView(label));
            const auto button = c.text.Label("roles.move", "Move this role", "role-" + id);
            const bool valid = (row.entity.flags & CABBIRD_UNITY_ENTITY_V1_VALID) &&
                               !(row.entity.flags & CABBIRD_UNITY_ENTITY_V1_STALE) && row.entity.entity_data;
            if (ui->button_enabled(ui->user, StringView(button), 0, 0,
                                   available && c.transforms && c.mover.Available() && valid &&
                                       model->identities && model->identities->contains(row.entity.entity_id)))
                Queue(c, *model, row.entity.entity_id, false);
        }
        if (ui->button_enabled(ui->user, StringView(c.text.Text("table.previous", "Previous page")), 0, 0,
                               page > 0))
            s.page = static_cast<std::uint32_t>(page - 1);
        if (ui->button_enabled(ui->user, StringView(c.text.Text("table.next", "Next page")), 0, 0,
                               page + 1 < pages))
            s.page = static_cast<std::uint32_t>(page + 1);
    }
}
void CABBIRD_CALL Draw(void* raw, const CabbirdUiServiceV1*) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        const auto before = c.settings;
        DrawContent(c);
        if (c.settings != before) {
            c.tracking.store(c.settings.trackRoles != 0);
            if (!c.config.Queue(Json(c.settings)))
                LogFailure(c, c.text.Text("error.save", "Could not save player teleport settings"));
        }
    } catch (...) {
    }
}
CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** output) noexcept {
    if (!host || !output)
        return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    *output = nullptr;
    try {
        auto c = std::make_unique<Context>(host);
        c->core = Query<CabbirdCoreServiceV1>(host, CABBIRD_CORE_SERVICE_V1_ID);
        c->ui = Query<CabbirdUiServiceV1>(host, CABBIRD_UI_SERVICE_V1_ID);
        if (!UiAvailable(c->ui))
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        const auto status = c->config.Bind(host, "player-teleport-settings", Schema());
        if (!Succeeded(status))
            return status;
        try {
            if (const auto json = c->config.Read()) {
                const auto loaded = json->get<Settings>();
                if (Valid(loaded))
                    c->settings = loaded;
            }
        } catch (...) {
        }
        c->player = Query<CabbirdUnityPlayerServiceV1>(host, CABBIRD_UNITY_PLAYER_SERVICE_V1_ID);
        if (c->player && !c->player->snapshot)
            c->player = nullptr;
        c->teleport =
            Query<CabbirdUnityPlayerTeleportServiceV1>(host, CABBIRD_UNITY_PLAYER_TELEPORT_SERVICE_V1_ID);
        if (c->teleport && (!c->teleport->teleport || !c->teleport->state))
            c->teleport = nullptr;
        c->entities = Query<CabbirdUnityEntitiesServiceV1>(host, CABBIRD_UNITY_ENTITIES_SERVICE_V1_ID);
        if (c->entities && (!c->entities->lookup || !c->entities->entity_count || !c->entities->entity_at))
            c->entities = nullptr;
        c->transforms = Query<CabbirdUnityTransformServiceV1>(host, CABBIRD_UNITY_TRANSFORM_SERVICE_V1_ID);
        if (c->transforms && (!c->transforms->resolve || !c->transforms->position))
            c->transforms = nullptr;
        c->mover.Bind(host);
        if (!c->player || !c->teleport || !c->entities || !c->transforms || !c->mover.Available())
            LogFailure(*c, c->text.Text("error.services", "Player teleport services unavailable"));
        c->tracking.store(c->settings.trackRoles != 0);
        *output = c.release();
        return cabbird::sdk::Ok();
    } catch (...) {
        return Status(CABBIRD_STATUS_V1_FAILED);
    }
}
CabbirdStatusV1 CABBIRD_CALL Start(void*) noexcept {
    return cabbird::sdk::Ok();
}
CabbirdStatusV1 CABBIRD_CALL Stop(void* raw, std::uint32_t) noexcept {
    auto& c = *static_cast<Context*>(raw);
    if (c.config.Stop())
        return cabbird::sdk::Ok();
    try { LogFailure(c, c.text.Text("error.save", "Could not save player teleport settings")); } catch (...) {}
    return Status(CABBIRD_STATUS_V1_FAILED);
}
void CABBIRD_CALL Unload(void* raw) noexcept {
    try {
        delete static_cast<Context*>(raw);
    } catch (...) {
    }
}
}  // namespace
CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(CabbirdPluginDescriptorV1* d) {
    if (!d || d->struct_size < sizeof(*d))
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *d = {sizeof(*d),
          CABBIRD_PLUGIN_API_V1_MAJOR,
          CABBIRD_PLUGIN_API_V1_MINOR,
          cabbird::sdk::StringView("cabbird.player-teleport"),
          cabbird::sdk::StringView("Player Teleport"),
          cabbird::sdk::StringView("Cabbird"),
          cabbird::sdk::StringView("1.0.0"),
          Load,
          Start,
          Stop,
          Unload,
          Update,
          Draw};
    return cabbird::sdk::Ok();
}

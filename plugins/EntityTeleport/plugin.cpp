#include <cctype>

#include "cabbird/plugin_support/spatial.hpp"

namespace {
using namespace cabbird::plugins;
struct Settings {
    std::array<int, 5> categories{1, 1, 1, 1, 1};
    int searchEnabled{}, relative{}, track{};
    std::string search;
    std::array<double, 3> position{};
    float landingHeight{}, maximumDistance{}, width{720}, height{700};
    std::uint32_t page{}, pageSize{12};
    bool operator==(const Settings&) const = default;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Settings, categories, searchEnabled, relative, track, search, position,
                                   landingHeight, maximumDistance, width, height, page, pageSize)
Json Schema() {
    return ObjectSchema({{"categories", ArraySchema(5, ToggleSchema())},
                         {"searchEnabled", ToggleSchema()},
                         {"relative", ToggleSchema()},
                         {"track", ToggleSchema()},
                         {"search", {{"type", "string"}, {"maxLength", 255}}},
                         {"position", ArraySchema(3, NumberSchema(-1e6, 1e6))},
                         {"landingHeight", NumberSchema(-2, 5)},
                         {"maximumDistance", NumberSchema(0, 10000)},
                         {"width", NumberSchema(100, 8192)},
                         {"height", NumberSchema(100, 8192)},
                         {"page", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1000000}}},
                         {"pageSize", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}}});
}
bool Valid(const Settings& s) {
    return std::all_of(s.categories.begin(), s.categories.end(), Toggle) && Toggle(s.searchEnabled) &&
           Toggle(s.relative) && Toggle(s.track) && s.search.size() < 256 &&
           s.search.find('\0') == std::string::npos && InRange(s.position, -1e6, 1e6) &&
           Range(s.landingHeight, -2, 5) && Range(s.maximumDistance, 0, 10000) && Range(s.width, 100, 8192) &&
           Range(s.height, 100, 8192) && s.page <= 1000000 && s.pageSize >= 1 && s.pageSize <= 64;
}
struct Model {
    CabbirdUnityPlayerSnapshotV1 player{sizeof(player)};
    std::shared_ptr<const EntityFrame> entities;
    std::shared_ptr<const EntityIdentities> identities;
};
struct Context {
    explicit Context(const CabbirdHostApiV1* host) : text(host) {}
    cabbird::sdk::Localizer text;
    ConfigStore config;
    const CabbirdCoreServiceV1* core{};
    const CabbirdUiServiceV1* ui{};
    const CabbirdUnityPlayerServiceV1* player{};
    const CabbirdUnityEntitiesServiceV1* entities{};
    const CabbirdUnityTransformServiceV1* transforms{};
    Settings settings;
    std::atomic_bool tracking{}, refresh{}, busy{};
    std::atomic<std::shared_ptr<const EntityMove>> intent;
    std::atomic<std::shared_ptr<const Model>> model;
    std::shared_ptr<const EntityFrame> gameEntities;
    std::shared_ptr<const EntityIdentities> gameIdentities;
    EntityMover mover;
    std::uint64_t scene{}, selected{}, selectedScene{};
    EntityIdentity selectedIdentity;
    double scanRemaining{}, scanStep{}, trackingElapsed{};
    bool windowInitialized{}, worldActive{}, bindingFailureLogged{}, saveFailureLogged{};
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
            message = c.text.Text("result.unavailable", "Entity teleport service unavailable");
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
double Distance(const double a[3], const double b[3]) {
    double sum{};
    for (int axis = 0; axis < 3; ++axis) {
        const auto d = a[axis] - b[axis];
        sum += d * d;
    }
    return std::sqrt(sum);
}
bool Matches(const Settings& s, const EntityRow& row, const CabbirdUnityPlayerSnapshotV1& player) {
    const auto& e = row.entity;
    if (!e.entity_id || e.entity_id == player.entity_id || (e.flags & CABBIRD_UNITY_ENTITY_V1_LOCAL_PLAYER) ||
        !(e.flags & CABBIRD_UNITY_ENTITY_V1_VALID) || (e.flags & CABBIRD_UNITY_ENTITY_V1_STALE) ||
        !s.categories[e.kind < 5 ? e.kind : 0])
        return false;
    if (s.maximumDistance > 0 &&
        (!PlayerValid(player) || Distance(e.bounds_center, player.position) > s.maximumDistance))
        return false;
    if (s.searchEnabled && !s.search.empty()) {
        const auto found = std::search(row.label.begin(), row.label.end(), s.search.begin(), s.search.end(),
                                       [](char left, char right) {
                                           return std::tolower(static_cast<unsigned char>(left)) ==
                                                  std::tolower(static_cast<unsigned char>(right));
                                       });
        if (found == row.label.end())
            return false;
    }
    return true;
}
void Queue(Context& c, const Model& model, std::uint64_t entity, MoveKind kind) {
    if (!model.identities || !model.identities->contains(entity))
        return;
    bool expected = false;
    if (!c.busy.compare_exchange_strong(expected, true))
        return;
    auto move = std::make_shared<EntityMove>();
    move->kind = kind;
    move->entity = entity;
    move->scene = model.player.generation;
    move->player = model.player.entity_id;
    move->identity = model.identities->at(entity);
    move->position = kind == MoveKind::ToPlayer ? std::array<double, 3>{0, c.settings.landingHeight, 0}
                                                : c.settings.position;
    c.intent.store(std::move(move));
}
void CABBIRD_CALL Update(void* raw, double delta) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        const auto player = ReadPlayer(c.player);
        const bool live = PlayerValid(player);
        if (c.scene != player.generation || !live) {
            c.gameEntities.reset();
            c.gameIdentities.reset();
            c.scanRemaining = 0;
            c.scene = player.generation;
        }
        if (live && (!c.worldActive || !c.gameEntities))
            c.refresh.store(true);
        c.worldActive = live;
        if (c.mover.Busy()) {
            c.mover.Tick(player, c.entities, c.transforms);
            if (!c.mover.Busy()) {
                ReportMoveFailure(c, c.mover.Result().state);
                c.busy.store(false);
            }
        }
        if (const auto intent = c.intent.exchange({})) {
            c.mover.Apply(*intent, player, c.entities, c.transforms);
            if (!c.mover.Busy()) {
                ReportMoveFailure(c, c.mover.Result().state);
                c.busy.store(false);
            }
        }
        if (PlayerValid(player) && c.entities) {
            if (c.refresh.exchange(false)) {
                c.scanRemaining = 0.8;
                c.scanStep = 0.4;
            }
            if (c.tracking.load()) {
                c.trackingElapsed += std::max(0.0, delta);
                if (c.trackingElapsed >= 1) {
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
                    ReadEntities(c.entities, *frame);
                    c.gameIdentities = std::make_shared<EntityIdentities>(c.mover.Capture(*frame));
                    if (!c.mover.Ready() && !c.bindingFailureLogged) {
                        c.bindingFailureLogged = true;
                        LogFailure(c, c.text.Text("error.binding", "Could not validate the game's entity teleport methods"));
                    }
                    c.gameEntities = std::move(frame);
                }
            }
        }
        auto model = std::make_shared<Model>();
        model->player = player;
        model->entities = c.gameEntities;
        model->identities = c.gameIdentities;
        c.model.store(std::move(model));
    } catch (...) {
    }
}
void DrawContent(Context& c) {
    const auto* ui = c.ui;
    auto& s = c.settings;
    Window window(ui, c.text.Label("window.title", "Entity Teleport", "entity-teleport"), s.width, s.height,
                  c.windowInitialized);
    if (!window)
        return;
    const auto model = c.model.load();
    const bool live = model && PlayerValid(model->player);
    const bool available = live && !c.busy.load() && c.transforms && c.entities && c.mover.Available();
    ui->checkbox(ui->user, StringView(c.text.Text("settings.track", "Refresh entity list every second")),
                 &s.track);
    if (ui->button_enabled(ui->user, StringView(c.text.Text("action.refresh", "Refresh entity list")), 0, 0,
                           live && c.entities))
        c.refresh.store(true);
    auto check = [&](const char* key, const char* english, int& value) {
        ui->checkbox(ui->user, StringView(c.text.Text(key, english)), &value);
    };
    check("kind.unknown", "Unclassified", s.categories[0]);
    check("kind.player", "Players and roles", s.categories[1]);
    check("kind.monster", "Monsters", s.categories[2]);
    check("kind.npc", "NPCs", s.categories[3]);
    check("kind.world", "World objects", s.categories[4]);
    check("settings.search_enabled", "Filter by name", s.searchEnabled);
    if (s.searchEnabled) {
        std::array<char, 256> input{};
        std::memcpy(input.data(), s.search.data(), s.search.size());
        if (ui->input_text(ui->user, StringView(c.text.Text("settings.search", "Name contains")),
                           input.data(), input.size(), 0))
            s.search = input.data();
    }
    ui->slider_float(
        ui->user,
        StringView(c.text.Text("settings.maximum_distance", "Maximum player distance (0 = unlimited)")),
        &s.maximumDistance, 0, 10000);
    ui->slider_float(ui->user,
                     StringView(c.text.Text("settings.landing_height", "Landing height above player (m)")),
                     &s.landingHeight, -2, 5);
    ui->input_uint32(ui->user, StringView(c.text.Text("settings.page_size", "Rows per page")), &s.pageSize, 1,
                     4);
    s.pageSize = std::clamp(s.pageSize, 1u, 64u);
    const auto frame = model ? model->entities : nullptr;
    if (frame) {
        if (c.selectedScene != model->player.generation) {
            c.selected = 0;
            c.selectedScene = model->player.generation;
        }
        std::vector<const EntityRow*> rows;
        rows.reserve(frame->rows.size());
        for (const auto& row : frame->rows)
            if (Matches(s, row, model->player))
                rows.push_back(&row);
        const auto movable = [&](const EntityRow& row) {
            return available && model->identities && model->identities->contains(row.entity.entity_id);
        };
        const auto pages = std::max<std::size_t>(1, (rows.size() + s.pageSize - 1) / s.pageSize);
        const auto page = std::min<std::size_t>(s.page, pages - 1);
        ui->text(
            ui->user,
            StringView(Format(c.text, "table.summary", "{0} matching entities; page {1}/{2}",
                              {std::to_string(rows.size()), std::to_string(page + 1), std::to_string(pages)})));
        bool selectedPresent{};
        for (const auto* row : rows)
            if (row->entity.entity_id == c.selected && model->identities &&
                model->identities->contains(c.selected) &&
                model->identities->at(c.selected) == c.selectedIdentity)
                selectedPresent = true;
        if (!selectedPresent)
            c.selected = 0;
        for (std::size_t index = page * s.pageSize; index < std::min(rows.size(), (page + 1) * s.pageSize);
             ++index) {
            const auto& row = *rows[index];
            const auto id = std::to_string(row.entity.entity_id);
            const auto name = row.label.empty() ? c.text.Text("table.unnamed", "Unnamed entity") : row.label;
            ui->text(ui->user, StringView(name));
            ui->text(ui->user, StringView(Format(c.text, "table.bounds", "Bounds center: ({0}, {1}, {2})",
                                                 {Number(row.entity.bounds_center[0], 1),
                                                  Number(row.entity.bounds_center[1], 1),
                                                  Number(row.entity.bounds_center[2], 1)})));
            if (ui->button_enabled(ui->user,
                                   StringView(c.text.Label("action.select", "Select entity", "select-" + id)),
                                   0, 0, model->identities && model->identities->contains(row.entity.entity_id))) {
                c.selected = row.entity.entity_id;
                c.selectedIdentity = model->identities->at(c.selected);
            }
            if (ui->button_enabled(ui->user,
                                   StringView(c.text.Label("action.bring", "Bring to player", "bring-" + id)),
                                   0, 0, movable(row)))
                Queue(c, *model, row.entity.entity_id, MoveKind::ToPlayer);
        }
        if (ui->button_enabled(ui->user, StringView(c.text.Text("table.previous", "Previous page")), 0, 0,
                               page > 0))
            s.page = static_cast<std::uint32_t>(page - 1);
        if (ui->button_enabled(ui->user, StringView(c.text.Text("table.next", "Next page")), 0, 0,
                               page + 1 < pages))
            s.page = static_cast<std::uint32_t>(page + 1);
        if (ui->button_enabled(
                ui->user,
                StringView(c.text.Text("action.nearest", "Bring nearest matching entity to player")), 0, 0,
                available && !rows.empty())) {
            const EntityRow* nearest{};
            double best{};
            for (const auto* row : rows)
                if (movable(*row)) {
                    const auto distance = Distance(row->entity.bounds_center, model->player.position);
                    if (!nearest || distance < best) {
                        nearest = row;
                        best = distance;
                    }
                }
            if (nearest)
                Queue(c, *model, nearest->entity.entity_id, MoveKind::ToPlayer);
        }
    }
    ui->separator(ui->user);
    std::string selectedName = c.text.Text("status.none", "None");
    if (frame)
        for (const auto& row : frame->rows)
            if (row.entity.entity_id == c.selected) {
                selectedName = row.label.empty() ? c.text.Text("table.unnamed", "Unnamed entity") : row.label;
                break;
            }
    ui->text(ui->user, StringView(Format(
                           c.text, "status.selected", "Selected entity: {0}",
                           {selectedName})));
    ui->input_double(ui->user, StringView(c.text.Text("settings.x", "X")), &s.position[0], 0, 0);
    ui->input_double(ui->user, StringView(c.text.Text("settings.y", "Y (up)")), &s.position[1], 0, 0);
    ui->input_double(ui->user, StringView(c.text.Text("settings.z", "Z")), &s.position[2], 0, 0);
    ui->checkbox(ui->user,
                 StringView(c.text.Text("settings.relative", "Relative to the entity's current world pivot")),
                 &s.relative);
    if (ui->button_enabled(ui->user, StringView(c.text.Text("action.move", "Move selected entity")), 0, 0,
                           available && c.selected && model->identities && model->identities->contains(c.selected)))
        Queue(c, *model, c.selected, s.relative ? MoveKind::Relative : MoveKind::Absolute);
}
void CABBIRD_CALL Draw(void* raw, const CabbirdUiServiceV1*) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        const auto before = c.settings;
        DrawContent(c);
        if (c.settings != before) {
            c.tracking.store(c.settings.track != 0);
            if (!c.config.Queue(Json(c.settings)) && !c.saveFailureLogged) {
                c.saveFailureLogged = true;
                LogFailure(c, c.text.Text("error.save", "Could not save entity teleport settings"));
            }
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
        const auto status = c->config.Bind(host, "entity-teleport-settings", Schema());
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
        c->entities = Query<CabbirdUnityEntitiesServiceV1>(host, CABBIRD_UNITY_ENTITIES_SERVICE_V1_ID);
        if (c->entities && (!c->entities->lookup || !c->entities->entity_count || !c->entities->entity_at))
            c->entities = nullptr;
        c->transforms = Query<CabbirdUnityTransformServiceV1>(host, CABBIRD_UNITY_TRANSFORM_SERVICE_V1_ID);
        if (c->transforms && (!c->transforms->resolve || !c->transforms->position))
            c->transforms = nullptr;
        c->mover.Bind(host);
        if (!c->player || !c->entities || !c->transforms || !c->mover.Available())
            LogFailure(*c, c->text.Text("error.services", "Entity teleport services unavailable"));
        c->tracking.store(c->settings.track != 0);
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
    try { LogFailure(c, c.text.Text("error.save", "Could not save entity teleport settings")); } catch (...) {}
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
          cabbird::sdk::StringView("cabbird.entity-teleport"),
          cabbird::sdk::StringView("Entity Teleport"),
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

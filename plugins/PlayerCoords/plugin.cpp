// Unity access stays in Game; Render consumes copied values.
#include "cabbird/plugin_support/spatial.hpp"
#include "cabbird/sdk/services/ui_resources.h"

namespace {
using namespace cabbird::plugins;
struct Settings {
    int watch{1};
    bool operator==(const Settings&) const = default;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Settings, watch)
Json Schema() {
    return ObjectSchema({{"watch", ToggleSchema()}});
}
bool Valid(const Settings& s) {
    return Toggle(s.watch);
}
struct Reading {
    CabbirdUnityPlayerSnapshotV1 player{sizeof(player)};
};
struct Context {
    explicit Context(const CabbirdHostApiV1* host) : text(host) {}
    cabbird::sdk::Localizer text;
    ConfigStore config;
    const CabbirdUiServiceV1* ui{};
    const CabbirdCoreServiceV1* core{};
    const CabbirdUnityPlayerServiceV1* player{};
    const CabbirdWindowServiceV1* windows{};
    CabbirdGenerationHandleV1 window{};
    Settings settings;
    std::atomic<int> watch{1};
    std::atomic_bool refresh{true};
    std::atomic<std::shared_ptr<const Reading>> snapshot;
    Reading game;
    std::atomic_bool update_failure_logged{}, draw_failure_logged{}, save_failure_logged{};
};
void LogOnce(Context& c, std::atomic_bool& reported, std::string_view key, std::string_view english) noexcept {
    if (!c.core || !c.core->log || reported.exchange(true))
        return;
    try {
        const auto message = c.text.Text(key, english);
        c.core->log(c.core->user, CABBIRD_CORE_LOG_LEVEL_V1_ERROR, StringView(message));
    } catch (...) {
    }
}
void CABBIRD_CALL Update(void* raw, double) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        const auto value = ReadPlayer(c.player);
        const bool changed =
            value.entity_id != c.game.player.entity_id || value.generation != c.game.player.generation;
        // Even a paused reading loses its validity when its owning player/scene disappears.
        if (c.watch.load() || c.refresh.exchange(false) || changed || !PlayerValid(value)) {
            c.game.player = value;
            c.snapshot.store(std::make_shared<Reading>(c.game));
        }
    } catch (...) {
        auto& c = *static_cast<Context*>(raw);
        LogOnce(c, c.update_failure_logged, "log.update_failed", "Player coordinate snapshot update failed.");
    }
}
void DrawContent(Context& c) {
    const auto* ui = c.ui;
    auto& s = c.settings;
    ui->checkbox(ui->user, StringView(c.text.Text("settings.watch", "Refresh continuously")), &s.watch);
    if (!s.watch &&
        ui->button_enabled(ui->user, StringView(c.text.Text("action.refresh", "Read current position")), 0, 0,
                           1))
        c.refresh.store(true);
    const auto reading = c.snapshot.load();
    if (reading && PlayerValid(reading->player)) {
        const auto& p = reading->player;
        ui->text(ui->user,
                 StringView(Format(c.text, "snapshot.position", "World pivot: x = {0}   y = {1}   z = {2}",
                                   {Number(p.position[0]), Number(p.position[1]), Number(p.position[2])})));
        if (p.flags & CABBIRD_UNITY_PLAYER_V1_HAS_BOUNDS)
            ui->text(ui->user,
                     StringView(Format(c.text, "snapshot.bounds", "Box center: x = {0}   y = {1}   z = {2}",
                                       {Number(p.bounds_center[0]), Number(p.bounds_center[1]),
                                        Number(p.bounds_center[2])})));
    } else {
        ui->text(ui->user, StringView(c.text.Text("snapshot.empty", "World pivot: --")));
    }
}
void CABBIRD_CALL Draw(void* raw, const CabbirdUiServiceV1*) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        // A closed registered window returns OK without pushing a UI scope.
        // Query it first; a collapsed or newly X-closed window still pairs End.
        CabbirdWindowStateV1 state{sizeof(state)};
        if (!c.window.id || !Succeeded(c.windows->state(c.windows->user, c.window, &state)) || !state.open)
            return;
        std::int32_t visible{};
        if (!Succeeded(c.windows->begin(c.windows->user, c.window, 0, &visible)))
            return;
        struct EndWindow {
            Context& context;
            ~EndWindow() { static_cast<void>(context.windows->end(context.windows->user, context.window)); }
        } end{c};
        if (!visible)
            return;
        const auto before = c.settings;
        DrawContent(c);
        if (c.settings != before) {
            c.watch.store(c.settings.watch);
            if (!c.config.Queue(Json(c.settings)))
                LogOnce(c, c.save_failure_logged, "log.save_failed", "Player coordinate settings could not be queued for saving.");
        }
    } catch (...) {
        auto& c = *static_cast<Context*>(raw);
        LogOnce(c, c.draw_failure_logged, "log.draw_failed", "Player coordinate window update failed.");
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
        c->windows = Query<CabbirdWindowServiceV1>(host, CABBIRD_WINDOW_SERVICE_V1_ID);
        if (!c->ui || !c->ui->text || !c->ui->checkbox || !c->ui->button_enabled ||
            !c->windows || !c->windows->register_window || !c->windows->release_window ||
            !c->windows->state || !c->windows->begin || !c->windows->end)
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        const auto status = c->config.Bind(host, "player-coordinates-settings", Schema());
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
        c->watch.store(c->settings.watch);
        *output = c.release();
        return cabbird::sdk::Ok();
    } catch (...) {
        return Status(CABBIRD_STATUS_V1_FAILED);
    }
}
CabbirdStatusV1 CABBIRD_CALL Start(void* raw) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        const auto title = c.text.Text("window.title", "Player Coordinates");
        CabbirdWindowSpecV1 spec{};
        spec.struct_size = sizeof(spec);
        spec.id = StringView("player-coordinates");
        spec.title = StringView(title);
        spec.initial_width = 560;
        spec.initial_height = 410;
        spec.minimum_width = 100;
        spec.minimum_height = 100;
        spec.maximum_width = 8192;
        spec.maximum_height = 8192;
        spec.default_open = 1;
        return c.windows->register_window(c.windows->user, &spec, &c.window);
    } catch (...) {
        return Status(CABBIRD_STATUS_V1_FAILED);
    }
}
CabbirdStatusV1 CABBIRD_CALL Stop(void* raw, std::uint32_t) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        if (!c.config.Stop())
            return Status(CABBIRD_STATUS_V1_FAILED);
        if (!c.window.id)
            return cabbird::sdk::Ok();
        const auto released = c.windows->release_window(c.windows->user, c.window);
        if (!Succeeded(released) && released.code != CABBIRD_STATUS_V1_NOT_FOUND)
            return released;
        c.window = {};
        return cabbird::sdk::Ok();
    } catch (...) {
        return Status(CABBIRD_STATUS_V1_FAILED);
    }
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
          cabbird::sdk::StringView("cabbird.player-coords"),
          cabbird::sdk::StringView("Player Coordinates"),
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

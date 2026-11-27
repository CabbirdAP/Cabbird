#include <limits>

#include "cabbird/plugin_support/spatial.hpp"

namespace {
using namespace cabbird::plugins;
constexpr std::size_t kEntityLimit = 512;
struct Settings {
    int enabled{1}, boxes{1}, labels{1}, hideUnnamed{1}, showLocal{};
    std::array<int, 5> categories{1, 1, 1, 1, 1};
    float minimumDistance{}, maximumDistance{}, thickness{1.5f}, labelScale{1.6f};
    std::array<std::array<float, 4>, 5> colors{{{0.3f, 0.85f, 1, 1},
                                                {0.25f, 1, 0.35f, 1},
                                                {1, 0.25f, 0.25f, 1},
                                                {1, 0.85f, 0.25f, 1},
                                                {0.65f, 0.65f, 0.7f, 1}}};
    std::array<float, 4> labelColor{1, 1, 1, 1};
    float width{520}, height{640};
    bool operator==(const Settings&) const = default;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Settings, enabled, boxes, labels, hideUnnamed, showLocal, categories,
                                   minimumDistance, maximumDistance, thickness, labelScale, colors,
                                   labelColor, width, height)
Json Schema() {
    return ObjectSchema({{"enabled", ToggleSchema()},
                         {"boxes", ToggleSchema()},
                         {"labels", ToggleSchema()},
                         {"hideUnnamed", ToggleSchema()},
                         {"showLocal", ToggleSchema()},
                         {"categories", ArraySchema(5, ToggleSchema())},
                         {"minimumDistance", NumberSchema(0, 10000)},
                         {"maximumDistance", NumberSchema(0, 10000)},
                         {"thickness", NumberSchema(1, 6)},
                         {"labelScale", NumberSchema(0.6, 2)},
                         {"colors", ArraySchema(5, ArraySchema(4, NumberSchema(0, 1)))},
                         {"labelColor", ArraySchema(4, NumberSchema(0, 1))},
                         {"width", NumberSchema(100, 8192)},
                         {"height", NumberSchema(100, 8192)}});
}
bool Valid(const Settings& s) {
    if (!Toggle(s.enabled) || !Toggle(s.boxes) || !Toggle(s.labels) || !Toggle(s.hideUnnamed) ||
        !Toggle(s.showLocal) || !std::all_of(s.categories.begin(), s.categories.end(), Toggle) ||
        !Range(s.minimumDistance, 0, 10000) || !Range(s.maximumDistance, 0, 10000) ||
        !Range(s.thickness, 1, 6) || !Range(s.labelScale, 0.6, 2) || !Range(s.width, 100, 8192) ||
        !Range(s.height, 100, 8192) || !InRange(s.labelColor, 0, 1))
        return false;
    return std::all_of(s.colors.begin(), s.colors.end(),
                       [](const auto& color) { return InRange(color, 0, 1); });
}
struct Row {
    CabbirdUnityEntityV1 entity{sizeof(entity)};
    std::array<char, 256> label{};
};
struct Snapshot {
    std::vector<Row> rows;
    bool cameraValid{};
};
struct LabelMeasure {
    std::array<char, 256> text{};
    std::uint32_t length{};
    float scale{}, width{}, height{};
};
struct Context {
    explicit Context(const CabbirdHostApiV1* host) : text(host) {}
    cabbird::sdk::Localizer text;
    ConfigStore config;
    const CabbirdCoreServiceV1* core{};
    const CabbirdUiServiceV1* ui{};
    const CabbirdUnityEntitiesServiceV1* entities{};
    const CabbirdUnityOverlayServiceV1* overlay{};
    CabbirdGenerationHandleV1 subscription{};
    Settings settings;
    std::atomic<std::shared_ptr<const Settings>> display;
    std::atomic<std::shared_ptr<const Snapshot>> snapshot;
    std::array<LabelMeasure, kEntityLimit> measurements;
    bool windowInitialized{};
    std::uint32_t readFailure{};
};
void LogFailure(Context& c, std::string_view message) {
    if (c.core && c.core->log)
        c.core->log(c.core->user, CABBIRD_CORE_LOG_LEVEL_V1_ERROR, StringView(message));
}
std::uint32_t Color(const std::array<float, 4>& color) {
    return CABBIRD_RGBA_V1(static_cast<unsigned>(color[0] * 255), static_cast<unsigned>(color[1] * 255),
                           static_cast<unsigned>(color[2] * 255), static_cast<unsigned>(color[3] * 255));
}
void CABBIRD_CALL DrawOverlay(void* raw, const CabbirdUnityOverlayFrameV1* frame) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        if (!frame || frame->struct_size < sizeof(*frame) || !frame->draw_line || !frame->draw_text ||
            !frame->measure_text)
            return;
        const auto settings = c.display.load();
        const auto snapshot = c.snapshot.load();
        if (!settings || !snapshot || !settings->enabled)
            return;
        const auto& s = *settings;
        for (std::size_t index = 0; index < snapshot->rows.size(); ++index) {
            const auto& row = snapshot->rows[index];
            const auto& e = row.entity;
            if (!(e.flags & CABBIRD_UNITY_ENTITY_V1_VALID) || (e.flags & CABBIRD_UNITY_ENTITY_V1_STALE) ||
                (!s.showLocal && (e.flags & CABBIRD_UNITY_ENTITY_V1_LOCAL_PLAYER)) ||
                (s.hideUnnamed && e.label_size == 0))
                continue;
            const auto kind = e.kind < 5 ? e.kind : 0;
            if (!s.categories[kind])
                continue;
            if (s.minimumDistance > 0 || s.maximumDistance > 0) {
                if (!snapshot->cameraValid || !std::isfinite(e.distance_meters) ||
                    e.distance_meters < s.minimumDistance ||
                    (s.maximumDistance > 0 && e.distance_meters > s.maximumDistance))
                    continue;
            }
            if (!(e.flags & CABBIRD_UNITY_ENTITY_V1_SCREEN_POINTS) || e.screen_point_mask != 0xff)
                continue;
            float left = std::numeric_limits<float>::max(), right = -left, top = left, bottom = -left;
            bool finite = true;
            for (const auto& p : e.screen_points) {
                finite = finite && std::isfinite(p[0]) && std::isfinite(p[1]);
                left = std::min(left, static_cast<float>(p[0]));
                right = std::max(right, static_cast<float>(p[0]));
                top = std::min(top, static_cast<float>(p[1]));
                bottom = std::max(bottom, static_cast<float>(p[1]));
            }
            if (!finite || right - left < 1 || bottom - top < 1 || right < 0 || bottom < 0 ||
                left > frame->viewport_width || top > frame->viewport_height)
                continue;
            if (s.boxes) {
                const auto color = Color(s.colors[kind]);
                frame->draw_line(frame->user, left, top, right, top, color, s.thickness);
                frame->draw_line(frame->user, right, top, right, bottom, color, s.thickness);
                frame->draw_line(frame->user, right, bottom, left, bottom, color, s.thickness);
                frame->draw_line(frame->user, left, bottom, left, top, color, s.thickness);
            }
            if (s.labels && e.label_size) {
                auto& measure = c.measurements[index];
                if (measure.length != e.label_size || measure.scale != s.labelScale ||
                    std::memcmp(measure.text.data(), row.label.data(), e.label_size) != 0) {
                    if (!frame->measure_text(frame->user, {row.label.data(), e.label_size}, s.labelScale,
                                             &measure.width, &measure.height))
                        continue;
                    measure.text = row.label;
                    measure.length = e.label_size;
                    measure.scale = s.labelScale;
                }
                frame->draw_text(frame->user, {row.label.data(), e.label_size},
                                 (left + right - measure.width) * 0.5f, top - measure.height - 2,
                                 Color(s.labelColor), s.labelScale);
            }
        }
    } catch (...) {
    }
}
void CABBIRD_CALL Update(void* raw, double) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        const auto settings = c.display.load();
        if (!settings || !c.entities)
            return;
        std::uint32_t failure{};
        const int want = settings->enabled && (settings->labels || settings->hideUnnamed);
        if (c.entities->set_want_labels) {
            const auto status = c.entities->set_want_labels(c.entities->user, want);
            if (!Succeeded(status) && status.code != CABBIRD_STATUS_V1_UNAVAILABLE &&
                status.code != CABBIRD_STATUS_V1_NOT_FOUND)
                failure = status.code;
        }
        if (!settings->enabled) {
            c.snapshot.store({});
            return;
        }
        auto next = std::make_shared<Snapshot>();
        double camera[3]{};
        next->cameraValid =
            c.entities->camera_position && Succeeded(c.entities->camera_position(c.entities->user, camera));
        const auto count = std::min<std::size_t>(c.entities->entity_count(c.entities->user), kEntityLimit);
        next->rows.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            Row row;
            const auto status = c.entities->entity_at(c.entities->user, static_cast<std::uint32_t>(index), &row.entity);
            if (!Succeeded(status)) {
                if (status.code != CABBIRD_STATUS_V1_UNAVAILABLE && status.code != CABBIRD_STATUS_V1_NOT_FOUND)
                    failure = status.code;
                continue;
            }
            const auto size = std::min<std::size_t>(row.entity.label_size, row.label.size() - 1);
            std::size_t copied = row.entity.label ? size : 0;
            // A bounded label must end at a UTF-8 character boundary.
            if (copied < row.entity.label_size && row.entity.label)
                while (copied > 0 && (static_cast<unsigned char>(row.entity.label[copied]) & 0xc0) == 0x80)
                    --copied;
            if (copied)
                std::memcpy(row.label.data(), row.entity.label, copied);
            row.entity.label = nullptr;
            row.entity.label_size = static_cast<std::uint32_t>(copied);
            next->rows.push_back(row);
        }
        if (failure && failure != c.readFailure)
            LogFailure(c, Format(c.text, "error.snapshot", "Entity snapshot failed (status {0})", {std::to_string(failure)}));
        c.readFailure = failure;
        c.snapshot.store(std::move(next));
    } catch (...) {
    }
}
void DrawContent(Context& c) {
    auto& s = c.settings;
    const auto* ui = c.ui;
    Window window(ui, c.text.Label("window.title", "Entity Overlay", "entity-overlay"), s.width, s.height,
                  c.windowInitialized);
    if (!window)
        return;
    auto check = [&](const char* key, const char* english, int& value) {
        ui->checkbox(ui->user, StringView(c.text.Text(key, english)), &value);
    };
    check("settings.enabled", "Enable overlay", s.enabled);
    check("settings.boxes", "Draw boxes", s.boxes);
    check("settings.labels", "Draw names", s.labels);
    check("settings.hide_unnamed", "Hide unnamed entities", s.hideUnnamed);
    check("settings.show_local", "Show controlled player", s.showLocal);
    ui->separator(ui->user);
    check("kind.unknown", "Unclassified", s.categories[0]);
    check("kind.player", "Players and roles", s.categories[1]);
    check("kind.monster", "Monsters", s.categories[2]);
    check("kind.npc", "NPCs", s.categories[3]);
    check("kind.world", "World objects", s.categories[4]);
    ui->slider_float(ui->user,
                     StringView(c.text.Text("settings.minimum_distance", "Minimum camera distance (m)")),
                     &s.minimumDistance, 0, 10000);
    ui->slider_float(
        ui->user,
        StringView(c.text.Text("settings.maximum_distance", "Maximum camera distance (0 = unlimited)")),
        &s.maximumDistance, 0, 10000);
    ui->slider_float(ui->user, StringView(c.text.Text("settings.thickness", "Box thickness")), &s.thickness,
                     1, 6);
    ui->slider_float(ui->user, StringView(c.text.Text("settings.label_scale", "Name scale")), &s.labelScale,
                     0.6f, 2);
    if (ui->color_edit4) {
        ui->color_edit4(ui->user, StringView(c.text.Text("color.unknown", "Unclassified color")),
                        s.colors[0].data());
        ui->color_edit4(ui->user, StringView(c.text.Text("color.player", "Player and role color")),
                        s.colors[1].data());
        ui->color_edit4(ui->user, StringView(c.text.Text("color.monster", "Monster color")),
                        s.colors[2].data());
        ui->color_edit4(ui->user, StringView(c.text.Text("color.npc", "NPC color")), s.colors[3].data());
        ui->color_edit4(ui->user, StringView(c.text.Text("color.world", "World object color")),
                        s.colors[4].data());
        ui->color_edit4(ui->user, StringView(c.text.Text("color.label", "Name color")), s.labelColor.data());
    }
}
void CABBIRD_CALL Draw(void* raw, const CabbirdUiServiceV1*) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        const auto before = c.settings;
        DrawContent(c);
        if (c.settings != before) {
            c.display.store(std::make_shared<Settings>(c.settings));
            if (!c.config.Queue(Json(c.settings)))
                LogFailure(c, c.text.Text("error.save", "Could not save overlay settings"));
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
        const auto bound = c->config.Bind(host, "entity-overlay-settings", Schema());
        if (!Succeeded(bound))
            return bound;
        try {
            if (const auto json = c->config.Read()) {
                const auto loaded = json->get<Settings>();
                if (Valid(loaded))
                    c->settings = loaded;
            }
        } catch (...) {
        }
        c->entities = Query<CabbirdUnityEntitiesServiceV1>(host, CABBIRD_UNITY_ENTITIES_SERVICE_V1_ID);
        if (c->entities && (!c->entities->entity_count || !c->entities->entity_at))
            c->entities = nullptr;
        c->overlay = Query<CabbirdUnityOverlayServiceV1>(host, CABBIRD_UNITY_OVERLAY_SERVICE_V1_ID);
        if (c->overlay && (!c->overlay->subscribe || !c->overlay->unsubscribe))
            c->overlay = nullptr;
        c->display.store(std::make_shared<Settings>(c->settings));
        *output = c.release();
        return cabbird::sdk::Ok();
    } catch (...) {
        return Status(CABBIRD_STATUS_V1_FAILED);
    }
}
CabbirdStatusV1 CABBIRD_CALL Start(void* raw) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        if (!c.overlay || !c.entities) {
            LogFailure(c, c.text.Text("error.services", "Entity overlay services unavailable"));
            return cabbird::sdk::Ok();
        }
        const auto status = c.overlay->subscribe(c.overlay->user, DrawOverlay, &c, &c.subscription);
        if (!Succeeded(status))
            LogFailure(c, c.text.Text("error.subscribe", "Could not register the entity overlay"));
        return status;
    } catch (...) {
        return Status(CABBIRD_STATUS_V1_FAILED);
    }
}
CabbirdStatusV1 CABBIRD_CALL Stop(void* raw, std::uint32_t) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        auto result = cabbird::sdk::Ok();
        if (c.subscription.id) {
            result = c.overlay->unsubscribe(c.overlay->user, c.subscription);
            // The host revokes subscriptions before on_stop, so an already
            // removed scope-owned registration is the normal teardown path.
            if (result.code == CABBIRD_STATUS_V1_NOT_FOUND)
                result = cabbird::sdk::Ok();
            if (Succeeded(result))
                c.subscription = {};
        }
        const bool saved = c.config.Stop();
        if (!saved)
            LogFailure(c, c.text.Text("error.save", "Could not save overlay settings"));
        return !Succeeded(result) ? result : saved ? cabbird::sdk::Ok() : Status(CABBIRD_STATUS_V1_FAILED);
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
CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(CabbirdPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor))
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {sizeof(*descriptor),
                   CABBIRD_PLUGIN_API_V1_MAJOR,
                   CABBIRD_PLUGIN_API_V1_MINOR,
                   cabbird::sdk::StringView("cabbird.entity-overlay"),
                   cabbird::sdk::StringView("Entity Overlay"),
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

// Reads the host's cached Unity snapshots through optional public services.
#include "cabbird/sdk/localization.hpp"

#include <cstdio>
#include <initializer_list>
#include <string>

namespace {
using cabbird::sdk::StringView;
const CabbirdUiServiceV1* g_ui{};
const CabbirdUnityEntitiesServiceV1* g_entities{};
const CabbirdUnityPlayerServiceV1* g_player{};
cabbird::sdk::Localizer g_text;
int g_open = 1;

std::string Fixed(double value, int precision) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}
void Text(const char* key, const char* fallback,
          std::initializer_list<std::string_view> arguments = {}) {
    const auto value = g_text.Format(key, fallback, {arguments.begin(), arguments.size()});
    g_ui->text(g_ui->user, StringView(value));
}
std::string KindName(std::uint32_t kind) {
    switch (kind) {
    case CABBIRD_UNITY_ENTITY_V1_KIND_PLAYER: return g_text.Text("kind.player", "player");
    case CABBIRD_UNITY_ENTITY_V1_KIND_MONSTER: return g_text.Text("kind.monster", "monster");
    case CABBIRD_UNITY_ENTITY_V1_KIND_NPC: return g_text.Text("kind.npc", "NPC");
    case CABBIRD_UNITY_ENTITY_V1_KIND_WORLD: return g_text.Text("kind.world", "world");
    default: return g_text.Text("kind.other", "unclassified");
    }
}

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *context = nullptr;
    const cabbird::sdk::Host services(host);
    g_ui = services.Query<CabbirdUiServiceV1>(CABBIRD_UI_SERVICE_V1_ID).get();
    if (g_ui == nullptr || g_ui->struct_size < offsetof(CabbirdUiServiceV1, separator) +
        sizeof(g_ui->separator) || g_ui->text == nullptr || g_ui->separator == nullptr)
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    g_text = cabbird::sdk::Localizer(host);
    g_entities = services.Query<CabbirdUnityEntitiesServiceV1>(CABBIRD_UNITY_ENTITIES_SERVICE_V1_ID).get();
    if (g_entities != nullptr && (g_entities->struct_size <
        offsetof(CabbirdUnityEntitiesServiceV1, entity_at) + sizeof(g_entities->entity_at) ||
        g_entities->entity_count == nullptr || g_entities->entity_at == nullptr ||
        g_entities->generation == nullptr)) g_entities = nullptr;
    g_player = services.Query<CabbirdUnityPlayerServiceV1>(CABBIRD_UNITY_PLAYER_SERVICE_V1_ID).get();
    if (g_player != nullptr && (g_player->struct_size <
        offsetof(CabbirdUnityPlayerServiceV1, snapshot) + sizeof(g_player->snapshot) ||
        g_player->snapshot == nullptr)) g_player = nullptr;
    g_open = 1;
    *context = &g_open;
    return cabbird::sdk::Ok();
}
CabbirdStatusV1 CABBIRD_CALL Start(void*) { return cabbird::sdk::Ok(); }
CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) { return cabbird::sdk::Ok(); }
void CABBIRD_CALL Unload(void*) {
    g_ui = nullptr;
    g_entities = nullptr;
    g_player = nullptr;
    g_text = {};
}

void DrawPlayer() {
    if (g_player == nullptr) return;
    CabbirdUnityPlayerSnapshotV1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    if (g_player->snapshot(g_player->user, &snapshot).code != CABBIRD_STATUS_V1_OK) {
        Text("player.missing", "Player: no live snapshot");
        return;
    }
    Text("player.position", "Player: {0} ({1}, {2}, {3})",
        {snapshot.data_class, Fixed(snapshot.position[0], 1), Fixed(snapshot.position[1], 1),
         Fixed(snapshot.position[2], 1)});
}
void DrawEntities() {
    if (g_entities == nullptr) return;
    const auto total = g_entities->entity_count(g_entities->user);
    Text("entities.summary", "Entities: {0}, generation {1}",
        {std::to_string(total), std::to_string(g_entities->generation(g_entities->user))});
    g_ui->separator(g_ui->user);
    constexpr std::uint32_t maximum_rows = 40;
    const auto rows = total < maximum_rows ? total : maximum_rows;
    for (std::uint32_t index = 0; index < rows; ++index) {
        CabbirdUnityEntityV1 entity{};
        entity.struct_size = sizeof(entity);
        if (g_entities->entity_at(g_entities->user, index, &entity).code != CABBIRD_STATUS_V1_OK) continue;
        const auto bytes = entity.label == nullptr ? 0 : (entity.label_size < 48 ? entity.label_size : 48);
        const std::string_view label(entity.label == nullptr ? "" : entity.label, bytes);
        Text("entities.row", "[{0}] {1} {2} m ({3}, {4}, {5}) {6}",
            {std::to_string(index), KindName(entity.kind), Fixed(entity.distance_meters, 1),
             Fixed(entity.bounds_center[0], 0), Fixed(entity.bounds_center[1], 0),
             Fixed(entity.bounds_center[2], 0), label});
    }
    if (total > rows) Text("entities.more", "... and {0} more", {std::to_string(total - rows)});
}
void CABBIRD_CALL Draw(void*, const CabbirdUiServiceV1*) {
    const auto title = g_text.Label("window.title", "Unity Entity Inspector", "unity-entity-inspector");
    cabbird::sdk::UiWindow window(g_ui, title, &g_open);
    if (!window) return;
    Text(g_entities != nullptr ? "services.entities.ready" : "services.entities.missing",
        g_entities != nullptr ? "Entities: available" : "Entities: unavailable");
    Text(g_player != nullptr ? "services.player.ready" : "services.player.missing",
        g_player != nullptr ? "Player: available" : "Player: unavailable");
    g_ui->separator(g_ui->user);
    DrawPlayer();
    g_ui->separator(g_ui->user);
    DrawEntities();
}
}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {
        sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        StringView("cabbird.example.unity-entity-inspector"), StringView("Unity Entity Inspector"),
        StringView("Cabbird"), StringView("1.0.0"), Load, Start, Stop, Unload, nullptr, Draw};
    return cabbird::sdk::Ok();
}

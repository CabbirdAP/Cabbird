#include "cabbird/sdk/localization.hpp"
#include "cabbird/sdk/services/platform.h"
#include "cabbird/sdk/services/ui.h"
#include "cabbird/sdk/services/ui_resources.h"
#include "uid_runtime.hpp"
#include "cabbird/sdk/profile.hpp"
#include "cabbird/sdk/services/game_lifecycle.h"
#include "cabbird/sdk/config_save_queue.hpp"
#include <cmath>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {
using cabbird::sdk::StringView;
using cabbird::sdk::Ok;
cabbird::sdk::Localizer g_text;
constexpr const char* kSchemaId = "fake-uid-settings-v1";
constexpr const char* kSchema = R"({
  "type":"object",
  "required":["enabled","hidePrefix","displayUid","windowWidth","windowHeight"],
  "properties":{
    "enabled":{"type":"boolean"},
    "hidePrefix":{"type":"boolean"},
    "displayUid":{"type":"string","minLength":1,"maxLength":1024},
    "windowWidth":{"type":"number","minimum":240,"maximum":8192},
    "windowHeight":{"type":"number","minimum":120,"maximum":8192}
  },
  "additionalProperties":false
})";

const CabbirdUiServiceV1* g_ui{};
const CabbirdCoreServiceV1* g_core{};
const CabbirdConfigServiceV1* g_config{};
const CabbirdIl2CppServiceV1* g_il2cpp{};
const CabbirdSchedulerServiceV1* g_scheduler{};
std::mutex g_mutex;
fake_uid::Settings g_active{};
bool g_running{};
const CabbirdWindowServiceV1* g_windows{};
CabbirdGenerationHandleV1 g_window{};
int g_hide_prefix{0};
char g_display[1025]{"12345678"};
bool g_save_failed{};
float g_width{440}, g_height{260};
cabbird::sdk::ConfigSaveQueue g_save;
const CabbirdGameLifecycleServiceV1* g_game_lifecycle{};
CabbirdGenerationHandleV1 g_cleanup{};
void LoadConfig() {
    if (!g_config) return;
    CabbirdGenerationHandleV1 handle{};
    const auto registration = g_config->register_schema(g_config->user, StringView(kSchemaId), 1,
        {reinterpret_cast<const std::uint8_t*>(kSchema), std::strlen(kSchema)}, &handle);
    if (registration.code != CABBIRD_STATUS_V1_OK) { fake_uid::LogFailure("Settings schema registration failed."); g_config = nullptr; return; }
    std::size_t size{};
    std::uint32_t version{};
    const auto probe = g_config->read(g_config->user, StringView(kSchemaId), &version, {nullptr, 0}, &size);
    if (probe.code == CABBIRD_STATUS_V1_NOT_FOUND) return;
    if (probe.code != CABBIRD_STATUS_V1_OK || size == 0 || size > 8192 || version != 1) {
        fake_uid::LogFailure("Stored settings could not be read; defaults retained."); return;
    }
    std::vector<std::uint8_t> bytes(size);
    if (g_config->read(g_config->user, StringView(kSchemaId), &version, {bytes.data(), bytes.size()}, &size).code !=
        CABBIRD_STATUS_V1_OK || size > bytes.size()) { fake_uid::LogFailure("Stored settings could not be read; defaults retained."); return; }
    try {
        const auto json = nlohmann::json::parse(bytes.begin(), bytes.begin() + size);
        const auto display = json.at("displayUid").get<std::string>();
        // Legacy originalUid is informational only, never an editable identity.
        if (display.empty() || display.size() >= sizeof(g_display) ||
            display.find('\0') != std::string::npos) { fake_uid::LogFailure("Stored settings contain an invalid display value; defaults retained."); return; }
        fake_uid::Settings loaded{};
        std::snprintf(loaded.display_uid, sizeof(loaded.display_uid), "%s", display.c_str());
        loaded.hide_prefix = json.value("hidePrefix", false);
        loaded.enabled = json.value("enabled", false);
        // Parse all fields before committing; a malformed field must not leave
        // a partially loaded editor. Load validates the final runtime request.
        g_active = loaded;
        std::memcpy(g_display, loaded.display_uid, sizeof(g_display));
        g_hide_prefix = loaded.hide_prefix;
        g_width = json.value("windowWidth", 440.0f); g_height = json.value("windowHeight", 260.0f);
    } catch (...) {
        fake_uid::LogFailure("Stored settings were invalid; defaults retained.");
    }
}
void SaveConfig() {
    const auto document = nlohmann::json{{"enabled", g_active.enabled},
        {"hidePrefix", g_active.hide_prefix}, {"displayUid", g_active.display_uid},
        {"windowWidth", g_width}, {"windowHeight", g_height}}.dump();
    const bool failed = !g_save.Queue(document);
    if (failed && !g_save_failed) fake_uid::LogFailure("Settings could not be queued for saving.");
    g_save_failed = failed;
}
void CopyEditor(fake_uid::Settings& request) {
    request.hide_prefix = g_hide_prefix;
    std::memcpy(request.display_uid, g_display, sizeof(g_display));
}
void CABBIRD_CALL Draw(void*, const CabbirdUiServiceV1*) {
    std::scoped_lock lock(g_mutex);
    if (!g_ui) return;
    std::int32_t visible{};
    if (!g_window.id || !cabbird::sdk::Succeeded(g_windows->begin(g_windows->user, g_window, 0, &visible))) return;
    struct EndWindow { ~EndWindow() { static_cast<void>(g_windows->end(g_windows->user, g_window)); } } end;
    if (!visible) return;
    auto* ui = g_ui;
    bool changed{};
    if (ui->get_window_size) {
        float width{}, height{}; ui->get_window_size(ui->user, &width, &height);
        if (std::isfinite(width) && std::isfinite(height) && width >= 240 && height >= 120 && width <= 8192 && height <= 8192 &&
            (width != g_width || height != g_height)) { g_width = width; g_height = height; changed = true; }
    }
    const fake_uid::Status state = fake_uid::Snapshot();
    char original_line[96]{};
    const auto waiting = g_text.Text("uid.waiting", "Waiting for game");
    const std::string_view original_value = state.original_uid[0] ? std::string_view(state.original_uid) :
        (state.original_text[0] ? std::string_view(state.original_text) : std::string_view(waiting));
    const std::array<std::string_view, 1> original{original_value};
    std::snprintf(original_line, sizeof(original_line), "%s", g_text.Format("uid.original", "Original UID: {0}", original).c_str());
    ui->text(ui->user, StringView(original_line));
    changed |= ui->input_text(ui->user, StringView(g_text.Text("settings.display", "Display UID")), g_display, sizeof(g_display), 0) != 0;
    changed |= ui->checkbox(ui->user, StringView(g_text.Text("settings.hide-prefix", "Hide UID prefix")), &g_hide_prefix) != 0;
    if (changed) {
        auto request = g_active; CopyEditor(request); std::string error;
        if (fake_uid::Configure(request, error)) { g_active = request; SaveConfig(); }
        else fake_uid::LogFailure(error);
    }
    const int usable = g_running;
    if (ui->button_enabled(ui->user, StringView(g_text.Text("action.apply", "Apply")), 0, 0, usable)) {
        fake_uid::Settings request{};
        CopyEditor(request);
        request.enabled = 1;
        std::string error;
        if (fake_uid::Configure(request, error)) {
            g_active = request;
            SaveConfig();
        } else {
            fake_uid::LogFailure(error);
        }
    }
    if (ui->button_enabled(ui->user, StringView(g_text.Text("action.revert", "Revert to original")), 0, 0, usable)) {
        g_active.enabled = 0;
        std::string error;
        if (fake_uid::Configure(g_active, error)) SaveConfig();
        else fake_uid::LogFailure("Restore request could not be queued.");
    }
}
CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** context) {
    if (!host || !context) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *context = nullptr;
    const cabbird::sdk::Host services(host);
    g_text = cabbird::sdk::Localizer(host);
    g_core = services.Query<CabbirdCoreServiceV1>(CABBIRD_CORE_SERVICE_V1_ID, 1).get();
    if (!g_core || g_core->struct_size < offsetof(CabbirdCoreServiceV1, log) + sizeof(g_core->log) || !g_core->log)
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    g_ui = services.Query<CabbirdUiServiceV1>(CABBIRD_UI_SERVICE_V1_ID, CABBIRD_UI_SERVICE_V1_VERSION).get();
    if (!g_ui || g_ui->struct_size < offsetof(CabbirdUiServiceV1, button_enabled) + sizeof(g_ui->button_enabled) ||
        !g_ui->input_text || !g_ui->button_enabled || !g_ui->text || !g_ui->checkbox)
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    g_windows = services.Query<CabbirdWindowServiceV1>(CABBIRD_WINDOW_SERVICE_V1_ID, 1).get();
    if (!g_windows || g_windows->struct_size < sizeof(*g_windows) || !g_windows->register_window || !g_windows->begin || !g_windows->end)
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    g_config = services.Query<CabbirdConfigServiceV1>(
        CABBIRD_CONFIG_SERVICE_V1_ID, CABBIRD_CONFIG_SERVICE_V1_VERSION).get();
    if (g_config && (g_config->struct_size < offsetof(CabbirdConfigServiceV1, write_atomic) + sizeof(g_config->write_atomic) ||
        !g_config->register_schema || !g_config->read || !g_config->write_atomic)) g_config = nullptr;
    g_il2cpp = services.Query<CabbirdIl2CppServiceV1>(
        CABBIRD_IL2CPP_SERVICE_V1_ID, CABBIRD_IL2CPP_SERVICE_V1_VERSION).get();
    g_scheduler = services.Query<CabbirdSchedulerServiceV1>(
        CABBIRD_SCHEDULER_SERVICE_V1_ID, CABBIRD_SCHEDULER_SERVICE_V1_VERSION).get();
    if (!g_config || !g_scheduler || g_scheduler->struct_size < sizeof(*g_scheduler) || !g_scheduler->schedule)
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    g_active = {};
    g_hide_prefix = g_active.hide_prefix;
    std::memcpy(g_display, g_active.display_uid, sizeof(g_display));
    g_width = 440; g_height = 260; g_save_failed = false;
    LoadConfig();
    g_save.Bind(host, g_config, kSchemaId, 1);
    if (!g_config) return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    const auto metadata = services.Query<CabbirdUnityMetadataServiceV1>(CABBIRD_UNITY_METADATA_SERVICE_V1_ID, 1);
    const auto hooks = services.Query<CabbirdHookServiceV1>(CABBIRD_HOOK_SERVICE_V1_ID, 1);
    auto profile = cabbird::sdk::LoadProfile(services, "compatibility.profile.json", {"ap", "GameAssembly.dll", "il2cpp", "watermark-uid-v1"});
    const auto core = services.Query<CabbirdCoreServiceV1>(CABBIRD_CORE_SERVICE_V1_ID, 1);
    std::uintptr_t refresh{}, identity{};
    if (profile) {
        const auto entry = cabbird::sdk::ResolveProfileSymbolRva(core.get(), profile.value(), "refreshUidText");
        const auto getter = cabbird::sdk::ResolveProfileSymbolRva(core.get(), profile.value(), "displayPlayerId");
        const auto edge = cabbird::sdk::ResolveProfileSymbolRva(core.get(), profile.value(), "watermarkToPlayerId");
        if (entry && getter && edge && getter.address == edge.address) { refresh = entry.address; identity = getter.address; }
    }
    fake_uid::Bind(g_il2cpp, metadata && metadata->struct_size >= sizeof(*metadata.get()) ? metadata.get() : nullptr,
        hooks && hooks->struct_size >= sizeof(*hooks.get()) ? hooks.get() : nullptr, refresh, identity);
    g_game_lifecycle = services.Query<CabbirdGameLifecycleServiceV1>(CABBIRD_GAME_LIFECYCLE_SERVICE_V1_ID, 1).get();
    if (!g_game_lifecycle || g_game_lifecycle->struct_size < sizeof(*g_game_lifecycle) || !g_game_lifecycle->register_stop_cleanup ||
        !cabbird::sdk::Succeeded(g_game_lifecycle->register_stop_cleanup(g_game_lifecycle->user,
            [](void*) -> CabbirdStatusV1 {
                const auto status = fake_uid::RestoreForStop();
                if (!cabbird::sdk::Succeeded(status)) fake_uid::LogFailure("Game watermark restoration failed during stop.");
                return status;
            }, nullptr, &g_cleanup)))
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    CopyEditor(g_active);
    std::string error;
    if (!fake_uid::Configure(g_active, error)) {
        g_active.enabled = false;
        static_cast<void>(fake_uid::Configure(g_active, error));
        fake_uid::LogFailure("Stored settings were rejected; override disabled.");
    }
    return Ok();
}
CabbirdStatusV1 CABBIRD_CALL Start(void*) {
    std::scoped_lock lock(g_mutex);
    const auto title = g_text.Text("window.title", "FakeUID");
    CabbirdWindowSpecV1 spec{}; spec.struct_size = sizeof(spec); spec.id = StringView("fake-uid.main");
    spec.title = StringView(title); spec.initial_width = g_width; spec.initial_height = g_height;
    spec.minimum_width = 240; spec.minimum_height = 120; spec.maximum_width = 8192; spec.maximum_height = 8192; spec.default_open = 1;
    const auto status = g_windows->register_window(g_windows->user, &spec, &g_window);
    g_running = cabbird::sdk::Succeeded(status);
    return status;
}
CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) {
    std::scoped_lock lock(g_mutex);
    g_running = false;
    const bool saved = g_save.Flush();
    const bool stopped = fake_uid::Shutdown(g_il2cpp);
    if (!stopped) fake_uid::LogFailure("Hook or managed handle cleanup failed during stop.");
    return stopped && saved ? Ok() : CabbirdStatusV1{CABBIRD_STATUS_V1_FAILED, 0, {}};
}
void CABBIRD_CALL Unload(void*) {
    std::scoped_lock lock(g_mutex);
    g_ui = nullptr; g_core = nullptr; g_config = nullptr; g_il2cpp = nullptr; g_scheduler = nullptr;
    g_text = {};
    g_game_lifecycle = nullptr; g_cleanup = {}; g_windows = nullptr; g_window = {};
    g_save.Reset();
    fake_uid::Bind(nullptr, nullptr, nullptr);
}

void CABBIRD_CALL Update(void*, double) {
    {
        std::scoped_lock lock(g_mutex);
        if (!g_running) return;
    }
    // Tick owns all managed objects. Do not hold the UI mutex over engine calls.
    // Keep ticking after Revert so original text can be restored before unload.
    fake_uid::Tick(g_il2cpp);
}
CabbirdStatusV1 CABBIRD_CALL SafeLoad(const CabbirdHostApiV1* host, void** context) { try { return Load(host, context); } catch (...) { return {CABBIRD_STATUS_V1_FAILED, 0, {}}; } }
CabbirdStatusV1 CABBIRD_CALL SafeStart(void* context) { try { return Start(context); } catch (...) { return {CABBIRD_STATUS_V1_FAILED, 0, {}}; } }
CabbirdStatusV1 CABBIRD_CALL SafeStop(void* context, std::uint32_t timeout) { try { return Stop(context, timeout); } catch (...) { return {CABBIRD_STATUS_V1_FAILED, 0, {}}; } }
void CABBIRD_CALL SafeUnload(void* context) { try { Unload(context); } catch (...) {} }
void CABBIRD_CALL SafeUpdate(void* context, double delta) { try { Update(context, delta); } catch (...) {} }
void CABBIRD_CALL SafeDraw(void* context, const CabbirdUiServiceV1* ui) { try { Draw(context, ui); } catch (...) {} }
} // namespace

std::string fake_uid::Tr(std::string_view key, std::string_view english) {
    return g_text.Text(key, english);
}
void fake_uid::LogFailure(std::string_view message) {
    if (!g_core) return;
    const auto line = std::string("FakeUID: ") + std::string(message);
    g_core->log(g_core->user, CABBIRD_CORE_LOG_LEVEL_V1_ERROR, StringView(line));
}

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(CabbirdPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor))
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        StringView("cabbird.fake-uid"), StringView("FakeUID"), StringView("Cabbird"), StringView("1.0.0"),
        SafeLoad, SafeStart, SafeStop, SafeUnload, SafeUpdate, SafeDraw};
    return Ok();
}

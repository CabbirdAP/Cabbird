#include "cabbird/sdk/localization.hpp"

#include <array>
#include <atomic>
#include <string>

namespace {

const CabbirdUiServiceV1* g_ui{};
cabbird::sdk::Localizer g_localizer;
std::string g_window_title;
std::atomic<unsigned long long> g_ticks{};

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** context) {
    const auto ui = cabbird::sdk::Host(host).Query<CabbirdUiServiceV1>(
        CABBIRD_UI_SERVICE_V1_ID, CABBIRD_UI_SERVICE_V1_VERSION);
    constexpr auto text_size = offsetof(CabbirdUiServiceV1, text) +
        sizeof(decltype(CabbirdUiServiceV1::text));
    if (!ui || ui->struct_size < text_size || ui->text == nullptr) {
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_ui = ui.get();
    g_localizer = cabbird::sdk::Localizer(host);
    g_window_title = g_localizer.Label("window.title", "Game Tick Counter", "tick-counter");
    *context = &g_ticks;
    return cabbird::sdk::Ok();
}

CabbirdStatusV1 CABBIRD_CALL Start(void*) { return cabbird::sdk::Ok(); }
CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) { return cabbird::sdk::Ok(); }

void CABBIRD_CALL Unload(void*) {
    g_ui = nullptr;
    g_ticks.store(0, std::memory_order_relaxed);
    g_localizer = {};
    g_window_title.clear();
}

void CABBIRD_CALL Update(void*, double) { g_ticks.fetch_add(1, std::memory_order_relaxed); }

void CABBIRD_CALL Draw(void*, const CabbirdUiServiceV1*) {
    const auto* ui = g_ui;
    int open = 1;
    cabbird::sdk::UiWindow window(ui, g_window_title, &open);
    if (!window) return;
    const auto ticks = std::to_string(g_ticks.load(std::memory_order_relaxed));
    const std::array<std::string_view, 1> arguments{ticks};
    const auto text = g_localizer.Format("ticks.count", "Validated game-thread ticks: {0}", arguments);
    ui->text(ui->user, cabbird::sdk::StringView(text));
}

}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        cabbird::sdk::StringView("cabbird.example.tick-counter"),
        cabbird::sdk::StringView("Game Tick Counter"),
        cabbird::sdk::StringView("Cabbird"), cabbird::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return cabbird::sdk::Ok();
}

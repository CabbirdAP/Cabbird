#include "cabbird/sdk/localization.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {
using cabbird::sdk::Host;
using cabbird::sdk::Ok;
using cabbird::sdk::StringView;
constexpr const char* kSchemaId = "settings";
constexpr const char* kSchema = R"({"type":"object","additionalProperties":false,"required":["value"],"properties":{"value":{"type":"integer","minimum":0,"maximum":4294967295}}})";
const CabbirdUiServiceV1* g_ui{};
const CabbirdConfigServiceV1* g_config{};
const CabbirdSchedulerServiceV1* g_scheduler{};
const CabbirdJsonServiceV1* g_json{};
cabbird::sdk::Localizer g_text;
std::atomic<std::uint32_t> g_value{};
std::atomic<std::uint32_t> g_saved{};
std::atomic_bool g_queued{};
int g_open = 1;

CabbirdByteSpanV1 Bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

void LoadSettings() {
    std::uint32_t version{};
    std::size_t size{};
    const auto probe = g_config->read(g_config->user, StringView(kSchemaId), &version, {}, &size);
    if ((probe.code != CABBIRD_STATUS_V1_OK && probe.code != CABBIRD_STATUS_V1_BUFFER_TOO_SMALL) ||
        version != 1 || size == 0 || size > 1024) return;
    std::vector<std::uint8_t> bytes(size);
    if (g_config->read(g_config->user, StringView(kSchemaId), &version,
        {bytes.data(), bytes.size()}, &size).code != CABBIRD_STATUS_V1_OK ||
        version != 1 || size > bytes.size()) return;
    CabbirdGenerationHandleV1 document{}, value{};
    if (g_json->parse(g_json->user,
        {reinterpret_cast<const char*>(bytes.data()), size}, &document).code != CABBIRD_STATUS_V1_OK) return;
    double number{};
    const bool valid = g_json->object_find(g_json->user, document, StringView("value"), &value).code ==
        CABBIRD_STATUS_V1_OK &&
        g_json->number_value(g_json->user, value, &number).code == CABBIRD_STATUS_V1_OK &&
        std::isfinite(number) && number >= 0 && number <= UINT32_MAX && std::floor(number) == number;
    if (value.id != 0) static_cast<void>(g_json->release(g_json->user, value));
    static_cast<void>(g_json->release(g_json->user, document));
    if (valid) {
        const auto loaded = static_cast<std::uint32_t>(number);
        g_value.store(loaded);
        g_saved.store(loaded);
    }
}

bool SaveSettings() {
    try {
    const auto value = g_value.load();
    if (value == g_saved.load()) return true;
    const std::string document = "{\"value\":" + std::to_string(value) + "}";
    if (g_config->write_atomic(g_config->user, StringView(kSchemaId), 1, Bytes(document)).code !=
        CABBIRD_STATUS_V1_OK) return false;
    g_saved.store(value);
    return true;
    } catch (...) { return false; }
}

void CABBIRD_CALL SaveWorker(void*, CabbirdGenerationHandleV1) {
    static_cast<void>(SaveSettings());
    g_queued.store(false);
}

void QueueSave() {
    if (g_value.load() == g_saved.load() || g_queued.exchange(true)) return;
    CabbirdGenerationHandleV1 task{};
    if (g_scheduler->schedule(g_scheduler->user, 150, SaveWorker, nullptr, &task).code !=
        CABBIRD_STATUS_V1_OK) g_queued.store(false);
}

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *context = nullptr;
    const Host services(host);
    g_ui = services.Query<CabbirdUiServiceV1>(CABBIRD_UI_SERVICE_V1_ID).get();
    g_config = services.Query<CabbirdConfigServiceV1>(CABBIRD_CONFIG_SERVICE_V1_ID).get();
    g_scheduler = services.Query<CabbirdSchedulerServiceV1>(CABBIRD_SCHEDULER_SERVICE_V1_ID).get();
    g_json = services.Query<CabbirdJsonServiceV1>(CABBIRD_JSON_SERVICE_V1_ID).get();
    if (g_ui == nullptr || g_config == nullptr || g_scheduler == nullptr || g_json == nullptr ||
        g_ui->struct_size < offsetof(CabbirdUiServiceV1, button) + sizeof(g_ui->button) ||
        g_config->struct_size < sizeof(CabbirdConfigServiceV1) ||
        g_scheduler->struct_size < sizeof(CabbirdSchedulerServiceV1) ||
        g_json->struct_size < sizeof(CabbirdJsonServiceV1) ||
        g_ui->text == nullptr || g_ui->button == nullptr ||
        g_config->register_schema == nullptr || g_config->read == nullptr ||
        g_config->write_atomic == nullptr || g_scheduler->schedule == nullptr ||
        g_json->parse == nullptr || g_json->object_find == nullptr ||
        g_json->number_value == nullptr || g_json->release == nullptr) {
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_text = cabbird::sdk::Localizer(host);
    g_value.store(0);
    g_saved.store(0);
    g_queued.store(false);
    g_open = 1;
    CabbirdGenerationHandleV1 registration{};
    const auto status = g_config->register_schema(g_config->user, StringView(kSchemaId), 1,
        Bytes(kSchema), &registration);
    if (status.code != CABBIRD_STATUS_V1_OK) return status;
    try { LoadSettings(); } catch (...) {}
    *context = &g_value;
    return Ok();
}

CabbirdStatusV1 CABBIRD_CALL Start(void*) { return Ok(); }
CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) {
    return SaveSettings() ? Ok() : CabbirdStatusV1{CABBIRD_STATUS_V1_FAILED, 0, {}};
}
void CABBIRD_CALL Unload(void*) {
    g_ui = nullptr;
    g_config = nullptr;
    g_scheduler = nullptr;
    g_json = nullptr;
    g_text = {};
}
void CABBIRD_CALL Draw(void*, const CabbirdUiServiceV1*) {
    QueueSave();
    const auto title = g_text.Label("window.title", "Reliable Config", "reliable-config");
    cabbird::sdk::UiWindow window(g_ui, title, &g_open);
    if (!window) return;
    const auto value = std::to_string(g_value.load());
    const std::string_view arguments[]{value};
    const auto text = g_text.Format("settings.value", "Atomic JSON config value: {0}", arguments);
    g_ui->text(g_ui->user, StringView(text));
    if (g_ui->button(g_ui->user, StringView(g_text.Text("settings.increment", "Increment")), 0, 0) &&
        g_value.load() != (std::numeric_limits<std::uint32_t>::max)()) {
        g_value.fetch_add(1);
        QueueSave();
    }
}
}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {
        sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        StringView("cabbird.example.reliable-config"), StringView("Reliable Config"),
        StringView("Cabbird"), StringView("1.2.0"), Load, Start, Stop, Unload, nullptr, Draw};
    return Ok();
}

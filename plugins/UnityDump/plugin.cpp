#include "cabbird/sdk/localization.hpp"
#include "cabbird/sdk/services/platform.h"
#include "cabbird/sdk/services/core.h"
#include "cabbird/sdk/services/ui.h"
#include "cabbird/sdk/services/ui_resources.h"
#include "cabbird/sdk/services/unity.h"
#include "cabbird/sdk/config_save_queue.hpp"
#include "catalog.hpp"

#include <Windows.h>
#include <shellapi.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {
using cabbird::sdk::Ok;
using cabbird::sdk::StringView;
using cabbird::sdk::Succeeded;
constexpr const char* kSchemaId = "unity-dump-settings";
constexpr const char* kSchema = R"({"type":"object","required":["includeFields","includeMethods","recoverMetadata","imageFilter","outputPath","metadataPath","pollEveryFrames","windowWidth","windowHeight"],"properties":{"includeFields":{"type":"boolean"},"includeMethods":{"type":"boolean"},"includeProperties":{"type":"boolean"},"includeInterfaces":{"type":"boolean"},"recoverMetadata":{"type":"boolean"},"imageFilter":{"type":"string","maxLength":511},"outputPath":{"type":"string","maxLength":4095},"metadataPath":{"type":"string","maxLength":4095},"catalogPath":{"type":"string","maxLength":4095},"pollEveryFrames":{"type":"integer","minimum":1,"maximum":600},"windowWidth":{"type":"number","minimum":320,"maximum":8192},"windowHeight":{"type":"number","minimum":240,"maximum":8192}},"additionalProperties":false})";
struct Settings {
    int fields{1}, methods{1}, metadata{1};
    int properties{}, interfaces{};
    char filter[512]{}; // Empty means all loaded images, including Assembly-CSharp.
    char output[4096]{"C:\\AzurPromilia-dump.cs"};
    char metadata_path[4096]{"C:\\global-metadata.decrypted.dat"};
    char catalog_path[4096]{"C:\\AzurPromilia-catalog"};
    std::uint32_t poll_frames{30};
    float width{620}, height{580};
};
struct Context {
    cabbird::sdk::Localizer text;
    const CabbirdCoreServiceV1* core{};
    const CabbirdUiServiceV1* ui{};
    const CabbirdWindowServiceV1* windows{};
    CabbirdGenerationHandleV1 window{};
    const CabbirdUnityDumpServiceV1* dump{};
    const CabbirdConfigServiceV1* config{};
    const CabbirdSchedulerServiceV1* scheduler{};
    cabbird::sdk::ConfigSaveQueue saves;
    unity_dump::Catalog catalog;
    Settings settings;
    CabbirdDumpResultV1 result{sizeof(CabbirdDumpResultV1), CABBIRD_DUMP_V1_RESULT_IDLE};
    std::uint32_t frames{};
    std::uint32_t poll_error{};
    bool running{}, requested{}, save_failed{};
    struct PathAction {
        std::atomic<bool> pending{};
        bool copy{};
        HWND owner{};
        std::string directory;
        std::mutex mutex;
        std::string failure;
    } path_action;
};
CabbirdStatusV1 Failed() { return {CABBIRD_STATUS_V1_FAILED, 0, {}}; }
void LogFailure(Context& ctx, std::string_view message) {
    const auto line = std::string("IL2CPP Dump: ") + std::string(message);
    ctx.core->log(ctx.core->user, CABBIRD_CORE_LOG_LEVEL_V1_ERROR, StringView(line));
}
void LogStatus(Context& ctx, std::string_view operation, CabbirdStatusV1 status) {
    std::string line(operation);
    line += " failed (code=" + std::to_string(status.code) + ")";
    if (status.message.data && status.message.size) {
        line += ": "; line.append(status.message.data, status.message.size);
    }
    LogFailure(ctx, line);
}
std::string Serialize(const Settings& s) {
    return nlohmann::json{{"includeFields", s.fields != 0}, {"includeMethods", s.methods != 0}, {"recoverMetadata", s.metadata != 0},
        {"includeProperties", s.properties != 0}, {"includeInterfaces", s.interfaces != 0},
        {"imageFilter", s.filter}, {"outputPath", s.output}, {"metadataPath", s.metadata_path}, {"catalogPath", s.catalog_path}, {"pollEveryFrames", s.poll_frames},
        {"windowWidth", s.width}, {"windowHeight", s.height}}.dump();
}
void Save(Context& ctx) {
    const bool failed = !ctx.saves.Queue(Serialize(ctx.settings));
    if (failed && !ctx.save_failed) LogFailure(ctx, "Settings could not be queued for saving.");
    ctx.save_failed = failed;
}
template <std::size_t N> bool CopyString(char (&out)[N], const std::string& input) {
    if (input.size() >= N || input.find('\0') != std::string::npos) return false;
    std::memcpy(out, input.data(), input.size()); out[input.size()] = 0; return true;
}
bool LoadSettings(Context& ctx) {
    CabbirdGenerationHandleV1 schema{};
    if (!Succeeded(ctx.config->register_schema(ctx.config->user, StringView(kSchemaId), 1,
        {reinterpret_cast<const std::uint8_t*>(kSchema), std::strlen(kSchema)}, &schema))) return false;
    std::uint32_t version{}; std::size_t size{};
    const auto probe = ctx.config->read(ctx.config->user, StringView(kSchemaId), &version, {nullptr, 0}, &size);
    if (probe.code == CABBIRD_STATUS_V1_NOT_FOUND) return true;
    if (!Succeeded(probe) || version != 1 || !size || size > 32768) {
        LogFailure(ctx, "Stored settings could not be read; defaults retained."); return true;
    }
    std::vector<std::uint8_t> bytes(size);
    if (!Succeeded(ctx.config->read(ctx.config->user, StringView(kSchemaId), &version, {bytes.data(), bytes.size()}, &size)) || size > bytes.size()) return false;
    try {
        const auto json = nlohmann::json::parse(bytes.begin(), bytes.begin() + size);
        Settings loaded;
        loaded.fields = json.at("includeFields").get<bool>(); loaded.methods = json.at("includeMethods").get<bool>();
        loaded.properties = json.value("includeProperties", false); loaded.interfaces = json.value("includeInterfaces", false);
        loaded.metadata = json.at("recoverMetadata").get<bool>(); loaded.poll_frames = json.at("pollEveryFrames").get<std::uint32_t>();
        loaded.width = json.value("windowWidth", 620.0f); loaded.height = json.value("windowHeight", 580.0f);
        if (!CopyString(loaded.filter, json.at("imageFilter").get<std::string>()) ||
            !CopyString(loaded.output, json.at("outputPath").get<std::string>()) ||
            !CopyString(loaded.metadata_path, json.at("metadataPath").get<std::string>()) ||
            !CopyString(loaded.catalog_path, json.value("catalogPath", std::string(loaded.catalog_path))) ||
            loaded.poll_frames < 1 || loaded.poll_frames > 600 || !std::isfinite(loaded.width) || !std::isfinite(loaded.height) ||
            loaded.width < 320 || loaded.height < 240 || loaded.width > 8192 || loaded.height > 8192) throw std::runtime_error("invalid settings");
        ctx.settings = loaded;
    } catch (...) { LogFailure(ctx, "Stored settings were invalid; defaults retained."); }
    return true;
}
void ReportResult(Context& ctx, CabbirdStatusV1 status) {
    if (!ctx.requested || ctx.result.state == CABBIRD_DUMP_V1_RESULT_PENDING) return;
    ctx.requested = false;
    if (ctx.result.state == CABBIRD_DUMP_V1_RESULT_FAILED || ctx.result.state == CABBIRD_DUMP_V1_RESULT_UNAVAILABLE) {
        const auto code = ctx.result.state == CABBIRD_DUMP_V1_RESULT_UNAVAILABLE
            ? CABBIRD_STATUS_V1_UNAVAILABLE : CABBIRD_STATUS_V1_FAILED;
        LogStatus(ctx, "Requested dump", {static_cast<std::uint32_t>(code), 0, status.message});
    } else if (ctx.result.contained_faults) {
        const auto line = "IL2CPP Dump: requested dump is incomplete (contained faults=" + std::to_string(ctx.result.contained_faults) + ").";
        ctx.core->log(ctx.core->user, CABBIRD_CORE_LOG_LEVEL_V1_WARNING, StringView(line));
    }
}
void Poll(Context& ctx) {
    if (!ctx.dump) return;
    CabbirdDumpResultV1 result{sizeof(result)};
    const auto status = ctx.dump->state(ctx.dump->user, &result);
    if (!Succeeded(status)) {
        if (ctx.requested && ctx.poll_error != status.code) LogStatus(ctx, "Reading dump result", status);
        ctx.poll_error = status.code; return;
    }
    ctx.poll_error = CABBIRD_STATUS_V1_OK;
    ctx.result = result; ReportResult(ctx, status);
}
bool Widen(const char* input, std::wstring& output) {
    if (!*input) { output.clear(); return true; }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, static_cast<int>(std::strlen(input)), nullptr, 0);
    if (!size) return false;
    output.resize(static_cast<std::size_t>(size));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, static_cast<int>(std::strlen(input)), output.data(), size) == size;
}
void PathActionFailure(Context& ctx, std::string_view detail) {
    const auto line = ctx.text.Format("error.path-action", "Path action failed: {0}", std::array<std::string_view, 1>{detail});
    ctx.core->log(ctx.core->user, CABBIRD_CORE_LOG_LEVEL_V1_ERROR, StringView(line));
    std::lock_guard lock(ctx.path_action.mutex); ctx.path_action.failure = line;
}
void CABBIRD_CALL RunPathAction(void* raw, CabbirdGenerationHandleV1) noexcept {
    auto& ctx = *static_cast<Context*>(raw); auto& action = ctx.path_action;
    try {
        std::wstring decoded;
        if (!Widen(action.directory.c_str(), decoded)) throw std::runtime_error("Directory must be valid UTF-8");
        const auto directory = std::filesystem::path(decoded).lexically_normal();
        if (!directory.is_absolute()) throw std::runtime_error("Catalog directory must be an absolute path");
        if (action.copy) {
            const auto path = (directory / L"index.json").wstring();
            if (!action.owner || !OpenClipboard(action.owner)) throw std::runtime_error("Cannot open clipboard");
            struct Close { ~Close() { CloseClipboard(); } } close;
            const auto memory = GlobalAlloc(GMEM_MOVEABLE, (path.size() + 1) * sizeof(wchar_t));
            if (!memory) throw std::runtime_error("Cannot allocate clipboard text");
            const auto text = GlobalLock(memory);
            if (!text) { GlobalFree(memory); throw std::runtime_error("Cannot access clipboard text"); }
            std::memcpy(text, path.c_str(), (path.size() + 1) * sizeof(wchar_t)); GlobalUnlock(memory);
            if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, memory)) {
                GlobalFree(memory); throw std::runtime_error("Cannot copy index path");
            }
        } else {
            SHELLEXECUTEINFOW request{sizeof(request)};
            request.fMask = SEE_MASK_FLAG_NO_UI; request.hwnd = action.owner; request.lpVerb = L"open";
            request.lpFile = directory.c_str(); request.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&request)) throw std::runtime_error("Cannot open catalog directory (Windows error " + std::to_string(GetLastError()) + ")");
        }
    } catch (const std::exception& error) { try { PathActionFailure(ctx, error.what()); } catch (...) {} }
    catch (...) { try { PathActionFailure(ctx, "Unexpected path action exception"); } catch (...) {} }
    action.pending.store(false);
}
void QueuePathAction(Context& ctx, bool copy) {
    std::string directory(ctx.settings.catalog_path);
    auto& action = ctx.path_action;
    if (action.pending.exchange(true)) return;
    action.directory = std::move(directory); action.copy = copy; action.owner = GetForegroundWindow();
    { std::lock_guard lock(action.mutex); action.failure.clear(); }
    CabbirdGenerationHandleV1 task{};
    if (!Succeeded(ctx.scheduler->schedule(ctx.scheduler->user, 0, RunPathAction, &ctx, &task))) {
        action.pending.store(false); PathActionFailure(ctx, "Cannot schedule path action");
    }
}
void Request(Context& ctx, bool metadata_only) {
    CabbirdDumpRequestV1 request{}; request.struct_size = sizeof(request);
    if (!metadata_only && ctx.settings.fields) request.flags |= CABBIRD_DUMP_V1_FIELDS;
    if (!metadata_only && ctx.settings.methods) request.flags |= CABBIRD_DUMP_V1_METHODS;
    if (!metadata_only && ctx.settings.properties) request.flags |= CABBIRD_DUMP_V1_PROPERTIES;
    if (!metadata_only && ctx.settings.interfaces) request.flags |= CABBIRD_DUMP_V1_INTERFACES;
    if (metadata_only || ctx.settings.metadata) request.flags |= CABBIRD_DUMP_V1_METADATA;
    request.image_filter = ctx.settings.filter; request.image_filter_size = static_cast<std::uint32_t>(std::strlen(ctx.settings.filter));
    std::wstring output, metadata;
    if ((!metadata_only && !Widen(ctx.settings.output, output)) || !Widen(ctx.settings.metadata_path, metadata)) {
        LogFailure(ctx, "Output paths must be valid UTF-8."); return;
    }
    request.output_path = output.empty() ? nullptr : output.data(); request.output_path_length = static_cast<std::uint32_t>(output.size());
    request.metadata_path = metadata.empty() ? nullptr : metadata.data(); request.metadata_path_length = static_cast<std::uint32_t>(metadata.size());
    CabbirdDumpResultV1 result{sizeof(result)};
    const auto status = ctx.dump->run(ctx.dump->user, &request, &result);
    if (Succeeded(status)) {
        ctx.requested = true; ctx.result = result; ctx.poll_error = CABBIRD_STATUS_V1_OK;
        ReportResult(ctx, status);
    } else if (status.code != CABBIRD_STATUS_V1_CONFLICT) {
        ctx.result = result; ctx.result.state = CABBIRD_DUMP_V1_RESULT_FAILED;
        LogStatus(ctx, "Starting dump", status);
    }
}
CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** output) {
    try {
        if (!host || !output) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
        *output = nullptr;
        auto ctx = std::make_unique<Context>(); ctx->text = cabbird::sdk::Localizer(host);
        const cabbird::sdk::Host services(host);
        ctx->core = services.Query<CabbirdCoreServiceV1>(CABBIRD_CORE_SERVICE_V1_ID, 1).get();
        ctx->ui = services.Query<CabbirdUiServiceV1>(CABBIRD_UI_SERVICE_V1_ID, 1).get();
        ctx->windows = services.Query<CabbirdWindowServiceV1>(CABBIRD_WINDOW_SERVICE_V1_ID, 1).get();
        ctx->config = services.Query<CabbirdConfigServiceV1>(CABBIRD_CONFIG_SERVICE_V1_ID, 1).get();
        ctx->scheduler = services.Query<CabbirdSchedulerServiceV1>(CABBIRD_SCHEDULER_SERVICE_V1_ID, 1).get();
        if (!ctx->core || ctx->core->struct_size < offsetof(CabbirdCoreServiceV1, log) + sizeof(ctx->core->log) || !ctx->core->log ||
            !ctx->ui || ctx->ui->struct_size < offsetof(CabbirdUiServiceV1, button_enabled) + sizeof(ctx->ui->button_enabled) ||
            !ctx->ui->input_text || !ctx->ui->checkbox || !ctx->ui->button_enabled ||
            !ctx->windows || ctx->windows->struct_size < sizeof(*ctx->windows) || !ctx->windows->register_window || !ctx->windows->begin || !ctx->windows->end ||
            !ctx->config || ctx->config->struct_size < sizeof(*ctx->config) || !ctx->config->register_schema || !ctx->config->read || !ctx->config->write_atomic ||
            !ctx->scheduler || ctx->scheduler->struct_size < sizeof(*ctx->scheduler) || !ctx->scheduler->schedule)
            return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
        ctx->dump = services.Query<CabbirdUnityDumpServiceV1>(CABBIRD_UNITY_DUMP_SERVICE_V1_ID, 1).get();
        if (ctx->dump && (ctx->dump->struct_size < offsetof(CabbirdUnityDumpServiceV1, state) + sizeof(ctx->dump->state) || !ctx->dump->run || !ctx->dump->state)) ctx->dump = nullptr;
        if (!LoadSettings(*ctx)) return Failed();
        ctx->saves.Bind(host, ctx->config, kSchemaId, 1);
        if (!ctx->catalog.Bind(host, &ctx->text)) return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
        *output = ctx.release(); return Ok();
    } catch (...) { return Failed(); }
}
CabbirdStatusV1 CABBIRD_CALL Start(void* raw) {
    try {
        auto& ctx = *static_cast<Context*>(raw); const auto title = ctx.text.Text("window.title", "IL2CPP Dump");
        CabbirdWindowSpecV1 spec{}; spec.struct_size = sizeof(spec); spec.id = StringView("unity-dump.main"); spec.title = StringView(title);
        spec.initial_width = ctx.settings.width; spec.initial_height = ctx.settings.height; spec.minimum_width = 320; spec.minimum_height = 240;
        spec.maximum_width = 8192; spec.maximum_height = 8192; spec.default_open = 1;
        const auto status = ctx.windows->register_window(ctx.windows->user, &spec, &ctx.window);
        ctx.running = Succeeded(status); return status;
    } catch (...) { return Failed(); }
}
CabbirdStatusV1 CABBIRD_CALL Stop(void* raw, std::uint32_t) {
    try { auto& ctx = *static_cast<Context*>(raw); ctx.running = false; return ctx.saves.Flush() ? Ok() : Failed(); } catch (...) { return Failed(); }
}
void CABBIRD_CALL Unload(void* raw) { try { delete static_cast<Context*>(raw); } catch (...) {} }
void CABBIRD_CALL Update(void* raw, double) noexcept {
    try { static_cast<Context*>(raw)->catalog.Update(); } catch (...) {}
}
void CABBIRD_CALL Draw(void* raw, const CabbirdUiServiceV1*) {
    try {
        auto& ctx = *static_cast<Context*>(raw); auto* ui = ctx.ui; auto& s = ctx.settings;
        if (++ctx.frames >= s.poll_frames) { ctx.frames = 0; Poll(ctx); }
        std::int32_t visible{};
        if (!ctx.window.id || !Succeeded(ctx.windows->begin(ctx.windows->user, ctx.window, 0, &visible))) return;
        struct EndWindow { Context& ctx; ~EndWindow() { static_cast<void>(ctx.windows->end(ctx.windows->user, ctx.window)); } } end{ctx};
        if (!visible) return;
        bool changed{};
        if (ui->get_window_size) {
            float width{}, height{}; ui->get_window_size(ui->user, &width, &height);
            if (std::isfinite(width) && std::isfinite(height) && width >= 320 && height >= 240 && width <= 8192 && height <= 8192 && (width != s.width || height != s.height)) {
                s.width = width; s.height = height; changed = true;
            }
        }
        changed |= ui->checkbox(ui->user, StringView(ctx.text.Text("settings.fields", "Fields (names, types, offsets)")), &s.fields) != 0;
        changed |= ui->checkbox(ui->user, StringView(ctx.text.Text("settings.methods", "Methods (+ signatures, RVAs)")), &s.methods) != 0;
        changed |= ui->checkbox(ui->user, StringView(ctx.text.Text("settings.properties", "Properties (types, accessors)")), &s.properties) != 0;
        changed |= ui->checkbox(ui->user, StringView(ctx.text.Text("settings.interfaces", "Implemented interfaces")), &s.interfaces) != 0;
        changed |= ui->checkbox(ui->user, StringView(ctx.text.Text("settings.metadata", "Recover plaintext metadata")), &s.metadata) != 0;
        changed |= ui->input_text(ui->user, StringView(ctx.text.Text("settings.filter", "Method image filter")), s.filter, sizeof(s.filter), 0) != 0;
        if (ui->button_enabled(ui->user, StringView(ctx.text.Text("action.all-images", "All method images")), 0, 0, *s.filter != 0)) { s.filter[0] = 0; changed = true; }
        changed |= ui->input_text(ui->user, StringView(ctx.text.Text("settings.output", "Output file")), s.output, sizeof(s.output), 0) != 0;
        changed |= ui->input_text(ui->user, StringView(ctx.text.Text("settings.metadata-path", "Metadata file")), s.metadata_path, sizeof(s.metadata_path), 0) != 0;
        changed |= ui->input_text(ui->user, StringView(ctx.text.Text("settings.catalog-path", "Resource / Lua directory")), s.catalog_path, sizeof(s.catalog_path), 0) != 0;
        const int path_enabled = ctx.running && *s.catalog_path && !ctx.path_action.pending.load();
        if (ui->button_enabled(ui->user, StringView(ctx.text.Text("action.open-catalog", "Open catalog directory")), 0, 0, path_enabled)) QueuePathAction(ctx, false);
        if (ui->button_enabled(ui->user, StringView(ctx.text.Text("action.copy-index", "Copy index path")), 0, 0, path_enabled)) QueuePathAction(ctx, true);
        std::string path_failure;
        { std::lock_guard lock(ctx.path_action.mutex); path_failure = ctx.path_action.failure; }
        if (!path_failure.empty()) ui->text(ui->user, StringView(path_failure));
        if (changed) Save(ctx);
        const bool busy = ctx.result.state == CABBIRD_DUMP_V1_RESULT_PENDING;
        const int enabled = ctx.running && ctx.dump && !busy;
        if (ui->button_enabled(ui->user, StringView(ctx.text.Text("action.dump", "Dump now")), 0, 0, enabled && (s.fields || s.methods || s.properties || s.interfaces || s.metadata))) Request(ctx, false);
        if (ui->button_enabled(ui->user, StringView(ctx.text.Text("action.metadata", "Metadata only")), 0, 0, enabled)) Request(ctx, true);
        if (ui->button_enabled(ui->user, StringView(ctx.text.Text("action.catalog", "Export resource / Lua catalog")), 0, 0, ctx.running && !ctx.catalog.Busy() && *s.catalog_path)) ctx.catalog.Request(s.catalog_path);
        const auto catalog_result = ctx.catalog.Result();
        if (!catalog_result.empty()) {
            if (ui->struct_size >= offsetof(CabbirdUiServiceV1, text_wrapped) + sizeof(ui->text_wrapped) && ui->text_wrapped)
                ui->text_wrapped(ui->user, StringView(catalog_result));
            else ui->text(ui->user, StringView(catalog_result));
        }
    } catch (...) {}
}
} // namespace
CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(CabbirdPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor) || descriptor->api_major != CABBIRD_PLUGIN_API_V1_MAJOR)
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        StringView("cabbird.unity-dump"), StringView("IL2CPP Dump"), StringView("Cabbird"), StringView("1.1.3"), Load, Start, Stop, Unload, Update, Draw};
    return Ok();
}

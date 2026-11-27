#include "cabbird/plugin_support/spatial.hpp"
#include "cabbird/sdk/services/game_lifecycle.h"
#include "cabbird/sdk/services/player_motion.h"
#include "cabbird/sdk/services/unity_metadata.h"

#include <chrono>

namespace {
using namespace cabbird::plugins;
using cabbird::sdk::Ok;
using Method = CabbirdMethodInfoV1;
using Object = CabbirdIl2CppObjectV1;
using Runtime = cabbird::sdk::il2cpp::Context;
struct Vector3 { float x{}, y{}, z{}; };
static_assert(sizeof(Vector3) == 12);
struct Settings {
    int enabled{};
    float speed{10};
    bool operator==(const Settings&) const = default;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Settings, enabled, speed)
Json Schema() { return ObjectSchema({{"enabled", ToggleSchema()}, {"speed", NumberSchema(1, 200)}}); }
bool Valid(const Settings& settings) { return Toggle(settings.enabled) && Range(settings.speed, 1, 200); }
struct Context {
    explicit Context(const CabbirdHostApiV1* host) : text(host) {}
    cabbird::sdk::Localizer text;
    ConfigStore config;
    Settings settings;
    std::atomic_bool enabled{}, running{};
    std::atomic<float> speed{10};
    std::atomic<std::uint32_t> reported{};
    const CabbirdCoreServiceV1* core{};
    const CabbirdUiServiceV1* ui{};
    const CabbirdWindowServiceV1* windows{};
    const CabbirdInputServiceV1* input{};
    const CabbirdUnityPlayerServiceV1* player{};
    const CabbirdIl2CppServiceV1* il2cpp{};
    const CabbirdUnityMetadataServiceV1* metadata{};
    const CabbirdUnityPlayerMotionServiceV1* motion{};
    const CabbirdGameLifecycleServiceV1* lifecycle{};
    CabbirdGenerationHandleV1 window{}, cleanup{}, motionLease{};
    std::uint64_t motionEntity{}; // Game-owned identity, not a retained Unity object.
    const Method *mainCamera{}, *componentTransform{}, *forward{}, *focused{};
    bool methodsReady{};
    std::chrono::steady_clock::time_point nextBind{};
};
void LogOnce(Context& c, unsigned bit, const char* key, const char* fallback) noexcept {
    try {
        if ((c.reported.fetch_or(bit) & bit) == 0)
            c.core->log(c.core->user, CABBIRD_CORE_LOG_LEVEL_V1_WARNING, StringView(c.text.Text(key, fallback)));
    } catch (...) {}
}
bool ReleaseMotion(Context& c) noexcept {
    if (!c.motionLease.id) return true;
    const auto status = c.motion->release(c.motion->user, c.motionLease);
    if (!Succeeded(status) && status.code != CABBIRD_STATUS_V1_NOT_FOUND) return false;
    c.motionLease = {};
    c.motionEntity = 0;
    return true;
}
const Method* CameraMethod(Context& c, const Runtime& runtime, const char* ns, const char* cls,
                           const char* name, const char* result, bool isStatic) {
    auto* klass = runtime.FindClassAnyImage(ns, cls);
    if (!klass) return nullptr;
    const auto fullName = std::string(ns) + '.' + cls;
    CabbirdUnityMethodRequestV1 request{sizeof(request)};
    request.method_kind = isStatic ? CABBIRD_UNITY_METHOD_V1_STATIC : CABBIRD_UNITY_METHOD_V1_INSTANCE;
    request.module_name = StringView("GameAssembly.dll"); request.type_name = StringView(fullName);
    request.method_name = StringView(name); request.return_type = StringView(result);
    CabbirdUnityMethodResultV1 resolved{sizeof(resolved)};
    if (!Succeeded(c.metadata->resolve_method(c.metadata->user, &request, &resolved))) return nullptr;
    const auto* method = runtime.FindMethodBySignature(klass, name, {});
    const auto& api = runtime.Functions();
    std::uint32_t implementation{};
    return method && runtime.TypeName(api.il2cpp_method_get_return_type(method)) == result &&
        (((api.il2cpp_method_get_flags(method, &implementation) & 0x10U) != 0) == isStatic) ? method : nullptr;
}
bool BindCamera(Context& c, const Runtime& runtime) {
    c.mainCamera = CameraMethod(c, runtime, "UnityEngine", "Camera", "get_main", "UnityEngine.Camera", true);
    c.componentTransform = CameraMethod(c, runtime, "UnityEngine", "Component", "get_transform", "UnityEngine.Transform", false);
    c.forward = CameraMethod(c, runtime, "UnityEngine", "Transform", "get_forward", "UnityEngine.Vector3", false);
    c.focused = CameraMethod(c, runtime, "UnityEngine", "Application", "get_isFocused", "System.Boolean", true);
    c.methodsReady = c.mainCamera && c.componentTransform && c.forward && c.focused;
    return c.methodsReady;
}
bool Key(const CabbirdInputSnapshotV1& input, unsigned key) {
    return (input.keys[key / 8U] & (1U << (key % 8U))) != 0;
}
Vector3 Direction(const CabbirdInputSnapshotV1& input, Vector3 forward, bool focused) {
    if (!focused || (input.capture_flags & (CABBIRD_INPUT_CAPTURE_V1_KEYBOARD | CABBIRD_INPUT_CAPTURE_V1_TEXT))) return {};
    const auto length = std::sqrt(forward.x * forward.x + forward.z * forward.z);
    if (!std::isfinite(length)) return {};
    if (length < 0.001F) { forward.x = 0; forward.z = 0; }
    else { forward.x /= length; forward.z /= length; }
    const float longitudinal = static_cast<float>(Key(input, 'W')) - static_cast<float>(Key(input, 'S'));
    const float lateral = static_cast<float>(Key(input, 'D')) - static_cast<float>(Key(input, 'A'));
    const float vertical = static_cast<float>(Key(input, VK_SPACE)) -
        static_cast<float>(Key(input, VK_CONTROL) || Key(input, VK_LCONTROL) || Key(input, VK_RCONTROL));
    Vector3 value{forward.x * longitudinal + forward.z * lateral, vertical,
                  forward.z * longitudinal - forward.x * lateral};
    const auto magnitude = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (magnitude > 1) { value.x /= magnitude; value.y /= magnitude; value.z /= magnitude; }
    return value;
}
struct CameraWork { Context* context; Vector3 velocity; };
CabbirdStatusV1 CABBIRD_CALL ReadCamera(void* raw, const CabbirdIl2CppApiV1* api) noexcept {
    try {
        if (!api || api->struct_size < sizeof(*api) || api->api_version != 1) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        auto& work = *static_cast<CameraWork*>(raw);
        auto& c = *work.context;
        Runtime runtime(*api);
        if (!c.methodsReady) {
            const auto now = std::chrono::steady_clock::now();
            if (now < c.nextBind) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
            c.nextBind = now + std::chrono::seconds(2);
            if (!BindCamera(c, runtime)) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        }
        Object *camera{}, *transform{}, *value{}, *focus{};
        if (!runtime.Invoke(c.mainCamera, nullptr, nullptr, &camera) || !camera ||
            !runtime.Invoke(c.componentTransform, camera, nullptr, &transform) || !transform ||
            !runtime.Invoke(c.forward, transform, nullptr, &value) || !value) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        const auto* forward = static_cast<const Vector3*>(api->il2cpp_object_unbox(value));
        if (!forward) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        const auto direction = *forward;
        if (!runtime.Invoke(c.focused, nullptr, nullptr, &focus) || !focus) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        const auto* focused = static_cast<const bool*>(api->il2cpp_object_unbox(focus));
        if (!focused) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        CabbirdInputSnapshotV1 input{sizeof(input)};
        if (!Succeeded(c.input->snapshot(c.input->user, &input))) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        work.velocity = Direction(input, direction, *focused);
        const auto speed = c.speed.load();
        work.velocity.x *= speed; work.velocity.y *= speed; work.velocity.z *= speed;
        return Ok();
    } catch (...) { return Status(CABBIRD_STATUS_V1_FAILED); }
}
void CABBIRD_CALL Update(void* raw, double) noexcept {
    auto& c = *static_cast<Context*>(raw);
    try {
        if (!c.running.load() || !c.enabled.load()) { static_cast<void>(ReleaseMotion(c)); return; }
        CabbirdUnityPlayerSnapshotV1 player{sizeof(player)};
        if (!Succeeded(c.player->snapshot(c.player->user, &player)) ||
            !(player.flags & CABBIRD_UNITY_PLAYER_V1_VALID) || !player.entity_id) {
            static_cast<void>(ReleaseMotion(c)); return;
        }
        if (c.motionLease.id && c.motionEntity != player.entity_id && !ReleaseMotion(c)) return;
        CameraWork work{&c};
        if (!Succeeded(c.il2cpp->with_runtime(c.il2cpp->user, ReadCamera, &work))) {
            static_cast<void>(ReleaseMotion(c));
            LogOnce(c, 2, "log.layout", "Free Flight: camera metadata or runtime is unavailable.");
            return;
        }
        CabbirdUnityPlayerMotionRequestV1 request{sizeof(request)};
        request.mode = CABBIRD_UNITY_PLAYER_MOTION_V1_OVERRIDE_VELOCITY;
        request.flags = CABBIRD_UNITY_PLAYER_MOTION_V1_FREE_GROUND;
        request.entity_id = player.entity_id;
        request.velocity[0] = work.velocity.x; request.velocity[1] = work.velocity.y; request.velocity[2] = work.velocity.z;
        const auto status = c.motionLease.id ? c.motion->update(c.motion->user, c.motionLease, &request)
            : c.motion->acquire(c.motion->user, &request, &c.motionLease);
        if (Succeeded(status)) c.motionEntity = player.entity_id;
        else {
            static_cast<void>(ReleaseMotion(c));
            LogOnce(c, 16, "log.motion", "Free Flight: shared movement control is unavailable.");
        }
    } catch (...) {
        static_cast<void>(ReleaseMotion(c));
        LogOnce(c, 4, "log.update", "Free Flight: Game-domain preparation failed.");
    }
}
CabbirdStatusV1 CABBIRD_CALL Cleanup(void* raw) noexcept {
    auto& c = *static_cast<Context*>(raw);
    c.running.store(false);
    return ReleaseMotion(c) ? Ok() : Status(CABBIRD_STATUS_V1_FAILED);
}
CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** result) noexcept {
    if (!host || !result) return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    *result = nullptr;
    try {
        auto c = std::make_unique<Context>(host);
        c->core = Query<CabbirdCoreServiceV1>(host, CABBIRD_CORE_SERVICE_V1_ID);
        c->ui = Query<CabbirdUiServiceV1>(host, CABBIRD_UI_SERVICE_V1_ID);
        c->windows = Query<CabbirdWindowServiceV1>(host, CABBIRD_WINDOW_SERVICE_V1_ID);
        c->input = Query<CabbirdInputServiceV1>(host, CABBIRD_INPUT_SERVICE_V1_ID);
        c->player = Query<CabbirdUnityPlayerServiceV1>(host, CABBIRD_UNITY_PLAYER_SERVICE_V1_ID);
        c->il2cpp = Query<CabbirdIl2CppServiceV1>(host, CABBIRD_IL2CPP_SERVICE_V1_ID);
        c->metadata = Query<CabbirdUnityMetadataServiceV1>(host, CABBIRD_UNITY_METADATA_SERVICE_V1_ID);
        c->motion = Query<CabbirdUnityPlayerMotionServiceV1>(host, CABBIRD_UNITY_PLAYER_MOTION_SERVICE_V1_ID);
        c->lifecycle = Query<CabbirdGameLifecycleServiceV1>(host, CABBIRD_GAME_LIFECYCLE_SERVICE_V1_ID);
        if (!c->core || !c->core->log || !c->ui || !c->ui->checkbox || !c->ui->slider_float ||
            !c->windows || !c->windows->register_window || !c->windows->release_window || !c->windows->state ||
            !c->windows->begin || !c->windows->end || !c->input || !c->input->snapshot ||
            !c->player || !c->player->snapshot || !c->il2cpp || !c->il2cpp->with_runtime ||
            !c->metadata || !c->metadata->resolve_method || !c->motion || !c->motion->acquire ||
            !c->motion->update || !c->motion->release || !c->lifecycle ||
            !c->lifecycle->register_stop_cleanup || !c->lifecycle->unregister_stop_cleanup) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        const auto status = c->config.Bind(host, "free-flight-settings", Schema());
        if (!Succeeded(status)) return status;
        try {
            if (const auto saved = c->config.Read()) {
                const auto settings = saved->get<Settings>();
                if (Valid(settings)) c->settings = settings;
            }
        } catch (...) { LogOnce(*c, 32, "log.settings", "Free Flight: saved settings are invalid; defaults retained."); }
        c->enabled.store(c->settings.enabled != 0); c->speed.store(c->settings.speed);
        const auto cleanup = c->lifecycle->register_stop_cleanup(c->lifecycle->user, Cleanup, c.get(), &c->cleanup);
        if (!Succeeded(cleanup)) return cleanup;
        *result = c.release(); return Ok();
    } catch (...) { return Status(CABBIRD_STATUS_V1_FAILED); }
}
CabbirdStatusV1 CABBIRD_CALL Start(void* raw) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        const auto title = c.text.Text("window.title", "Free Flight");
        CabbirdWindowSpecV1 spec{sizeof(spec)};
        spec.id = StringView("free-flight.main"); spec.title = StringView(title);
        spec.initial_width = 420; spec.initial_height = 180; spec.minimum_width = 280; spec.minimum_height = 100; spec.default_open = 1;
        const auto status = c.windows->register_window(c.windows->user, &spec, &c.window);
        if (Succeeded(status)) c.running.store(true);
        return status;
    } catch (...) { return Status(CABBIRD_STATUS_V1_FAILED); }
}
CabbirdStatusV1 CABBIRD_CALL Stop(void* raw, std::uint32_t) noexcept {
    auto& c = *static_cast<Context*>(raw);
    try {
        c.running.store(false);
        bool released = ReleaseMotion(c);
        const bool saved = c.config.Stop();
        if (c.window.id) {
            const auto status = c.windows->release_window(c.windows->user, c.window);
            released = released && (Succeeded(status) || status.code == CABBIRD_STATUS_V1_NOT_FOUND); c.window = {};
        }
        if (c.cleanup.id) {
            const auto status = c.lifecycle->unregister_stop_cleanup(c.lifecycle->user, c.cleanup);
            released = released && (Succeeded(status) || status.code == CABBIRD_STATUS_V1_NOT_FOUND); c.cleanup = {};
        }
        return saved && released ? Ok() : Status(CABBIRD_STATUS_V1_FAILED);
    } catch (...) { return Status(CABBIRD_STATUS_V1_FAILED); }
}
void CABBIRD_CALL Unload(void* raw) noexcept { try { delete static_cast<Context*>(raw); } catch (...) {} }
void CABBIRD_CALL Draw(void* raw, const CabbirdUiServiceV1*) noexcept {
    auto& c = *static_cast<Context*>(raw);
    try {
        CabbirdWindowStateV1 state{sizeof(state)};
        if (!Succeeded(c.windows->state(c.windows->user, c.window, &state)) || !state.open) return;
        std::int32_t visible{};
        if (!Succeeded(c.windows->begin(c.windows->user, c.window, 0, &visible))) return;
        struct End { Context& c; ~End() { static_cast<void>(c.windows->end(c.windows->user, c.window)); } } end{c};
        if (!visible) return;
        const auto before = c.settings;
        c.ui->checkbox(c.ui->user, StringView(c.text.Text("settings.enabled", "Enabled")), &c.settings.enabled);
        c.ui->slider_float(c.ui->user, StringView(c.text.Text("settings.speed", "Flight speed (m/s)")), &c.settings.speed, 1, 200);
        if (c.settings != before && Valid(c.settings)) {
            c.enabled.store(c.settings.enabled != 0); c.speed.store(c.settings.speed);
            if (!c.config.Queue(Json(c.settings))) LogOnce(c, 64, "log.save", "Free Flight: settings could not be queued for saving.");
        }
    } catch (...) { LogOnce(c, 128, "log.draw", "Free Flight: the window callback failed."); }
}
} // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(CabbirdPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor) || descriptor->api_major != CABBIRD_PLUGIN_API_V1_MAJOR)
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    descriptor->id = StringView("cabbird.free-flight"); descriptor->name = StringView("Free Flight");
    descriptor->author = StringView("Cabbird"); descriptor->version = StringView("1.0.0");
    descriptor->on_load = Load; descriptor->on_start = Start; descriptor->on_stop = Stop;
    descriptor->on_unload = Unload; descriptor->on_update = Update; descriptor->on_draw = Draw;
    return Ok();
}

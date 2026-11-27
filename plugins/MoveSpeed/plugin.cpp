/* Scale verified per-frame locomotion inputs, before the game adds skill,
 * impulse and gravity velocity. Never alter cached attributes or global time.
 */
#include "cabbird/sdk/cpp.hpp"
#include "cabbird/sdk/localization.hpp"
#include "cabbird/sdk/profile.hpp"
#include "cabbird/sdk/services/il2cpp.h"
#include "cabbird/sdk/config_save_queue.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>

using cabbird::sdk::Host;
using cabbird::sdk::Ok;
using cabbird::sdk::StringView;
using cabbird::sdk::Succeeded;

namespace {
constexpr float kMinimumFactor = 0.1f, kMaximumFactor = 20.0f, kDefaultFactor = 2.0f;
constexpr std::uint32_t kSettingsVersion = 2;
constexpr std::string_view kSchemaId = "movement-settings";
constexpr std::string_view kSchema = R"({"type":"object","additionalProperties":false,
"required":["enabled","playerOnly","factor"],"properties":{"enabled":{"type":"boolean"},
"playerOnly":{"type":"boolean"},"factor":{"type":"number","minimum":0.1,"maximum":20}}})";
constexpr std::string_view kWorld = "Lens.Gameplay.Modules.BigWorld.";

enum Native : std::size_t { BaseMove, Controllable, MountRun, MountSprint, World,
    SpeedLimit, RootMotion, CurveJump, CurveDouble, FastJump, StandJumpUpdate,
    JumpLoopMove, JumpLoopLimit, MountJumpCurve,
    FlightMove, FlightIdleMove, FlightCurve, FlightLimit, FlightSprint,
    HookFlightMove, HookFlightIdleMove, HookFlightCurve, HookFlightLimit, HookFlightSprint,
    TargetSpeed, SetTargetSpeed, NativeCount };
constexpr std::size_t kHookCount = TargetSpeed;
struct Method final { const char *symbol, *type, *name, *result, *parameter; };
// Runtime identities stay in code; addresses, bytes and field offsets stay in Profile.
constexpr std::array<Method,NativeCount> kMethods{{
    {"baseMove","Common.BaseState","Move","System.Void","System.Boolean"},
    {"controllableRun","Controllable.RunState","CalcDesiredVelocity","UnityEngine.Vector3",nullptr},
    {"mountRun","Mount.RunState","CalcDesiredVelocity","UnityEngine.Vector3",nullptr},
    {"mountSprint","Mount.MountSprintState","CalcDesiredVelocity","UnityEngine.Vector3",nullptr},
    {"worldDecorate","MoveSystem.WorldMoveBehavior","Decorate","System.Void","Lens.Gameplay.Modules.BigWorld.MoveSystem.DoMoveState"},
    {"groundSpeedLimit","Common.BaseState","GetMaxSpeed","System.Single",nullptr},
    {"rootMotionVelocity","AliveEntity","GetRootMotionVelocity","UnityEngine.Vector3","System.Single"},
    {"jumpCurve","Common.CurveJumpState","CalcDesiredCurveVelocity","UnityEngine.Vector3",nullptr},
    {"doubleJumpCurve","Common.CurveDoubleJumpState","CalcDesiredCurveVelocity","UnityEngine.Vector3",nullptr},
    {"fastJumpCurve","Hero.FastJumpState","CalcDesiredCurveVelocity","UnityEngine.Vector3",nullptr},
    {"standJumpUpdate","Common.CurveStandJumpState","OnUpdate","System.Void",nullptr},
    {"jumpLoopMove","Common.JumpLoop","Move","System.Void","System.Boolean"},
    {"jumpLoopLimit","Common.JumpLoop","GetMaxSpeed","System.Single",nullptr},
    {"mountJumpCurve","Mount.BaseCurveJumpState","CalcDesiredCurveVelocity","UnityEngine.Vector3",nullptr},
    {"flightMove","Mount.FlyState","Move","System.Void","System.Boolean"},
    {"flightIdleMove","Mount.FlyIdleState","Move","System.Void","System.Boolean"},
    {"flightCurve","Mount.FlyState","CalcDesiredCurveVelocity","UnityEngine.Vector3",nullptr},
    {"flightLimit","Mount.FlyState","GetMaxSpeed","System.Single",nullptr},
    {"flightSprint","Mount.FlySprintState","CalcDesiredVelocity","UnityEngine.Vector3",nullptr},
    {"hookFlightMove","Mount.HookFlyState","Move","System.Void","System.Boolean"},
    {"hookFlightIdleMove","Mount.HookFlyIdleState","Move","System.Void","System.Boolean"},
    {"hookFlightCurve","Mount.HookFlyState","CalcDesiredCurveVelocity","UnityEngine.Vector3",nullptr},
    {"hookFlightLimit","Mount.HookFlyState","GetMaxSpeed","System.Single",nullptr},
    {"hookFlightSprint","Mount.HookFlySprintState","CalcDesiredVelocity","UnityEngine.Vector3",nullptr},
    {"targetSpeed","MoveSystem.DoMoveState","get_TargetSpeed","System.Single",nullptr},
    {"setTargetSpeed","MoveSystem.DoMoveState","set_TargetSpeed","System.Void","System.Single"}
}};
enum class BindingState { waiting, ready, entry, metadata, fields, dependency, alias, hook };
struct HookSlot final {
    std::uintptr_t target{}, original{};
    CabbirdGenerationHandleV1 handle{};
    std::atomic<BindingState> state{BindingState::waiting};
};
struct Owner final { std::uintptr_t state{}, entity{}, data{}; float factor{1}; bool flight{}; };
struct Context final {
    const CabbirdCoreServiceV1* core{};
    const CabbirdHookServiceV1* hooks{};
    const CabbirdConfigServiceV1* config{};
    const CabbirdJsonServiceV1* json{};
    const CabbirdUnityPlayerServiceV1* player{};
    const CabbirdUnityMetadataServiceV1* metadata{};
    const CabbirdIl2CppServiceV1* il2cpp{};
    const CabbirdWindowServiceV1* windows{};
    CabbirdGenerationHandleV1 window{};
    cabbird::sdk::Localizer localizer;
    cabbird::sdk::Profile profile;
    std::array<HookSlot,NativeCount> methods;
    std::uint32_t state_entity{}, steering_entity{}, handle_entity{}, entity_data{};
    std::atomic<DWORD> game_thread{};
    std::atomic_bool requested_enabled{}, player_only{true};
    std::uint32_t state_data{}, state_type{};
    std::array<std::uint32_t,5> move_states{};
    std::array<std::uint32_t,2> mount_jump_states{};
    bool ownership_ready{}, states_ready{};
    double bind_remaining{};
    std::string bind_issue,last_bind_issue;
    std::atomic<float> factor{kDefaultFactor};
    // Only the observed Game thread may access this synchronous nested scope.
    // It spans one native Move/OnUpdate and its original input consumers.
    Owner locomotion;
    cabbird::sdk::ConfigSaveQueue save_queue;
    std::atomic_bool save_failed{}, settings_warning{};
};
std::atomic<Context*> g_active{};
std::string g_load_error;
#include "preferences.hpp"
#include "movement_runtime.hpp"

CabbirdStatusV1 Unavailable(Context& context, std::string_view key, std::string_view fallback) {
    g_load_error = context.localizer.Text(key, fallback);
    return {CABBIRD_STATUS_V1_UNAVAILABLE,0,StringView(g_load_error)};
}
CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* api, void** output) noexcept {
    if (!api || !output) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT,0,{}};
    *output = nullptr;
    try {
        auto c = std::make_unique<Context>();
        c->localizer = cabbird::sdk::Localizer(api);
        const Host host(api);
        c->core = host.Query<CabbirdCoreServiceV1>(CABBIRD_CORE_SERVICE_V1_ID).get();
        c->hooks = host.Query<CabbirdHookServiceV1>(CABBIRD_HOOK_SERVICE_V1_ID).get();
        c->config = host.Query<CabbirdConfigServiceV1>(CABBIRD_CONFIG_SERVICE_V1_ID).get();
        c->json = host.Query<CabbirdJsonServiceV1>(CABBIRD_JSON_SERVICE_V1_ID).get();
        c->player = host.Query<CabbirdUnityPlayerServiceV1>(CABBIRD_UNITY_PLAYER_SERVICE_V1_ID).get();
        c->metadata = host.Query<CabbirdUnityMetadataServiceV1>(CABBIRD_UNITY_METADATA_SERVICE_V1_ID).get();
        c->il2cpp = host.Query<CabbirdIl2CppServiceV1>(CABBIRD_IL2CPP_SERVICE_V1_ID).get();
        c->windows = host.Query<CabbirdWindowServiceV1>(CABBIRD_WINDOW_SERVICE_V1_ID).get();
        const auto scheduler = host.Query<CabbirdSchedulerServiceV1>(CABBIRD_SCHEDULER_SERVICE_V1_ID);
        const auto ui = host.Query<CabbirdUiServiceV1>(CABBIRD_UI_SERVICE_V1_ID);
        if (!c->core || c->core->struct_size < offsetof(CabbirdCoreServiceV1,module_base)+sizeof(c->core->module_base) ||
            !c->core->read_memory || !c->core->module_base || !c->core->log ||
            !c->hooks || c->hooks->struct_size < sizeof(*c->hooks) || !c->hooks->create || !c->hooks->release || !c->hooks->begin_callback || !c->hooks->end_callback ||
            !c->config || c->config->struct_size < sizeof(*c->config) || !c->config->register_schema || !c->config->read || !c->config->write_atomic ||
            !c->json || c->json->struct_size < sizeof(*c->json) || !c->json->parse || !c->json->release || !c->json->object_find || !c->json->boolean_value || !c->json->number_value ||
            !c->player || c->player->struct_size < offsetof(CabbirdUnityPlayerServiceV1,snapshot)+sizeof(c->player->snapshot) || !c->player->snapshot ||
            !c->metadata || c->metadata->struct_size < sizeof(*c->metadata) || !c->metadata->resolve_method || !c->metadata->resolve_instance_field ||
            !c->il2cpp || c->il2cpp->struct_size < sizeof(*c->il2cpp) || !c->il2cpp->with_runtime ||
            !c->windows || c->windows->struct_size < sizeof(*c->windows) || !c->windows->register_window || !c->windows->release_window || !c->windows->begin || !c->windows->end ||
            !scheduler || scheduler->struct_size < sizeof(*scheduler.get()) || !scheduler->schedule ||
            !ui || ui->struct_size < offsetof(CabbirdUiServiceV1,slider_float)+sizeof(ui->slider_float) || !ui->checkbox || !ui->slider_float) {
            return Unavailable(*c,"load.services","Required host services are unavailable.");
        }
        auto loaded = cabbird::sdk::LoadProfile(host,"compatibility.profile.json",
            {"ap","GameAssembly.dll","il2cpp","cabbird.move-speed.v5"});
        if (!loaded) return Unavailable(*c,"load.profile","The movement compatibility profile is unavailable.");
        c->profile = std::move(loaded).value();
        try {
            constexpr std::uint32_t maximum = 64U*1024U;
            c->state_entity = c->profile.Value("stateEntity",maximum,alignof(std::uintptr_t));
            c->steering_entity = c->profile.Value("steeringEntity",maximum,alignof(std::uintptr_t));
            c->handle_entity = c->profile.Value("handleEntity",maximum,alignof(std::uintptr_t));
            c->entity_data = c->profile.Value("entityData",maximum,alignof(std::uintptr_t));
            c->state_data = c->profile.Value("stateData",maximum,alignof(std::uintptr_t));
            c->state_type = c->profile.Value("stateType",maximum,alignof(std::uint32_t));
            if (c->state_entity < sizeof(std::uintptr_t)*2 || c->steering_entity < sizeof(std::uintptr_t)*2 ||
                c->handle_entity < sizeof(std::uintptr_t)*2 || c->entity_data < sizeof(std::uintptr_t)*2)
                return Unavailable(*c,"load.layout","The movement profile has an unsupported ownership layout.");
        } catch (...) { return Unavailable(*c,"load.layout","The movement profile has an unsupported ownership layout."); }
        CabbirdGenerationHandleV1 schema{};
        if (!Succeeded(c->config->register_schema(c->config->user,StringView(kSchemaId),kSettingsVersion,
            {reinterpret_cast<const std::uint8_t*>(kSchema.data()),kSchema.size()},&schema)))
            return Unavailable(*c,"load.schema","Movement settings could not be registered.");
        c->save_queue.Bind(api,c->config,kSchemaId,kSettingsVersion);
        c->settings_warning.store(!LoadSettings(*c));
        if(c->settings_warning.load())c->core->log(c->core->user,CABBIRD_CORE_LOG_LEVEL_V1_WARNING,
            StringView(c->localizer.Text("log.settings-invalid","Move Speed: saved settings are invalid; defaults retained.")));
        *output = c.release();
        return Ok();
    } catch (...) { return {CABBIRD_STATUS_V1_FAILED,0,{}}; }
}
CabbirdStatusV1 CABBIRD_CALL Start(void* user) noexcept {
    auto& c = *static_cast<Context*>(user);
    try {
        CabbirdWindowSpecV1 spec{};
        spec.struct_size=sizeof(spec); spec.id=StringView("move-speed-main");
        const auto title=c.localizer.Text("window.title","Move Speed"); spec.title=StringView(title);
        spec.initial_width=600; spec.initial_height=440; spec.minimum_width=360; spec.minimum_height=260; spec.default_open=1;
        const auto status=c.windows->register_window(c.windows->user,&spec,&c.window);
        if (!Succeeded(status)) return status;
        g_active.store(&c,std::memory_order_release);
        return Ok();
    } catch (...) { return {CABBIRD_STATUS_V1_FAILED,0,{}}; }
}
CabbirdStatusV1 CABBIRD_CALL Stop(void* user,std::uint32_t) noexcept {
    auto& c=*static_cast<Context*>(user);
    try {
        // PreStop has disabled/drained hooks and Worker callbacks. No native
        // cached movement attribute was changed. World target speed is rewritten
        // by its next Decorate; the game retains its normal acceleration.
        g_active.store(nullptr,std::memory_order_release);
        c.locomotion={};
        if (c.save_failed.load()) SettingsChanged(c);
        if (c.save_failed.load() || !c.save_queue.Flush()) return {CABBIRD_STATUS_V1_FAILED,0,{}};
        const auto released=c.windows->release_window(c.windows->user,c.window);
        if (!Succeeded(released) && released.code!=CABBIRD_STATUS_V1_NOT_FOUND) return released;
        c.window={};
        return Ok();
    } catch (...) { return {CABBIRD_STATUS_V1_FAILED,0,{}}; }
}
void CABBIRD_CALL Unload(void* user) noexcept {
    try { auto* c=static_cast<Context*>(user); c->save_queue.Reset(); delete c; } catch (...) {}
}
void CABBIRD_CALL Update(void* user,double delta) noexcept {
    auto& c=*static_cast<Context*>(user);
    try {
        c.game_thread.store(GetCurrentThreadId(),std::memory_order_release);
        // Always renew player demand. An early NOT_FOUND is normal before the
        // world's next sample and must not prevent this generation from starting.
        CabbirdUnityPlayerSnapshotV1 player{};player.struct_size=sizeof(player);
        if(!Succeeded(c.player->snapshot(c.player->user,&player)) || !(player.flags&CABBIRD_UNITY_PLAYER_V1_VALID)) {
            c.bind_remaining=0;return;
        }
        if(c.bind_remaining>0)c.bind_remaining-=delta;
        if(NeedsAutomaticBinding(c) && c.bind_remaining<=0) {
            c.bind_remaining=1.0;BindMovement(c);ReportBinding(c);
        }
    } catch (...) {}
}
void DrawContents(Context& c,const CabbirdUiServiceV1* ui) {
    int enabled=c.requested_enabled.load()?1:0;
    if(ui->checkbox(ui->user,StringView(c.localizer.Text("enabled","Enabled")),&enabled)) {c.requested_enabled.store(enabled!=0);SettingsChanged(c);}
    int only=c.player_only.load()?1:0;
    if(ui->checkbox(ui->user,StringView(c.localizer.Text("player-only","Only the controlled entity")),&only)) {c.player_only.store(only!=0);SettingsChanged(c);}
    float factor=c.factor.load();
    if(ui->slider_float(ui->user,StringView(c.localizer.Text("factor","Movement multiplier")),&factor,kMinimumFactor,kMaximumFactor) &&
        std::isfinite(factor) && factor>=kMinimumFactor && factor<=kMaximumFactor) {c.factor.store(factor);SettingsChanged(c);}
}
void CABBIRD_CALL Draw(void* user,const CabbirdUiServiceV1* ui) noexcept {
    auto& c=*static_cast<Context*>(user);
    try {
        int32_t visible{};
        const auto status=c.windows->begin(c.windows->user,c.window,0,&visible);
        if(!Succeeded(status)) return;
        struct End final {Context& c;~End(){static_cast<void>(c.windows->end(c.windows->user,c.window));}} end{c};
        if(visible && ui) DrawContents(c,ui);
    } catch (...) {}
}
} // namespace
CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(CabbirdPluginDescriptorV1* d) {
    if(!d || d->struct_size<sizeof(*d) || d->api_major!=CABBIRD_PLUGIN_API_V1_MAJOR) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT,0,{}};
    d->id=StringView("cabbird.move-speed");d->name=StringView("Move Speed");d->author=StringView("Cabbird");d->version=StringView("1.0.0");
    d->on_load=Load;d->on_start=Start;d->on_stop=Stop;d->on_unload=Unload;d->on_update=Update;d->on_draw=Draw;
    return Ok();
}

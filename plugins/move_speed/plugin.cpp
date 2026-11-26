/* Move Speed -- rewritten for AzurPromilia 1.0.1.1.
 *
 * WHAT WAS WRONG WITH THE BINARY THAT SHIPPED
 *
 * The previous build hooked five addresses, four of them capture paths in
 * `PlayerEntityManageSystem`.  Two of those (`OnUpdateDeltaTime`, `OnManualUpdate`) have a
 * `(System.Single)` signature on this build -- there is no `EntityHandle` argument to read --
 * so the detour's capture helper was handed something that was not an `EntityHandle`, walked
 * `[handle+0x10] -> [entity+0x18]` on it, and faulted at `plugin.dll+0x1485` with
 * `0xc0000005`.  Its own fingerprint guard would have refused the mismatched targets, except
 * the fingerprint bytes had been replaced with "whatever is at the new address", which turns
 * the guard into a tautology.
 *
 * SO THIS FILE TAKES A DIFFERENT SHAPE
 *
 *   1. ONE capture hook, on `PlayerEntityManageSystem.UpdateControllingGameObject(EntityHandle)`
 *      (0x1C33340) -- a method whose argument really is an `EntityHandle` on this build.  Its
 *      whole job is to remember which entity id the game is currently controlling.  It resolves
 *      that through the host's `read_memory`, one hop at a time, and abandons the sample if any
 *      hop fails.  A bad pointer can therefore cost a sample and nothing else: this file never
 *      dereferences a game pointer directly on the capture path.
 *   2. ONE scaling hook, on `AliveProperty.get_SpeedReviseRatio` (0x18052A0) -- the per-frame
 *      multiplier the movement code reads.  `BaseProperty.entityId` is at 0x10, so deciding
 *      "is this the player's property" is one aligned int32 read of an object the game itself
 *      just passed in.  The value is scaled on the way out: nothing in game state is written.
 *
 * The scaling path runs about 1400 times a second, so it takes no callback lease and no host
 * call: a lease per call would cost more than the feature saves.  It reads two atomics, one
 * int32 and one float.  Removal clears the trampoline before releasing the hook (the host's
 * documented order), so a call already in flight sees "not callable" instead of a freed body.
 *
 * Both targets are prologue-checked before the hook is created, against bytes recorded from
 * THIS build.  A future update therefore makes this plugin refuse to hook and say so, which is
 * the behaviour the previous build lost.
 */

#include "cabbird/sdk/cpp.hpp"
#include "cabbird/sdk/services/core.h"
#include "cabbird/sdk/services/interop.h"
#include "cabbird/sdk/services/localization.h"
#include "cabbird/sdk/services/platform.h"
#include "cabbird/sdk/services/ui.h"
#include "cabbird/sdk/services/unity.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <windows.h>

using cabbird::sdk::Host;
using cabbird::sdk::Ok;
using cabbird::sdk::StringView;

namespace {

// ---------------------------------------------------------------------------------------------
// THIS BUILD'S ADDRESSES AND LAYOUT
// ---------------------------------------------------------------------------------------------
constexpr std::uintptr_t kCaptureRva = 0x1C33340;  // PlayerEntityManageSystem.UpdateControllingGameObject(EntityHandle)
constexpr std::uintptr_t kSpeedRva = 0x18052A0;    // AliveProperty.get_SpeedReviseRatio()

// `Lens.Gameplay.Modules.BigWorld` layout, live dump of 1.0.1.1:
//   EntityHandle.<entity>k__BackingField   0x10
//   Entity.data                            0x18
//   BaseData.<entityId>k__BackingField     0x130
//   BaseProperty.entityId                  0x10   (AliveProperty inherits it)
constexpr std::size_t kOffHandleEntity = 0x10;
constexpr std::size_t kOffEntityData = 0x18;
constexpr std::size_t kOffDataEntityId = 0x130;
constexpr std::size_t kOffPropertyEntityId = 0x10;

// First four bytes of each body on this build.  Recorded from GameAssembly.dll, not invented:
//   0x1C33340  48 89 5C 24 10 57 48 83 EC 40 80 3D ...   mov [rsp+0x10],rbx; push rdi; sub rsp,0x40
//   0x18052A0  40 53 48 83 EC 30 80 3D ...               push rbx; sub rsp,0x30
constexpr unsigned char kCapturePrologue[4] = {0x48, 0x89, 0x5C, 0x24};
constexpr unsigned char kSpeedPrologue[4] = {0x40, 0x53, 0x48, 0x83};

constexpr float kMinimumFactor = 0.1f;
constexpr float kMaximumFactor = 20.0f;
// 2.0 rather than 1.0: a plugin that is installed but multiplies by one moves nothing, which
// looks exactly like a broken plugin.  The first thing a user does after enabling it is move; it
// should already be faster when they do.
constexpr float kDefaultFactor = 2.0f;

const char* const kStorageKey = "move-speed.txt";
const char* const kStorageFactorField = "factor=";
constexpr std::size_t kMaximumStoredBytes = 256;

const CabbirdCoreServiceV1* g_core = nullptr;
const CabbirdUiServiceV1* g_ui = nullptr;
const CabbirdHookServiceV1* g_hook = nullptr;
const CabbirdLocalizationServiceV1* g_localization = nullptr;
const CabbirdStorageServiceV1* g_storage = nullptr;
// THE PLAYER ID'S REAL SOURCE.
//
// The capture hook below only fires when the game SWITCHES the controlled entity, so a hook
// created after the player already entered the world never learns the id -- which is exactly
// what happened: hooks=2, 1400 getter calls a second, and not one entity scaled because
// `player entity` stayed 0.  `cabbird.unity.player` publishes the same identity the host
// resolved for the player (`BaseData.entityId`, 90011 in the observed session), so it is read
// once per frame here and the detour only compares two integers.
const CabbirdUnityPlayerServiceV1* g_player = nullptr;

std::atomic<std::uintptr_t> g_capture_trampoline{0};
std::atomic<std::uintptr_t> g_speed_trampoline{0};

CabbirdGenerationHandleV1 g_capture_handle{};
CabbirdGenerationHandleV1 g_speed_handle{};
std::atomic<bool> g_installed{false};

std::atomic<int> g_player_entity_id{0};
std::atomic<float> g_factor{kDefaultFactor};
std::atomic<float> g_stored_factor{kDefaultFactor};
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_player_only{true};
std::atomic<std::uint64_t> g_speed_calls{0};
std::atomic<std::uint64_t> g_scaled_calls{0};
std::atomic<std::uint64_t> g_capture_calls{0};

enum StatusId : int {
    kStatusNone = 0,
    kStatusInstalled,
    kStatusRemoved,
    kStatusNoHookService,
    kStatusNoModule,
    kStatusPrologueMismatch,
    kStatusCreateRefused,
    kStatusCount,
};

struct StatusText final {
    const char* key;
    const char* english;
};

const StatusText kStatusTexts[kStatusCount] = {
    {"cabbird.move-speed.status.none", ""},
    {"cabbird.move-speed.status.live", "speed hook is live"},
    {"cabbird.move-speed.status.disabled", "speed hook removed"},
    {"cabbird.move-speed.status.no-hook", "this host publishes no hook service"},
    {"cabbird.move-speed.status.no-module", "GameAssembly.dll is not loaded"},
    {"cabbird.move-speed.status.prologue",
     "the target's body does not match this build; not hooking it"},
    {"cabbird.move-speed.status.refused", "the host refused to create the hook"},
};

std::atomic<int> g_status_id{kStatusNone};
std::atomic<int> g_status_code{0};

void SetStatus(const int id, const int code = 0) noexcept {
    g_status_code.store(code, std::memory_order_relaxed);
    g_status_id.store(id, std::memory_order_release);
}

constexpr std::size_t kTranslationBytes = 512;

const char* Tr(const char* const key, const char* const english,
               const char* const argument0 = nullptr, const char* const argument1 = nullptr,
               const char* const argument2 = nullptr) noexcept {
    if (english == nullptr) return "";
    const CabbirdLocalizationServiceV1* const service = g_localization;
    if (service == nullptr || service->translate == nullptr || key == nullptr) return english;

    const CabbirdStringViewV1 arguments[3] = {
        {argument0, argument0 == nullptr ? 0U : std::strlen(argument0)},
        {argument1, argument1 == nullptr ? 0U : std::strlen(argument1)},
        {argument2, argument2 == nullptr ? 0U : std::strlen(argument2)}};
    std::size_t argument_count = 0;
    if (argument2 != nullptr) {
        argument_count = 3;
    } else if (argument1 != nullptr) {
        argument_count = 2;
    } else if (argument0 != nullptr) {
        argument_count = 1;
    }

    thread_local char buffer[kTranslationBytes];
    std::size_t size = sizeof(buffer);
    const CabbirdStatusV1 status = service->translate(
        service->user, StringView(key), StringView(english), arguments, argument_count, buffer,
        &size);
    if (status.code != CABBIRD_STATUS_V1_OK || size == 0 || buffer[0] == '\0') return english;
    return buffer;
}

bool HookUsable() noexcept {
    return g_hook != nullptr && g_hook->create != nullptr && g_hook->release != nullptr &&
           g_hook->begin_callback != nullptr && g_hook->end_callback != nullptr;
}

// A read through the host's own memory service.  Never a raw dereference: that is what the
// previous build did with a pointer it had not verified, and it is why the game went down.
bool ReadBytes(const std::uintptr_t address, void* const destination, const std::size_t size) noexcept {
    if (address == 0 || destination == nullptr) return false;
    const CabbirdCoreServiceV1* const core = g_core;
    if (core == nullptr || core->read_memory == nullptr) return false;
    const CabbirdStatusV1 status = core->read_memory(
        core->user, address, {static_cast<std::uint8_t*>(destination), size});
    return status.code == CABBIRD_STATUS_V1_OK;
}

// The scaling path's one direct read, SEH-contained.  The object is one the game passed in as
// `this`, so this is not a guess about a pointer -- but the function is POD-only so MSVC will
// accept `__try` here, and a fault returns 0 (meaning "not the player") rather than escaping
// into the game's frame.
__declspec(noinline) std::int32_t ReadPropertyEntityId(const std::uintptr_t self) noexcept {
    __try {
        return *reinterpret_cast<const std::int32_t*>(self + kOffPropertyEntityId);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        return 0;
    }
}

class CallbackLease {
public:
    CallbackLease() noexcept {
        if (!HookUsable()) return;
        CabbirdGenerationHandleV1 lease{};
        if (g_hook->begin_callback(g_hook->user, g_capture_handle, &lease).code ==
            CABBIRD_STATUS_V1_OK) {
            lease_ = lease;
            held_ = true;
        }
    }
    ~CallbackLease() noexcept {
        if (held_ && g_hook != nullptr && g_hook->end_callback != nullptr) {
            static_cast<void>(g_hook->end_callback(g_hook->user, lease_));
        }
    }
    CallbackLease(const CallbackLease&) = delete;
    CallbackLease& operator=(const CallbackLease&) = delete;

private:
    CabbirdGenerationHandleV1 lease_{};
    bool held_{false};
};

// ---------------------------------------------------------------------------------------------
// CAPTURE: which entity id is the game driving right now
// ---------------------------------------------------------------------------------------------
void RememberPlayer(const std::uintptr_t handle) noexcept {
    std::uintptr_t entity = 0;
    if (!ReadBytes(handle + kOffHandleEntity, &entity, sizeof(entity)) || entity == 0) return;
    std::uintptr_t data = 0;
    if (!ReadBytes(entity + kOffEntityData, &data, sizeof(data)) || data == 0) return;
    std::int32_t id = 0;
    if (!ReadBytes(data + kOffDataEntityId, &id, sizeof(id)) || id == 0) return;
    g_player_entity_id.store(id, std::memory_order_release);
}

// Ask the host which entity it resolved as the controlled player.  Runs from `Update` (the game
// domain, once per frame), never from the scaling detour: a host call 1400 times a second would
// cost more than the feature is worth.
void RefreshPlayerFromHost() noexcept {
    const CabbirdUnityPlayerServiceV1* const service = g_player;
    if (service == nullptr || service->snapshot == nullptr) return;
    CabbirdUnityPlayerSnapshotV1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    if (service->snapshot(service->user, &snapshot).code != CABBIRD_STATUS_V1_OK) return;
    if ((snapshot.flags & CABBIRD_UNITY_PLAYER_V1_VALID) == 0) return;
    if (snapshot.entity_id == 0) return;
    g_player_entity_id.store(static_cast<std::int32_t>(snapshot.entity_id),
                             std::memory_order_release);
}

using CaptureFn = void(CABBIRD_CALL*)(std::uintptr_t self, std::uintptr_t handle,
                                     std::uintptr_t method_info);

void CABBIRD_CALL CaptureDetour(const std::uintptr_t self, const std::uintptr_t handle,
                                const std::uintptr_t method_info) {
    const CallbackLease lease;
    CaptureFn trampoline = nullptr;
    for (int spin = 0; spin < 200000; ++spin) {
        trampoline = reinterpret_cast<CaptureFn>(g_capture_trampoline.load(std::memory_order_acquire));
        if (trampoline != nullptr) break;
        YieldProcessor();
    }
    if (trampoline == nullptr) return;

    g_capture_calls.fetch_add(1, std::memory_order_relaxed);
    RememberPlayer(handle);
    trampoline(self, handle, method_info);
}

// ---------------------------------------------------------------------------------------------
// SCALING: the per-frame multiplier
// ---------------------------------------------------------------------------------------------
using SpeedFn = float(CABBIRD_CALL*)(std::uintptr_t self, std::uintptr_t unused,
                                     std::uintptr_t method_info);

float CABBIRD_CALL SpeedDetour(const std::uintptr_t self, const std::uintptr_t unused,
                               const std::uintptr_t method_info) {
    const auto trampoline =
        reinterpret_cast<SpeedFn>(g_speed_trampoline.load(std::memory_order_acquire));
    if (trampoline == nullptr) return 0.0f;

    const float base = trampoline(self, unused, method_info);
    g_speed_calls.fetch_add(1, std::memory_order_relaxed);

    if (!g_enabled.load(std::memory_order_relaxed)) return base;

    const float factor = g_factor.load(std::memory_order_relaxed);
    if (factor == 1.0f) return base;

    const std::int32_t player = g_player_entity_id.load(std::memory_order_acquire);
    if (g_player_only.load(std::memory_order_relaxed)) {
        // Not captured yet: the world is still loading, or the player entity was rebuilt.
        // Scaling anyway would move every monster at the player's multiplier.
        if (player == 0) return base;
        const std::int32_t self_id = ReadPropertyEntityId(self);
        if (self_id == 0 || self_id != player) return base;
    }

    g_scaled_calls.fetch_add(1, std::memory_order_relaxed);
    return base * factor;
}

// ---------------------------------------------------------------------------------------------
// INSTALL / REMOVE
// ---------------------------------------------------------------------------------------------
bool PrologueMatches(const std::uintptr_t address, const unsigned char* const expected) noexcept {
    unsigned char found[4]{};
    if (!ReadBytes(address, found, sizeof(found))) return false;
    return std::memcmp(found, expected, sizeof(found)) == 0;
}

bool InstallHook() {
    if (!HookUsable()) {
        SetStatus(kStatusNoHookService);
        return false;
    }
    if (g_installed.load(std::memory_order_relaxed)) return true;
    if (g_core == nullptr || g_core->module_base == nullptr) {
        SetStatus(kStatusNoModule);
        return false;
    }

    const std::uintptr_t base = g_core->module_base(g_core->user, StringView("GameAssembly.dll"));
    if (base == 0) {
        SetStatus(kStatusNoModule);
        return false;
    }

    const std::uintptr_t capture_target = base + kCaptureRva;
    const std::uintptr_t speed_target = base + kSpeedRva;
    if (!PrologueMatches(capture_target, kCapturePrologue) ||
        !PrologueMatches(speed_target, kSpeedPrologue)) {
        SetStatus(kStatusPrologueMismatch);
        return false;
    }

    CabbirdHookRequestV1 capture_request{};
    capture_request.struct_size = sizeof(capture_request);
    capture_request.kind = CABBIRD_HOOK_V1_FUNCTION;
    capture_request.target = capture_target;
    capture_request.detour = reinterpret_cast<void*>(&CaptureDetour);
    capture_request.label = StringView("cabbird.move-speed.capture");

    std::uintptr_t capture_original = 0;
    CabbirdGenerationHandleV1 capture_handle{};
    if (g_hook->create(g_hook->user, &capture_request, &capture_original, &capture_handle).code !=
            CABBIRD_STATUS_V1_OK ||
        capture_original == 0) {
        SetStatus(kStatusCreateRefused);
        return false;
    }
    g_capture_handle = capture_handle;
    g_capture_trampoline.store(capture_original, std::memory_order_release);

    CabbirdHookRequestV1 speed_request{};
    speed_request.struct_size = sizeof(speed_request);
    speed_request.kind = CABBIRD_HOOK_V1_FUNCTION;
    speed_request.target = speed_target;
    speed_request.detour = reinterpret_cast<void*>(&SpeedDetour);
    speed_request.label = StringView("cabbird.move-speed.scale");

    std::uintptr_t speed_original = 0;
    CabbirdGenerationHandleV1 speed_handle{};
    if (g_hook->create(g_hook->user, &speed_request, &speed_original, &speed_handle).code !=
            CABBIRD_STATUS_V1_OK ||
        speed_original == 0) {
        g_capture_trampoline.store(0, std::memory_order_release);
        static_cast<void>(g_hook->release(g_hook->user, g_capture_handle));
        g_capture_handle = CabbirdGenerationHandleV1{};
        SetStatus(kStatusCreateRefused);
        return false;
    }
    g_speed_handle = speed_handle;
    g_speed_trampoline.store(speed_original, std::memory_order_release);

    // The scaling switch follows the HOOK, not a second checkbox.  It used to be a separate
    // atomic that nothing ever set, so the detour answered `base` forever: hooks installed,
    // getter called a thousand times a second, zero entities scaled.
    g_enabled.store(true, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    SetStatus(kStatusInstalled);
    return true;
}

void RemoveHook() {
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;
    g_enabled.store(false, std::memory_order_release);
    // Trampolines first, so a call already inside a detour finds nothing to invoke; only then
    // are the hooks released.
    g_speed_trampoline.store(0, std::memory_order_release);
    g_capture_trampoline.store(0, std::memory_order_release);
    if (g_hook != nullptr && g_hook->release != nullptr) {
        static_cast<void>(g_hook->release(g_hook->user, g_speed_handle));
        static_cast<void>(g_hook->release(g_hook->user, g_capture_handle));
    }
    g_speed_handle = CabbirdGenerationHandleV1{};
    g_capture_handle = CabbirdGenerationHandleV1{};
    SetStatus(kStatusRemoved);
}

// ---------------------------------------------------------------------------------------------
// STORAGE
// ---------------------------------------------------------------------------------------------
bool ReadStoredFactor(float* const factor) noexcept {
    if (factor == nullptr) return false;
    const CabbirdStorageServiceV1* const storage = g_storage;
    if (storage == nullptr || storage->read == nullptr) return false;

    std::size_t size = 0;
    CabbirdStatusV1 status = storage->read(storage->user, StringView(kStorageKey), {nullptr, 0}, &size);
    if (status.code != CABBIRD_STATUS_V1_OK || size == 0 || size > kMaximumStoredBytes) return false;

    char document[kMaximumStoredBytes + 1]{};
    std::size_t capacity = kMaximumStoredBytes;
    status = storage->read(storage->user, StringView(kStorageKey),
                           {reinterpret_cast<std::uint8_t*>(document), capacity}, &capacity);
    if (status.code != CABBIRD_STATUS_V1_OK || capacity > kMaximumStoredBytes) return false;
    document[capacity] = '\0';

    const char* field = std::strstr(document, kStorageFactorField);
    if (field == nullptr) return false;
    field += std::strlen(kStorageFactorField);
    char* end = nullptr;
    const double parsed = std::strtod(field, &end);
    if (end == field) return false;

    float value = static_cast<float>(parsed);
    if (value < kMinimumFactor) value = kMinimumFactor;
    if (value > kMaximumFactor) value = kMaximumFactor;
    *factor = value;
    return true;
}

bool WriteStoredFactor(const float factor) noexcept {
    const CabbirdStorageServiceV1* const storage = g_storage;
    if (storage == nullptr || storage->write_atomic == nullptr) return false;

    char document[64]{};
    const int length = std::snprintf(document, sizeof(document), "%s%.3f\n", kStorageFactorField,
                                     static_cast<double>(factor));
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(document)) return false;
    const CabbirdStatusV1 status = storage->write_atomic(
        storage->user, StringView(kStorageKey),
        {reinterpret_cast<const std::uint8_t*>(document), static_cast<std::size_t>(length)});
    return status.code == CABBIRD_STATUS_V1_OK;
}

void FlushStoredFactor() noexcept {
    const float current = g_factor.load(std::memory_order_relaxed);
    if (current == g_stored_factor.load(std::memory_order_relaxed)) return;
    if (WriteStoredFactor(current)) g_stored_factor.store(current, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// LIFECYCLE
// ---------------------------------------------------------------------------------------------
std::atomic<int> g_pending{0};
int g_open = 1;

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *context = nullptr;
    const Host services(host);

    g_core = services.Query<CabbirdCoreServiceV1>(CABBIRD_CORE_SERVICE_V1_ID,
                                                  CABBIRD_CORE_SERVICE_V1_VERSION).get();
    if (g_core == nullptr || g_core->read_memory == nullptr || g_core->module_base == nullptr) {
        g_core = nullptr;
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_ui = services.Query<CabbirdUiServiceV1>(CABBIRD_UI_SERVICE_V1_ID,
                                              CABBIRD_UI_SERVICE_V1_VERSION).get();
    if (g_ui == nullptr || g_ui->text == nullptr || g_ui->begin_window == nullptr ||
        g_ui->end_window == nullptr) {
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_hook = services.Query<CabbirdHookServiceV1>(CABBIRD_HOOK_SERVICE_V1_ID,
                                                  CABBIRD_HOOK_SERVICE_V1_VERSION).get();
    g_localization = services.Query<CabbirdLocalizationServiceV1>(
        CABBIRD_LOCALIZATION_SERVICE_V1_ID, CABBIRD_LOCALIZATION_SERVICE_V1_VERSION).get();
    if (g_localization != nullptr &&
        (g_localization->translate == nullptr || g_localization->locale == nullptr)) {
        g_localization = nullptr;
    }
    g_storage = services.Query<CabbirdStorageServiceV1>(CABBIRD_STORAGE_SERVICE_V1_ID,
                                                        CABBIRD_STORAGE_SERVICE_V1_VERSION).get();
    if (g_storage != nullptr &&
        (g_storage->read == nullptr || g_storage->write_atomic == nullptr)) {
        g_storage = nullptr;
    }
    g_player = services.Query<CabbirdUnityPlayerServiceV1>(CABBIRD_UNITY_PLAYER_SERVICE_V1_ID,
                                                          CABBIRD_UNITY_PLAYER_SERVICE_V1_VERSION).get();
    if (g_player != nullptr && g_player->snapshot == nullptr) g_player = nullptr;

    float stored = kDefaultFactor;
    if (ReadStoredFactor(&stored)) {
        g_factor.store(stored, std::memory_order_relaxed);
        g_stored_factor.store(stored, std::memory_order_relaxed);
    }

    if (!HookUsable()) {
        SetStatus(kStatusNoHookService);
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    return Ok();
}

CabbirdStatusV1 CABBIRD_CALL Start(void*) {
    // INSTALL WITHOUT WAITING FOR A CLICK.
    //
    // The window's checkbox used to be the only thing that queued an install, and its initial
    // value was "whatever is installed right now" -- which on a fresh load is 0.  So enabling the
    // plugin in the host's plugin list put a window on screen, showed hooks=0, and did nothing
    // until the user found and ticked a second checkbox inside it.  The host already decided this
    // plugin should run; that decision is what queues the install.
    g_pending.store(1, std::memory_order_release);
    return Ok();
}

CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) {
    FlushStoredFactor();
    return Ok();
}

void CABBIRD_CALL Unload(void*) {
    g_ui = nullptr;
    g_core = nullptr;
    g_hook = nullptr;
    g_localization = nullptr;
    g_storage = nullptr;
    g_player = nullptr;
}

void CABBIRD_CALL Update(void*, double) {
    // The player id is refreshed BEFORE the install is serviced, so a hook created this frame
    // already knows who the player is instead of waiting for a character switch that may never
    // come.
    RefreshPlayerFromHost();
    const int pending = g_pending.exchange(0, std::memory_order_acq_rel);
    if (pending == 1) {
        InstallHook();
    } else if (pending == 2) {
        RemoveHook();
    }
}

void CABBIRD_CALL Draw(void*, const CabbirdUiServiceV1* ui) {
    if (ui == nullptr) return;

    const int visible = ui->begin_window(ui->user, StringView("Move Speed"), &g_open, 0);
    if (visible == 0) {
        ui->end_window(ui->user);
        return;
    }

    const bool installed = g_installed.load(std::memory_order_relaxed);
    const bool busy = g_pending.load(std::memory_order_relaxed) != 0;

    static int enabled_ui = 0;
    if (!busy) enabled_ui = installed ? 1 : 0;
    if (ui->checkbox != nullptr) {
        if (ui->checkbox(ui->user, StringView(Tr("cabbird.move-speed.enabled", "enabled")),
                         &enabled_ui) != 0) {
            g_pending.store(enabled_ui != 0 ? 1 : 2, std::memory_order_release);
        }
    }

    static int player_only_ui = 1;
    player_only_ui = g_player_only.load(std::memory_order_relaxed) ? 1 : 0;
    if (ui->checkbox != nullptr) {
        if (ui->checkbox(ui->user,
                         StringView(Tr("cabbird.move-speed.player-only",
                                       "only the entity the game is driving")),
                         &player_only_ui) != 0) {
            g_player_only.store(player_only_ui != 0, std::memory_order_relaxed);
        }
    }

    if (ui->separator != nullptr) ui->separator(ui->user);

    if (ui->slider_float != nullptr) {
        float value = g_factor.load(std::memory_order_relaxed);
        if (ui->slider_float(ui->user, StringView(Tr("cabbird.move-speed.factor", "multiplier")),
                             &value, kMinimumFactor, kMaximumFactor) != 0) {
            if (value < kMinimumFactor) value = kMinimumFactor;
            if (value > kMaximumFactor) value = kMaximumFactor;
            g_factor.store(value, std::memory_order_relaxed);
        }
    }

    if (ui->separator != nullptr) ui->separator(ui->user);

    char player[24]{};
    char calls[24]{};
    char scaled[24]{};
    char captures[24]{};
    std::snprintf(player, sizeof(player), "%d", g_player_entity_id.load(std::memory_order_relaxed));
    std::snprintf(calls, sizeof(calls), "%llu",
                  static_cast<unsigned long long>(g_speed_calls.load(std::memory_order_relaxed)));
    std::snprintf(scaled, sizeof(scaled), "%llu",
                  static_cast<unsigned long long>(g_scaled_calls.load(std::memory_order_relaxed)));
    std::snprintf(captures, sizeof(captures), "%llu",
                  static_cast<unsigned long long>(g_capture_calls.load(std::memory_order_relaxed)));
    ui->text(ui->user, StringView(Tr(
        "cabbird.move-speed.counters",
        "player entity {0}   getter calls {1}   scaled {2}",
        player, calls, scaled)));
    ui->text(ui->user, StringView(Tr("cabbird.move-speed.captures",
                                     "capture calls {0}", captures)));

    if (g_player_only.load(std::memory_order_relaxed) &&
        g_player_entity_id.load(std::memory_order_relaxed) == 0) {
        ui->text(ui->user, StringView(Tr(
            "cabbird.move-speed.waiting",
            "waiting for the game to hand over the controlled entity; nothing is scaled yet")));
    }

    const int status_id = g_status_id.load(std::memory_order_acquire);
    if (status_id > kStatusNone && status_id < kStatusCount) {
        if (ui->separator != nullptr) ui->separator(ui->user);
        char code[16]{};
        std::snprintf(code, sizeof(code), "%d", g_status_code.load(std::memory_order_relaxed));
        ui->text(ui->user, StringView(Tr(kStatusTexts[status_id].key,
                                         kStatusTexts[status_id].english, code)));
    }
    if (busy) {
        ui->text(ui->user, StringView(Tr("cabbird.move-speed.applying",
                                         "applying on the game thread...")));
    }

    ui->end_window(ui->user);
}

}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    descriptor->id = StringView("cabbird.move-speed");
    descriptor->name = StringView("Move Speed");
    descriptor->author = StringView("Cabbird");
    descriptor->version = StringView("2.0.0");
    descriptor->on_load = Load;
    descriptor->on_start = Start;
    descriptor->on_stop = Stop;
    descriptor->on_unload = Unload;
    descriptor->on_update = Update;
    descriptor->on_draw = Draw;
    return Ok();
}

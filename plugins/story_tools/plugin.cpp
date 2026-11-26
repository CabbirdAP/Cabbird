/* Story Tools -- story skipping, speed and automatic dialogue choices for AzurPromilia 1.0.1.1.
 *
 * WHY EVERY FEATURE IS A HOOK RATHER THAN A CALL
 *
 * The game exposes the operations this plugin needs, but almost all of them are instance methods
 * on objects the plugin cannot name a path to: `StoryManager` is a `BasePlay` with no singleton
 * accessor, and `StorySection` / `BranchFragment` are components created per story.  Reaching them
 * through reflection would mean finding a class, a static getter and then invoking across the ABI
 * on every use.
 *
 * Hooking the method the game ALREADY calls is strictly less machinery: the detour receives `this`
 * in RCX, so the instance is captured for free, and the plugin never needs to discover one.  The
 * same detour also gets to decide the answer, which is what three of the five features are:
 *
 *   - `StoryManager.CanSkipStory()` is the gate every skip path asks before allowing a skip.
 *     Answering `true` is "unlock skip" -- no write to game state, nothing to undo.
 *   - `StorySection.get_playSpeed()` is read every frame by the fragment player.  Multiplying the
 *     answer scales the story, which covers "story speed" and "skip animations" (the animation and
 *     the timeline run off the same clock).
 *   - `BranchFragment.TryOpenBranch()` is called when the game is about to open the choice window.
 *     Recording the first option and advancing instead is "auto dialogue choice".
 *
 * `SkipStory()` is the one true call: the captured `StoryManager` plus the instance-method ABI
 * (this in RCX, MethodInfo* in R8).  It is invoked from the plugin's own action, never from a
 * detour, so a fault there cannot land inside the game's call stack.
 *
 * ABI NOTE, and it is the mistake that cost a session earlier: every detour below declares the
 * hidden `MethodInfo*` as its last parameter.  IL2CPP reads it from R8 (R9 for the two-argument
 * case) inside the callee; a detour that omits it lets the compiler clobber that register, and the
 * trampoline then runs the game's body with garbage.  Declaring it makes the compiler place it
 * where the callee expects.
 *
 * Everything here is a behavioural override: no game state is written, so turning a switch off
 * restores the original behaviour on the next call without a restart.
 */

#include "cabbird/sdk/cpp.hpp"
#include "cabbird/sdk/services/core.h"
#include "cabbird/sdk/services/interop.h"
#include "cabbird/sdk/services/localization.h"
#include "cabbird/sdk/services/platform.h"
#include "cabbird/sdk/services/ui.h"

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
// THIS BUILD'S ADDRESSES (ArmPromilia 1.0.1.1, GameAssembly.dll)
// ---------------------------------------------------------------------------------------------
constexpr std::uintptr_t kCanSkipStoryRva = 0x5FB49A0;    // StoryManager.CanSkipStory() -> bool
constexpr std::uintptr_t kSkipStoryRva = 0x5FB9610;       // StoryManager.SkipStory() -> bool
constexpr std::uintptr_t kSectionSpeedRva = 0x5FEF2C0;    // StorySection.get_playSpeed() -> float
constexpr std::uintptr_t kTryOpenBranchRva = 0x5F8B650;   // BranchFragment.TryOpenBranch(bool)
constexpr std::uintptr_t kRecordOptionRva = 0x5F8B1A0;    // BranchFragment.RecordSelectedOption(int,int)
constexpr std::uintptr_t kPlayNextDialogRva = 0x5F8B070;  // BranchFragment.PlayNextDialog()

// First bytes of each body on this build, recorded from GameAssembly.dll so a patch that moves
// them is refused instead of hooked.
constexpr unsigned char kCanSkipPrologue[4] = {0x40, 0x53, 0x48, 0x83};   // push rbx; sub rsp,0x20
constexpr unsigned char kSpeedPrologue[4] = {0x40, 0x53, 0x48, 0x83};     // push rbx; sub rsp,0x20
constexpr unsigned char kBranchPrologue[4] = {0x4C, 0x89, 0x44, 0x24};    // mov [rsp+0x18],r8

constexpr float kMinimumFactor = 1.0f;
constexpr float kMaximumFactor = 20.0f;
constexpr float kDefaultFactor = 3.0f;

const char* const kStorageKey = "story-tools.txt";

const CabbirdCoreServiceV1* g_core = nullptr;
const CabbirdUiServiceV1* g_ui = nullptr;
const CabbirdHookServiceV1* g_hook = nullptr;
const CabbirdLocalizationServiceV1* g_localization = nullptr;
const CabbirdStorageServiceV1* g_storage = nullptr;

std::atomic<std::uintptr_t> g_can_skip_trampoline{0};
std::atomic<std::uintptr_t> g_speed_trampoline{0};
std::atomic<std::uintptr_t> g_branch_trampoline{0};

CabbirdGenerationHandleV1 g_can_skip_handle{};
CabbirdGenerationHandleV1 g_speed_handle{};
CabbirdGenerationHandleV1 g_branch_handle{};
std::atomic<bool> g_installed{false};

// The live StoryManager, captured from the detour that the game itself calls with it in RCX.
std::atomic<std::uintptr_t> g_story_manager{0};
std::atomic<std::uintptr_t> g_story_method{0};

std::atomic<bool> g_unlock_skip{true};
// OFF by default: taking a branch automatically changes which story the player sees, which is a
// different kind of decision from "go faster".  Opt in, and the window says so.
std::atomic<bool> g_auto_choice{false};
std::atomic<bool> g_auto_skip{false};
std::atomic<float> g_speed_factor{kDefaultFactor};

std::atomic<std::uint64_t> g_skip_queries{0};
std::atomic<std::uint64_t> g_speed_reads{0};
std::atomic<std::uint64_t> g_branches{0};
std::atomic<std::uint64_t> g_skips_triggered{0};

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
    {"cabbird.story.none", ""},
    {"cabbird.story.live", "story hooks are live"},
    {"cabbird.story.removed", "story hooks removed"},
    {"cabbird.story.no-hook", "this host publishes no hook service"},
    {"cabbird.story.no-module", "GameAssembly.dll is not loaded"},
    {"cabbird.story.prologue", "a target's body does not match this build; not hooking it"},
    {"cabbird.story.refused", "the host refused to create a hook"},
};

std::atomic<int> g_status_id{kStatusNone};
std::atomic<int> g_status_code{0};
std::atomic<int> g_pending{0};
int g_open = 1;

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
    std::size_t count = 0;
    if (argument2 != nullptr) {
        count = 3;
    } else if (argument1 != nullptr) {
        count = 2;
    } else if (argument0 != nullptr) {
        count = 1;
    }
    thread_local char buffer[kTranslationBytes];
    std::size_t size = sizeof(buffer);
    const CabbirdStatusV1 status = service->translate(
        service->user, StringView(key), StringView(english), arguments, count, buffer, &size);
    if (status.code != CABBIRD_STATUS_V1_OK || size == 0 || buffer[0] == '\0') return english;
    return buffer;
}

bool HookUsable() noexcept {
    return g_hook != nullptr && g_hook->create != nullptr && g_hook->release != nullptr &&
           g_hook->begin_callback != nullptr && g_hook->end_callback != nullptr;
}

bool ReadBytes(const std::uintptr_t address, void* const destination, const std::size_t size) noexcept {
    if (address == 0 || destination == nullptr) return false;
    const CabbirdCoreServiceV1* const core = g_core;
    if (core == nullptr || core->read_memory == nullptr) return false;
    return core->read_memory(core->user, address,
                             {static_cast<std::uint8_t*>(destination), size}).code ==
           CABBIRD_STATUS_V1_OK;
}

// ---------------------------------------------------------------------------------------------
// 1+2. UNLOCK SKIP / STORY SPEED
// ---------------------------------------------------------------------------------------------
using CanSkipFn = bool(CABBIRD_CALL*)(std::uintptr_t self, std::uintptr_t method_info);

bool CABBIRD_CALL CanSkipDetour(const std::uintptr_t self, const std::uintptr_t method_info) {
    // Capture first: this is the only place the plugin is ever handed the live StoryManager.
    if (self != 0) {
        g_story_manager.store(self, std::memory_order_release);
        if (method_info != 0) g_story_method.store(method_info, std::memory_order_relaxed);
    }
    g_skip_queries.fetch_add(1, std::memory_order_relaxed);
    if (g_unlock_skip.load(std::memory_order_relaxed)) return true;

    const auto trampoline =
        reinterpret_cast<CanSkipFn>(g_can_skip_trampoline.load(std::memory_order_acquire));
    if (trampoline == nullptr) return false;
    return trampoline(self, method_info);
}

using SpeedFn = float(CABBIRD_CALL*)(std::uintptr_t self, std::uintptr_t method_info);

float CABBIRD_CALL SpeedDetour(const std::uintptr_t self, const std::uintptr_t method_info) {
    const auto trampoline =
        reinterpret_cast<SpeedFn>(g_speed_trampoline.load(std::memory_order_acquire));
    const float base = trampoline != nullptr ? trampoline(self, method_info) : 1.0f;
    g_speed_reads.fetch_add(1, std::memory_order_relaxed);
    const float factor = g_speed_factor.load(std::memory_order_relaxed);
    if (factor <= 1.0f) return base;
    return base * factor;
}

// ---------------------------------------------------------------------------------------------
// 5. AUTO DIALOGUE CHOICE
// ---------------------------------------------------------------------------------------------
using TryOpenBranchFn = void(CABBIRD_CALL*)(std::uintptr_t self, bool force,
                                            std::uintptr_t method_info);
using RecordOptionFn = void(CABBIRD_CALL*)(std::uintptr_t self, std::int32_t dialog_index,
                                          std::int32_t option_index, std::uintptr_t method_info);
using PlayNextDialogFn = void(CABBIRD_CALL*)(std::uintptr_t self, std::uintptr_t method_info);

std::atomic<std::uintptr_t> g_record_option{0};
std::atomic<std::uintptr_t> g_play_next_dialog{0};

// RE-ENTRY GUARD, and it is not theoretical.
//
// `PlayNextDialog` advances to the next dialog, and the next dialog may itself be a branch --
// which calls `TryOpenBranch` again, which lands back in this detour.  Without the guard that is
// detour -> advance -> detour -> advance on the same stack, burning a core for as long as the
// story keeps producing branches.  The trampoline is called instead of taking another shortcut,
// so the nested call behaves exactly as the game asked.
thread_local bool t_in_branch_detour = false;

void CABBIRD_CALL TryOpenBranchDetour(const std::uintptr_t self, const bool force,
                                      const std::uintptr_t method_info) {
    const auto trampoline =
        reinterpret_cast<TryOpenBranchFn>(g_branch_trampoline.load(std::memory_order_acquire));
    if (!g_auto_choice.load(std::memory_order_relaxed) || trampoline == nullptr ||
        t_in_branch_detour) {
        if (trampoline != nullptr) trampoline(self, force, method_info);
        return;
    }

    // Take the first option without showing the window: record it, then advance.  The window is
    // never opened, so there is nothing to close and no callback left dangling.
    t_in_branch_detour = true;
    g_branches.fetch_add(1, std::memory_order_relaxed);
    const auto record = reinterpret_cast<RecordOptionFn>(g_record_option.load(std::memory_order_acquire));
    const auto next = reinterpret_cast<PlayNextDialogFn>(
        g_play_next_dialog.load(std::memory_order_acquire));
    if (record != nullptr) record(self, 0, 0, method_info);
    if (next != nullptr) next(self, method_info);
    t_in_branch_detour = false;
}

// ---------------------------------------------------------------------------------------------
// SKIP STORY -- the one real call
// ---------------------------------------------------------------------------------------------
using SkipStoryFn = bool(CABBIRD_CALL*)(std::uintptr_t self, std::uintptr_t method_info);

void TriggerSkipStory() {
    const std::uintptr_t manager = g_story_manager.load(std::memory_order_acquire);
    if (manager == 0 || g_core == nullptr || g_core->module_base == nullptr) return;
    const std::uintptr_t base = g_core->module_base(g_core->user, StringView("GameAssembly.dll"));
    if (base == 0) return;
    const auto skip = reinterpret_cast<SkipStoryFn>(base + kSkipStoryRva);
    if (skip != nullptr) {
        static_cast<void>(skip(manager, g_story_method.load(std::memory_order_relaxed)));
        g_skips_triggered.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------------------------
// INSTALL / REMOVE
// ---------------------------------------------------------------------------------------------
bool PrologueMatches(const std::uintptr_t address, const unsigned char* const expected) noexcept {
    unsigned char found[4]{};
    if (!ReadBytes(address, found, sizeof(found))) return false;
    return std::memcmp(found, expected, sizeof(found)) == 0;
}

bool CreateHook(const std::uintptr_t target, void* const detour, const char* const label,
                std::atomic<std::uintptr_t>* const trampoline,
                CabbirdGenerationHandleV1* const handle) {
    CabbirdHookRequestV1 request{};
    request.struct_size = sizeof(request);
    request.kind = CABBIRD_HOOK_V1_FUNCTION;
    request.target = target;
    request.detour = detour;
    request.label = StringView(label);
    std::uintptr_t original = 0;
    CabbirdGenerationHandleV1 created{};
    if (g_hook->create(g_hook->user, &request, &original, &created).code != CABBIRD_STATUS_V1_OK ||
        original == 0) {
        return false;
    }
    *handle = created;
    trampoline->store(original, std::memory_order_release);
    return true;
}

bool InstallHooks() {
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

    const std::uintptr_t can_skip = base + kCanSkipStoryRva;
    const std::uintptr_t speed = base + kSectionSpeedRva;
    const std::uintptr_t branch = base + kTryOpenBranchRva;
    if (!PrologueMatches(can_skip, kCanSkipPrologue) || !PrologueMatches(speed, kSpeedPrologue) ||
        !PrologueMatches(branch, kBranchPrologue)) {
        SetStatus(kStatusPrologueMismatch);
        return false;
    }

    if (!CreateHook(can_skip, reinterpret_cast<void*>(&CanSkipDetour), "cabbird.story.can-skip",
                    &g_can_skip_trampoline, &g_can_skip_handle)) {
        SetStatus(kStatusCreateRefused);
        return false;
    }
    if (!CreateHook(speed, reinterpret_cast<void*>(&SpeedDetour), "cabbird.story.speed",
                    &g_speed_trampoline, &g_speed_handle)) {
        SetStatus(kStatusCreateRefused);
        return false;
    }
    if (!CreateHook(branch, reinterpret_cast<void*>(&TryOpenBranchDetour), "cabbird.story.branch",
                    &g_branch_trampoline, &g_branch_handle)) {
        SetStatus(kStatusCreateRefused);
        return false;
    }

    g_record_option.store(base + kRecordOptionRva, std::memory_order_release);
    g_play_next_dialog.store(base + kPlayNextDialogRva, std::memory_order_release);

    g_installed.store(true, std::memory_order_release);
    SetStatus(kStatusInstalled);
    return true;
}

void RemoveHooks() {
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;
    g_speed_trampoline.store(0, std::memory_order_release);
    g_branch_trampoline.store(0, std::memory_order_release);
    g_can_skip_trampoline.store(0, std::memory_order_release);
    if (g_hook != nullptr && g_hook->release != nullptr) {
        static_cast<void>(g_hook->release(g_hook->user, g_branch_handle));
        static_cast<void>(g_hook->release(g_hook->user, g_speed_handle));
        static_cast<void>(g_hook->release(g_hook->user, g_can_skip_handle));
    }
    g_branch_handle = CabbirdGenerationHandleV1{};
    g_speed_handle = CabbirdGenerationHandleV1{};
    g_can_skip_handle = CabbirdGenerationHandleV1{};
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
    if (storage->read(storage->user, StringView(kStorageKey), {nullptr, 0}, &size).code !=
            CABBIRD_STATUS_V1_OK ||
        size == 0 || size > 128) {
        return false;
    }
    char document[129]{};
    std::size_t capacity = 128;
    if (storage->read(storage->user, StringView(kStorageKey),
                      {reinterpret_cast<std::uint8_t*>(document), capacity}, &capacity).code !=
        CABBIRD_STATUS_V1_OK) {
        return false;
    }
    document[capacity] = '\0';
    const char* field = std::strstr(document, "factor=");
    if (field == nullptr) return false;
    const double parsed = std::strtod(field + 7, nullptr);
    float value = static_cast<float>(parsed);
    if (value < kMinimumFactor) value = kMinimumFactor;
    if (value > kMaximumFactor) value = kMaximumFactor;
    *factor = value;
    return true;
}

void WriteStoredFactor() noexcept {
    const CabbirdStorageServiceV1* const storage = g_storage;
    if (storage == nullptr || storage->write_atomic == nullptr) return;
    char document[48]{};
    const int length = std::snprintf(document, sizeof(document), "factor=%.2f\n",
                                     static_cast<double>(g_speed_factor.load(std::memory_order_relaxed)));
    if (length <= 0) return;
    static_cast<void>(storage->write_atomic(
        storage->user, StringView(kStorageKey),
        {reinterpret_cast<const std::uint8_t*>(document), static_cast<std::size_t>(length)}));
}

// ---------------------------------------------------------------------------------------------
// LIFECYCLE
// ---------------------------------------------------------------------------------------------
CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
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

    float factor = kDefaultFactor;
    if (ReadStoredFactor(&factor)) g_speed_factor.store(factor, std::memory_order_relaxed);

    if (!HookUsable()) {
        SetStatus(kStatusNoHookService);
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    return Ok();
}

CabbirdStatusV1 CABBIRD_CALL Start(void*) {
    g_pending.store(1, std::memory_order_release);
    return Ok();
}

CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) {
    WriteStoredFactor();
    return Ok();
}

void CABBIRD_CALL Unload(void*) {
    g_ui = nullptr;
    g_core = nullptr;
    g_hook = nullptr;
    g_localization = nullptr;
    g_storage = nullptr;
}

void CABBIRD_CALL Update(void*, double) {
    const int pending = g_pending.exchange(0, std::memory_order_acq_rel);
    if (pending == 1) {
        InstallHooks();
    } else if (pending == 2) {
        RemoveHooks();
    }
    // Auto-skip keeps asking the game to skip; SkipStory itself is the game's own decision, so a
    // story that refuses to skip is left alone rather than forced.
    if (g_auto_skip.load(std::memory_order_relaxed)) {
        TriggerSkipStory();
    }
}

void CABBIRD_CALL Draw(void*, const CabbirdUiServiceV1* ui) {
    if (ui == nullptr) return;
    const int visible = ui->begin_window(ui->user, StringView("Story Tools"), &g_open, 0);
    if (visible == 0) {
        ui->end_window(ui->user);
        return;
    }

    const bool installed = g_installed.load(std::memory_order_relaxed);
    const bool busy = g_pending.load(std::memory_order_relaxed) != 0;

    static int hooks_ui = 0;
    if (!busy) hooks_ui = installed ? 1 : 0;
    if (ui->checkbox != nullptr &&
        ui->checkbox(ui->user, StringView(Tr("cabbird.story.hooks", "enable story hooks")),
                     &hooks_ui) != 0) {
        g_pending.store(hooks_ui != 0 ? 1 : 2, std::memory_order_release);
    }

    static int unlock_ui = 1;
    unlock_ui = g_unlock_skip.load(std::memory_order_relaxed) ? 1 : 0;
    if (ui->checkbox != nullptr &&
        ui->checkbox(ui->user, StringView(Tr("cabbird.story.unlock", "unlock skip (never blocked)")),
                     &unlock_ui) != 0) {
        g_unlock_skip.store(unlock_ui != 0, std::memory_order_relaxed);
    }

    static int auto_choice_ui = 0;
    auto_choice_ui = g_auto_choice.load(std::memory_order_relaxed) ? 1 : 0;
    if (ui->checkbox != nullptr &&
        ui->checkbox(ui->user,
                     StringView(Tr("cabbird.story.auto-choice", "auto dialogue choice (first option)")),
                     &auto_choice_ui) != 0) {
        g_auto_choice.store(auto_choice_ui != 0, std::memory_order_relaxed);
    }

    if (ui->separator != nullptr) ui->separator(ui->user);

    if (ui->slider_float != nullptr) {
        float value = g_speed_factor.load(std::memory_order_relaxed);
        if (ui->slider_float(ui->user,
                             StringView(Tr("cabbird.story.speed", "story speed (also skips animation)")),
                             &value, kMinimumFactor, kMaximumFactor) != 0) {
            if (value < kMinimumFactor) value = kMinimumFactor;
            if (value > kMaximumFactor) value = kMaximumFactor;
            g_speed_factor.store(value, std::memory_order_relaxed);
        }
    }

    if (ui->separator != nullptr) ui->separator(ui->user);

    static int auto_skip_ui = 0;
    auto_skip_ui = g_auto_skip.load(std::memory_order_relaxed) ? 1 : 0;
    if (ui->checkbox != nullptr &&
        ui->checkbox(ui->user, StringView(Tr("cabbird.story.auto-skip", "auto skip story")),
                     &auto_skip_ui) != 0) {
        g_auto_skip.store(auto_skip_ui != 0, std::memory_order_relaxed);
    }

    const std::uintptr_t manager = g_story_manager.load(std::memory_order_acquire);
    char manager_text[24]{};
    char skip_text[24]{};
    char speed_text[24]{};
    char branch_text[24]{};
    char factor_text[24]{};
    std::snprintf(manager_text, sizeof(manager_text), "0x%llX",
                  static_cast<unsigned long long>(manager));
    std::snprintf(skip_text, sizeof(skip_text), "%llu",
                  static_cast<unsigned long long>(g_skip_queries.load(std::memory_order_relaxed)));
    std::snprintf(speed_text, sizeof(speed_text), "%llu",
                  static_cast<unsigned long long>(g_speed_reads.load(std::memory_order_relaxed)));
    std::snprintf(branch_text, sizeof(branch_text), "%llu",
                  static_cast<unsigned long long>(g_branches.load(std::memory_order_relaxed)));
    std::snprintf(factor_text, sizeof(factor_text), "%.2f",
                  static_cast<double>(g_speed_factor.load(std::memory_order_relaxed)));

    ui->text(ui->user, StringView(Tr("cabbird.story.counters",
                                     "manager {0}   can-skip {1}   speed reads {2}",
                                     manager_text, skip_text, speed_text)));
    ui->text(ui->user, StringView(Tr("cabbird.story.counters2", "auto-choices {0}   factor {1}",
                                     branch_text, factor_text)));

    if (manager == 0) {
        ui->text(ui->user, StringView(Tr(
            "cabbird.story.waiting",
            "the story manager has not been captured yet; start (or skip) a story once")));
    }

    if (ui->button != nullptr) {
        if (ui->button(ui->user, StringView(Tr("cabbird.story.skip-now", "skip current story")),
                       0.0f, 0.0f) != 0) {
            TriggerSkipStory();
        }
    }

    const int status_id = g_status_id.load(std::memory_order_acquire);
    if (status_id > kStatusNone && status_id < kStatusCount) {
        if (ui->separator != nullptr) ui->separator(ui->user);
        char code[16]{};
        std::snprintf(code, sizeof(code), "%d", g_status_code.load(std::memory_order_relaxed));
        ui->text(ui->user,
                 StringView(Tr(kStatusTexts[status_id].key, kStatusTexts[status_id].english, code)));
    }

    ui->end_window(ui->user);
}

}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    descriptor->id = StringView("cabbird.story-tools");
    descriptor->name = StringView("Story Tools");
    descriptor->author = StringView("Cabbird");
    descriptor->version = StringView("1.0.0");
    descriptor->on_load = Load;
    descriptor->on_start = Start;
    descriptor->on_stop = Stop;
    descriptor->on_unload = Unload;
    descriptor->on_update = Update;
    descriptor->on_draw = Draw;
    return Ok();
}

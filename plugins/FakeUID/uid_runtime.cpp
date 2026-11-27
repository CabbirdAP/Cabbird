#include "uid_runtime.hpp"
#include "cabbird/sdk/il2cpp.hpp"
#include "cabbird/sdk/cpp.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace fake_uid {
namespace {
using namespace cabbird::sdk::il2cpp;
using Clock = std::chrono::steady_clock;
using RefreshFn = void(CABBIRD_CALL*)(Il2CppObject*, const MethodInfo*);

// Authoritative 1.0 dump: ModuleNetworkInfoView.m_txtUid/RefreshUidText,
// AzurWorld.<playerMgr> and PlayerManager.get_myDisplayPlayerId. Offline
// disassembly of RefreshUidText confirms it calls that identity getter before
// the TMP setter. Every binding below is resolved and type-checked at runtime.
struct Binding {
    Il2CppClass *view{}, *tmp{}, *manager{};
    FieldInfo *uid{}, *player_manager{};
    const MethodInfo *find{}, *alive{}, *identity{}, *refresh{};
};
struct Target {
    Il2CppGCHandle view{}, text{};
    const MethodInfo *get{}, *set{};
    std::string original, prefix, suffix, last_written;
};
struct State {
    std::mutex mutex;
    Settings request;
    Status published;
    const CabbirdIl2CppServiceV1* il2cpp{};
    const CabbirdUnityMetadataServiceV1* metadata{};
    const CabbirdHookServiceV1* hooks{};
    Binding binding;
    CabbirdGenerationHandleV1 hook{};
    std::uintptr_t original{}, verified_refresh{}, verified_identity{};
    std::vector<Target> targets;
    Clock::time_point next_resolve{}, next_discovery{};
    std::string identity;
    std::string last_error;
    bool in_tick{};
};
State g_state;

bool ValidDisplay(const char* value, std::size_t capacity) {
    const auto* end = static_cast<const char*>(std::memchr(value, 0, capacity));
    if (!end || end == value) return false;
    wchar_t wide[1025]{};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value,
        static_cast<int>(end - value), wide, static_cast<int>(std::size(wide)));
    if (!size) return false;
    unsigned scalars{};
    for (int i = 0; i < size; ++i) {
        const auto ch = static_cast<unsigned>(wide[i]);
        if (ch < 0x20 || (ch >= 0x7f && ch <= 0x9f) || ch == 0x2028 || ch == 0x2029 || ch == '<' || ch == '>') return false;
        if (ch < 0xdc00 || ch > 0xdfff) ++scalars;
    }
    return scalars <= 256;
}
bool MethodShape(const Context& runtime, const MethodInfo* method, const char* result, bool is_static) {
    if (!method) return false;
    const auto& api = runtime.Functions();
    std::uint32_t flags{};
    return runtime.TypeName(api.il2cpp_method_get_return_type(method)) == result &&
        ((api.il2cpp_method_get_flags(method, &flags) & 0x10u) != 0) == is_static;
}
bool Resolve(const Context& runtime, Binding& b, std::string& reason) {
    const auto& api = runtime.Functions();
    if (!api.il2cpp_field_static_get_value || !api.il2cpp_field_get_flags ||
        !api.il2cpp_method_get_return_type || !api.il2cpp_method_get_flags || !api.il2cpp_object_unbox ||
        !api.il2cpp_gchandle_new || !api.il2cpp_gchandle_get_target || !api.il2cpp_gchandle_free ||
        !api.il2cpp_type_get_object || !api.il2cpp_runtime_invoke || !api.il2cpp_string_new ||
        !api.il2cpp_field_get_value || !api.il2cpp_class_is_assignable_from || !api.il2cpp_array_length) {
        reason = Tr("runtime.api-unavailable", "Required IL2CPP APIs unavailable"); return false;
    }
    b.view = runtime.FindClassAnyImage("Lens.Gameplay.UI.Watermark", "ModuleNetworkInfoView");
    b.tmp = runtime.FindClass("Unity.TextMeshPro.dll", "TMPro", "TMP_Text");
    b.manager = runtime.FindClassAnyImage("Azur.Gameplay.PlayerModule", "PlayerManager");
    auto* world = runtime.FindClassAnyImage("Lens.Gameplay.Modules.Azur", "AzurWorld");
    auto* object = runtime.FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Object");
    auto* resources = runtime.FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Resources");
    if (!b.view || !b.tmp || !b.manager || !world || !object || !resources) {
        reason = Tr("runtime.types-unavailable", "Watermark, player identity or Unity types unavailable"); return false;
    }
    b.uid = runtime.FindField(b.view, "m_txtUid");
    b.player_manager = runtime.FindField(world, "<playerMgr>k__BackingField");
    if (!b.uid || runtime.TypeName(api.il2cpp_field_get_type(b.uid)) != "TMPro.TMP_Text" ||
        (api.il2cpp_field_get_flags(b.uid) & 0x10u) || !b.player_manager ||
        runtime.TypeName(api.il2cpp_field_get_type(b.player_manager)) != "Azur.Gameplay.PlayerModule.PlayerManager" ||
        !(api.il2cpp_field_get_flags(b.player_manager) & 0x10u)) {
        reason = Tr("runtime.field-unavailable", "Watermark or player-manager field shape does not match this build"); return false;
    }
    b.find = runtime.FindMethodBySignature(resources, "FindObjectsOfTypeAll", {"System.Type"});
    b.alive = runtime.FindMethodBySignature(object, "op_Implicit", {"UnityEngine.Object"});
    b.identity = runtime.FindMethodBySignature(b.manager, "get_myDisplayPlayerId", {});
    b.refresh = runtime.FindMethodBySignature(b.view, "RefreshUidText", {});
    if (!MethodShape(runtime, b.find, "UnityEngine.Object[]", true) || !MethodShape(runtime, b.alive, "System.Boolean", true) ||
        !MethodShape(runtime, b.identity, "System.UInt64", false) || !MethodShape(runtime, b.refresh, "System.Void", false)) {
        reason = Tr("runtime.discovery-unavailable", "Watermark or player identity method shape is unavailable"); return false;
    }
    return true;
}
bool Alive(const Context& runtime, Il2CppObject* object) {
    if (!object) return false;
    void* args[]{object}; Il2CppObject* result{};
    if (!runtime.Invoke(g_state.binding.alive, nullptr, args, &result) || !result) return false;
    const auto* value = static_cast<const bool*>(runtime.Functions().il2cpp_object_unbox(result));
    return value && *value;
}
bool ReadIdentity(const Context& runtime, std::string& identity) {
    const auto& api = runtime.Functions();
    Il2CppObject* manager{};
    api.il2cpp_field_static_get_value(g_state.binding.player_manager, &manager);
    if (!manager || !api.il2cpp_class_is_assignable_from(g_state.binding.manager, api.il2cpp_object_get_class(manager))) return false;
    Il2CppObject* result{};
    if (!runtime.Invoke(g_state.binding.identity, manager, nullptr, &result) || !result) return false;
    const auto* value = static_cast<const std::uint64_t*>(api.il2cpp_object_unbox(result));
    if (!value || !*value) return false;
    identity = std::to_string(*value);
    return true;
}
bool ReadText(const Context& runtime, const Target& target, Il2CppObject* object, std::string& text) {
    Il2CppObject* result{};
    if (!runtime.Invoke(target.get, object, nullptr, &result) || !result) return false;
    auto value = runtime.ReadString(result, 1152);
    if (!value) return false;
    text = std::move(*value); return true;
}
bool WriteText(const Context& runtime, const Target& target, Il2CppObject* object, const std::string& text) {
    auto* value = runtime.Functions().il2cpp_string_new(text.c_str());
    if (!value) return false;
    void* args[]{value}; Il2CppObject* ignored{};
    return runtime.Invoke(target.set, object, args, &ignored);
}
// Match the independently observed account identity, not an arbitrary digit run.
// Preserve labels/markup around the exact token when the prefix is visible.
bool MatchIdentity(const std::string& text, const std::string& identity, std::string& prefix, std::string& suffix) {
    const auto at = text.find(identity);
    if (at == std::string::npos || text.find(identity, at + identity.size()) != std::string::npos) return false;
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto end = at + identity.size();
    if ((at && digit(text[at - 1])) || (end < text.size() && digit(text[end]))) return false;
    prefix = text.substr(0, at); suffix = text.substr(end); return true;
}
void Release(const Context& runtime, Target& target) {
    const auto& api = runtime.Functions();
    if (target.view) api.il2cpp_gchandle_free(target.view);
    if (target.text) api.il2cpp_gchandle_free(target.text);
    target.view = target.text = {};
}
bool Restore(const Context& runtime, Target& target) {
    const auto& api = runtime.Functions();
    auto* text = api.il2cpp_gchandle_get_target(target.text);
    if (!Alive(runtime, text) || target.last_written.empty()) return true;
    std::string current;
    if (!ReadText(runtime, target, text, current)) return false;
    if (current != target.last_written) return true;
    auto* view = api.il2cpp_gchandle_get_target(target.view);
    if (!Alive(runtime, view)) return false;
    Il2CppObject* current_field{};
    api.il2cpp_field_get_value(view, g_state.binding.uid, &current_field);
    if (current_field != text) return WriteText(runtime, target, text, target.original);
    Il2CppObject* ignored{};
    // The game's own formatter reads the current player; do not replay a prior
    // account's cached text after reconnect or a world transition.
    if (!runtime.Invoke(g_state.binding.refresh, view, nullptr, &ignored)) return false;
    std::string identity, prefix, suffix;
    return ReadText(runtime, target, text, current) && ReadIdentity(runtime, identity) &&
        MatchIdentity(current, identity, prefix, suffix);
}
void Publish(std::string_view error = {}, const bool operation_succeeded = false) {
    auto& s = g_state;
    if (!error.empty() && error != s.last_error) {
        s.last_error = error;
        LogFailure(error);
    } else if (error.empty() && operation_succeeded) s.last_error.clear();
    Status next{};
    std::snprintf(next.original_uid, sizeof(next.original_uid), "%s", s.identity.c_str());
    if (!s.targets.empty()) std::snprintf(next.original_text, sizeof(next.original_text), "%s", s.targets.front().original.c_str());
    std::scoped_lock lock(s.mutex); s.published = next;
}
void Discover(const Context& runtime) {
    auto& s = g_state; const auto& api = runtime.Functions();
    auto* type = api.il2cpp_type_get_object(api.il2cpp_class_get_type(s.binding.view));
    void* args[]{type}; Il2CppObject* array{};
    if (!type || !runtime.Invoke(s.binding.find, nullptr, args, &array) || !array) return;
    const auto root = api.il2cpp_gchandle_new(array, false);
    if (!root) return;
    struct Root { const CabbirdIl2CppApiV1& api; Il2CppGCHandle handle; ~Root() { api.il2cpp_gchandle_free(handle); } } owned{api, root};
    const auto count = api.il2cpp_array_length(array);
    if (count > 32) return;
    for (std::uint32_t i = 0; i < count && s.targets.size() < 8; ++i) {
        Il2CppObject* view{};
        if (!runtime.ArrayReferenceAt(array, i, &view) || !Alive(runtime, view) || api.il2cpp_object_get_class(view) != s.binding.view) continue;
        Il2CppObject* text{}; api.il2cpp_field_get_value(view, s.binding.uid, &text);
        if (!Alive(runtime, text) || std::any_of(s.targets.begin(), s.targets.end(), [&](const Target& t) { return api.il2cpp_gchandle_get_target(t.text) == text; })) continue;
        auto* cls = api.il2cpp_object_get_class(text);
        if (!api.il2cpp_class_is_assignable_from(s.binding.tmp, cls)) continue;
        Target target;
        target.get = runtime.FindMethodBySignature(cls, "get_text", {});
        target.set = runtime.FindMethodBySignature(cls, "set_text", {"System.String"});
        if (!MethodShape(runtime, target.get, "System.String", false) || !MethodShape(runtime, target.set, "System.Void", false) ||
            !ReadText(runtime, target, text, target.original) || !MatchIdentity(target.original, s.identity, target.prefix, target.suffix)) continue;
        target.view = api.il2cpp_gchandle_new(view, false); target.text = api.il2cpp_gchandle_new(text, false);
        if (!target.view || !target.text) { Release(runtime, target); continue; }
        s.targets.push_back(std::move(target));
    }
}
void CABBIRD_CALL OnRefresh(Il2CppObject* self, const MethodInfo* method) noexcept {
    auto& s = g_state;
    CabbirdGenerationHandleV1 lease{};
    const bool held = s.hooks && cabbird::sdk::Succeeded(s.hooks->begin_callback(s.hooks->user, s.hook, &lease));
    if (!held) return;
    try {
        if (s.original) reinterpret_cast<RefreshFn>(s.original)(self, method);
        // A native refresh may run outside the host's observed Game callback.
        // The next on_update reapplies the native settings copy; only that
        // callback may enter with_runtime and borrow its IL2CPP table.
    } catch (...) {}
    static_cast<void>(s.hooks->end_callback(s.hooks->user, lease));
}
bool InstallHook(std::string& reason) {
    auto& s = g_state;
    if (s.hook.id) return true;
    if (!s.verified_refresh || !s.verified_identity) {
        reason = Tr("runtime.profile-unavailable", "Watermark entry or player-identity call edge failed profile validation"); return false;
    }
    if (!s.metadata || !s.metadata->resolve_method || !s.hooks || !s.hooks->create || !s.hooks->release || !s.hooks->begin_callback || !s.hooks->end_callback) {
        reason = Tr("runtime.hook-unavailable", "Verified watermark hook services unavailable"); return false;
    }
    CabbirdUnityMethodRequestV1 method{};
    method.struct_size = sizeof(method); method.method_kind = CABBIRD_UNITY_METHOD_V1_INSTANCE;
    method.module_name = cabbird::sdk::StringView("GameAssembly.dll");
    method.type_name = cabbird::sdk::StringView("Lens.Gameplay.UI.Watermark.ModuleNetworkInfoView");
    method.method_name = cabbird::sdk::StringView("RefreshUidText"); method.return_type = cabbird::sdk::StringView("System.Void");
    CabbirdUnityMethodResultV1 result{sizeof(result)};
    if (!cabbird::sdk::Succeeded(s.metadata->resolve_method(s.metadata->user, &method, &result)) || result.address != s.verified_refresh) {
        reason = Tr("runtime.hook-unavailable", "Verified watermark hook services unavailable"); return false;
    }
    method.type_name = cabbird::sdk::StringView("Azur.Gameplay.PlayerModule.PlayerManager");
    method.method_name = cabbird::sdk::StringView("get_myDisplayPlayerId"); method.return_type = cabbird::sdk::StringView("System.UInt64");
    CabbirdUnityMethodResultV1 identity{sizeof(identity)};
    if (!cabbird::sdk::Succeeded(s.metadata->resolve_method(s.metadata->user, &method, &identity)) || identity.address != s.verified_identity) {
        reason = Tr("runtime.profile-unavailable", "Watermark entry or player-identity call edge failed profile validation"); return false;
    }
    CabbirdHookRequestV1 hook{sizeof(hook), CABBIRD_HOOK_V1_FUNCTION, result.address, reinterpret_cast<void*>(&OnRefresh), cabbird::sdk::StringView("fake-uid.refresh")};
    if (!cabbird::sdk::Succeeded(s.hooks->create(s.hooks->user, &hook, &s.original, &s.hook))) {
        reason = Tr("runtime.hook-unavailable", "Verified watermark hook services unavailable"); return false;
    }
    return true;
}
void TickBound(const Context& runtime) {
    auto& s = g_state;
    if (s.in_tick) return;
    struct TickGuard { bool& active; explicit TickGuard(bool& v) : active(v) { active = true; } ~TickGuard() { active = false; } } guard(s.in_tick);
    Settings request; { std::scoped_lock lock(s.mutex); request = s.request; }
    std::string reason;
    const auto now = Clock::now();
    if (!s.binding.view || !s.hook.id) {
        if (now < s.next_resolve) return;
        Binding binding;
        if (!Resolve(runtime, binding, reason)) { s.next_resolve = now + std::chrono::seconds(2); Publish(request.enabled ? reason : std::string_view{}); return; }
        s.binding = binding;
        if (!InstallHook(reason)) { s.next_resolve = now + std::chrono::seconds(2); Publish(request.enabled ? reason : std::string_view{}); return; }
    }
    std::string identity;
    if (!ReadIdentity(runtime, identity)) { Publish(); return; }
    if (identity != s.identity) {
        for (auto it = s.targets.begin(); it != s.targets.end();) {
            if (!Restore(runtime, *it)) { Publish(); return; }
            Release(runtime, *it); it = s.targets.erase(it);
        }
        s.identity = std::move(identity); s.next_discovery = {};
    }
    const auto& api = runtime.Functions();
    bool targets_changed{};
    std::vector<Il2CppGCHandle> restore_failed;
    for (auto it = s.targets.begin(); it != s.targets.end();) {
        auto* view = api.il2cpp_gchandle_get_target(it->view); auto* text = api.il2cpp_gchandle_get_target(it->text);
        Il2CppObject* field{};
        if (Alive(runtime, view)) api.il2cpp_field_get_value(view, s.binding.uid, &field);
        if (!Alive(runtime, text) || field != text) {
            if (Alive(runtime, text) && !Restore(runtime, *it)) {
                restore_failed.push_back(it->text); ++it; continue;
            }
            Release(runtime, *it); it = s.targets.erase(it); targets_changed = true; continue;
        }
        ++it;
    }
    // A valid watermark binding is retained by GC handles. Global Unity object
    // discovery is only needed initially or after that binding is replaced.
    // Prune first so a rebuilt HUD can be rebound in this same Game callback.
    if (targets_changed) s.next_discovery = {};
    if (request.enabled && (s.targets.empty() || targets_changed) && now >= s.next_discovery) {
        Discover(runtime);
        s.next_discovery = now + std::chrono::seconds(2);
    }
    bool operation_succeeded{};
    for (auto it = s.targets.begin(); it != s.targets.end(); ++it) {
        if (std::find(restore_failed.begin(), restore_failed.end(), it->text) != restore_failed.end()) continue;
        auto* text = api.il2cpp_gchandle_get_target(it->text);
        if (!request.enabled) {
            const bool had_override = !it->last_written.empty();
            if (Restore(runtime, *it)) {
                operation_succeeded |= had_override;
                it->last_written.clear();
            }
            continue;
        }
        std::string current;
        if (!ReadText(runtime, *it, text, current)) { reason = Tr("runtime.read-failed", "UID text read failed"); continue; }
        if (current != it->last_written) {
            std::string prefix, suffix;
            if (!MatchIdentity(current, s.identity, prefix, suffix)) {
                reason = Tr("runtime.identity-mismatch", "Watermark text does not match the current player identity"); continue;
            }
            it->original = current; it->prefix = std::move(prefix); it->suffix = std::move(suffix);
        }
        const std::string desired = request.hide_prefix ? request.display_uid : it->prefix + request.display_uid + it->suffix;
        if (current == desired) { operation_succeeded = true; continue; }
        if (!WriteText(runtime, *it, text, desired)) { reason = Tr("runtime.setter-failed", "TMP text setter failed"); continue; }
        it->last_written = desired;
        if (!ReadText(runtime, *it, text, current) || current != desired)
            reason = Tr("runtime.readback-mismatch", "Setter returned; text readback did not match");
        else operation_succeeded = true;
    }
    Publish(reason, operation_succeeded);
}
} // namespace

void Bind(const CabbirdIl2CppServiceV1* il2cpp, const CabbirdUnityMetadataServiceV1* metadata, const CabbirdHookServiceV1* hooks, std::uintptr_t verified_refresh, std::uintptr_t verified_identity) {
    g_state.il2cpp = il2cpp; g_state.metadata = metadata; g_state.hooks = hooks;
    g_state.verified_refresh = verified_refresh; g_state.verified_identity = verified_identity;
}
bool Configure(const Settings& settings, std::string& error) {
    if (!ValidDisplay(settings.display_uid, sizeof(settings.display_uid))) {
        error = Tr("runtime.invalid-display", "Use 1-256 single-line UTF-8 characters (no < > markup)."); return false;
    }
    std::scoped_lock lock(g_state.mutex); g_state.request = settings; error.clear(); return true;
}
Status Snapshot() { std::scoped_lock lock(g_state.mutex); return g_state.published; }
void Tick(const CabbirdIl2CppServiceV1* service) {
    if (!service || service->struct_size < sizeof(*service) || !service->with_runtime) {
        Settings request; { std::scoped_lock lock(g_state.mutex); request = g_state.request; }
        Publish(request.enabled ? Tr("runtime.service-unavailable", "Generic cabbird.unity.il2cpp service unavailable; update the host") : std::string_view{}); return;
    }
    const auto status = service->with_runtime(service->user, [](void*, const CabbirdIl2CppApiV1* api) -> CabbirdStatusV1 {
        try {
            if (!api || api->struct_size < sizeof(*api) || api->api_version != 1) return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
            TickBound(Context(*api)); return cabbird::sdk::Ok();
        } catch (...) { return {CABBIRD_STATUS_V1_FAILED, 0, {}}; }
    }, nullptr);
    if (!cabbird::sdk::Succeeded(status) && status.code != CABBIRD_STATUS_V1_UNAVAILABLE)
        Publish(Tr("runtime.access-failed", "IL2CPP access failed") + " (status=" + std::to_string(status.code) + ")");
}
CabbirdStatusV1 RestoreForStop() {
    auto& s = g_state;
    if (s.targets.empty()) return cabbird::sdk::Ok();
    if (!s.il2cpp || !s.il2cpp->with_runtime) return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    return s.il2cpp->with_runtime(s.il2cpp->user, [](void*, const CabbirdIl2CppApiV1* api) -> CabbirdStatusV1 {
        try {
            if (!api || api->struct_size < sizeof(*api) || api->api_version != 1) return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
            auto& state = g_state; Context runtime(*api);
            state.in_tick = true;
            struct Guard { ~Guard() { g_state.in_tick = false; } } guard;
            for (auto& target : state.targets) {
                if (!Restore(runtime, target)) return {CABBIRD_STATUS_V1_FAILED, 0, {}};
                target.last_written.clear();
            }
            return cabbird::sdk::Ok();
        } catch (...) { return {CABBIRD_STATUS_V1_FAILED, 0, {}}; }
    }, nullptr);
}
bool Shutdown(const CabbirdIl2CppServiceV1* service) {
    auto& s = g_state;
    // Host revokes/drains the hook source before Stop. Only GC cleanup may run
    // on Lifecycle; the registered Game cleanup has already refreshed the text.
    if (s.hook.id && s.hooks) {
        const auto released = s.hooks->release(s.hooks->user, s.hook);
        if (!cabbird::sdk::Succeeded(released) && released.code != CABBIRD_STATUS_V1_NOT_FOUND) return false;
    }
    std::array<Il2CppGCHandle, 16> handles{}; std::size_t count{};
    for (const auto& target : s.targets) { if (target.view) handles[count++] = target.view; if (target.text) handles[count++] = target.text; }
    if (count && (!service || !service->release_handles || !cabbird::sdk::Succeeded(service->release_handles(service->user, handles.data(), count)))) return false;
    s.targets.clear(); s.binding = {}; s.hook = {}; s.original = 0; s.identity.clear(); s.next_resolve = {}; s.next_discovery = {}; s.last_error.clear();
    std::scoped_lock lock(s.mutex); s.request.enabled = false; s.published = {}; return true;
}
} // namespace fake_uid

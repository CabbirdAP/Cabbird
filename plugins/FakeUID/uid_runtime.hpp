#pragma once
#include <cstdint>
#include "cabbird/sdk/services/il2cpp.h"
#include "cabbird/sdk/services/interop.h"
#include "cabbird/sdk/services/unity_metadata.h"
#include <string>
#include <string_view>

namespace fake_uid {
// Resolves text through the one Localizer initialized by the plugin's Load.
std::string Tr(std::string_view key, std::string_view english);
void LogFailure(std::string_view message);
struct Settings {
    bool enabled{};
    bool hide_prefix{false};
    char display_uid[1025]{"12345678"};
};
struct Status {
    char original_uid[32]{};
    char original_text[1153]{};
};
void Bind(const CabbirdIl2CppServiceV1*, const CabbirdUnityMetadataServiceV1*, const CabbirdHookServiceV1*, std::uintptr_t verified_refresh = 0, std::uintptr_t verified_identity = 0);
// Native copies only; callable from Draw. No managed entry point here.
bool Configure(const Settings& settings, std::string& error);
Status Snapshot();
CabbirdStatusV1 RestoreForStop();
// All Unity calls are made from the plugin's existing on_update callback.
void Tick(const CabbirdIl2CppServiceV1* service);
// Ordinary callbacks are drained by the host before on_stop. Releases only GC
// roots; never invokes a Unity UI method from the lifecycle worker.
bool Shutdown(const CabbirdIl2CppServiceV1* service);
}

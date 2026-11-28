#pragma once
#include "catalog_lua.hpp"
#include "cabbird/sdk/il2cpp.hpp"
#include "cabbird/sdk/localization.hpp"
#include "cabbird/sdk/services/game_lifecycle.h"
#include "cabbird/sdk/services/platform.h"
#include "cabbird/sdk/services/unity_metadata.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace unity_dump {
using namespace cabbird::sdk;
class Catalog {
    enum class Phase { Idle, Requested, Capture, Write, Complete, Failed };
    const CabbirdIl2CppServiceV1* runtime_{};
    const CabbirdUnityMetadataServiceV1* metadata_{};
    const CabbirdSchedulerServiceV1* scheduler_{};
    const CabbirdCoreServiceV1* core_{};
    const Localizer* text_{};
    std::atomic<Phase> phase_{Phase::Idle};
    std::vector<std::string> chunks_; // Game owns until the Worker task is submitted
    bool lua_state_{}; // Game only, including stop cleanup
    std::string directory_; // Request publishes the immutable path to Game/Worker
    mutable std::mutex result_mutex_;
    std::string result_; // Render reads a snapshot of the Worker result

    template<class T> static const T* Query(const Host& host, const char* id) {
        const auto service = host.Query<T>(id);
        return service && service->struct_size >= sizeof(T) ? service.get() : nullptr;
    }
    void Failure(std::string_view detail) {
        const auto line = text_->Format("log.catalog-failed", "Resource catalog export failed: {0}", std::array<std::string_view, 1>{detail});
        core_->log(core_->user, CABBIRD_CORE_LOG_LEVEL_V1_ERROR, StringView(line));
        { std::lock_guard lock(result_mutex_); result_ = line; }
        phase_.store(Phase::Failed);
    }
    const CabbirdMethodInfoV1* Resolve(il2cpp::Context& rt, const char* type, const char* name,
        const char* result, std::initializer_list<const char*> parameters = {}) {
        std::vector<CabbirdStringViewV1> args;
        for (const auto* arg : parameters) args.push_back(StringView(arg));
        CabbirdUnityMethodRequestV1 request{sizeof(request)};
        request.module_name = StringView("GameAssembly.dll"); request.type_name = StringView(type);
        request.method_name = StringView(name); request.return_type = StringView(result);
        request.parameter_types = args.data(); request.parameter_count = args.size(); request.method_kind = CABBIRD_UNITY_METHOD_V1_STATIC;
        CabbirdUnityMethodResultV1 resolved{sizeof(resolved)};
        const auto status = metadata_->resolve_method(metadata_->user, &request, &resolved);
        if (!Succeeded(status)) throw std::runtime_error(std::string(type) + "." + name + " metadata is unavailable");
        const std::string full(type); const auto dot = full.rfind('.');
        const auto* method = rt.FindMethodBySignature(rt.FindClassAnyImage(full.substr(0, dot).c_str(), full.substr(dot + 1).c_str()), name, parameters);
        if (!method || rt.TypeName(rt.Functions().il2cpp_method_get_return_type(method)) != result ||
            !(rt.Functions().il2cpp_method_get_flags(method, nullptr) & 0x10)) throw std::runtime_error("Catalog method shape mismatch");
        return method;
    }
    CabbirdIl2CppObjectV1* Eval(il2cpp::Context& rt, const char* source, bool capture = false) {
        CabbirdIl2CppObjectV1* result{};
        if (!rt.Invoke(Resolve(rt, "Lens.Framework.Managers.LuaManager", "IsLuaStarted", "System.Boolean"), nullptr, nullptr, &result) || !result)
            throw std::runtime_error("Lua startup state is unavailable");
        const auto* started = rt.Functions().il2cpp_object_unbox(result);
        if (!started || !*static_cast<const bool*>(started)) throw std::runtime_error("Game Lua environment is not started");
        auto* script = rt.Functions().il2cpp_string_new(source); void* args[]{script};
        const auto* method = Resolve(rt, "Lens.Framework.Managers.LuaManager", "DoString", "System.Object[]", {"System.String"});
        // No retained Lua state exists until the capture chunk can actually run.
        // In particular, a request before Lua startup must not require Lua cleanup.
        if (capture) lua_state_ = true;
        if (!rt.Invoke(method, nullptr, args, &result))
            throw std::runtime_error("Catalog Lua invocation failed; see the game log");
        return result;
    }
    static CabbirdStatusV1 CABBIRD_CALL Capture(void* user, const CabbirdIl2CppApiV1* api) noexcept {
        auto& c = *static_cast<Catalog*>(user);
        try {
            il2cpp::Context rt(*api);
            if (!c.lua_state_) {
                c.chunks_.clear();
                c.Resolve(rt, "YooAsset.YooAssets", "GetAllPackages", "System.Collections.Generic.List<YooAsset.ResourcePackage>");
            }
            auto* result = c.Eval(rt, kCatalogChunk, true);
            CabbirdIl2CppObjectV1 *chunk{}, *done{};
            if (!result || api->il2cpp_array_length(result) != 2 || !rt.ArrayReferenceAt(result, 0, &chunk) || !rt.ArrayReferenceAt(result, 1, &done) || !done)
                throw std::runtime_error("Unexpected resource catalog response");
            const auto text = rt.ReadString(chunk, 4 * 1024 * 1024);
            const auto* complete = static_cast<const bool*>(api->il2cpp_object_unbox(done));
            if (!text || !complete) throw std::runtime_error("Unreadable resource catalog response");
            c.chunks_.push_back(*text);
            if (*complete) {
                c.lua_state_ = false;
                c.phase_.store(Phase::Write);
                CabbirdGenerationHandleV1 task{};
                if (!Succeeded(c.scheduler_->schedule(c.scheduler_->user, 0, Write, &c, &task))) throw std::runtime_error("Cannot schedule catalog file output");
            }
            return Ok();
        } catch (const std::exception& e) { try { c.Failure(e.what()); } catch (...) {} }
        catch (...) { try { c.Failure("Unexpected native exception"); } catch (...) {} }
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    static void CABBIRD_CALL Write(void* user, CabbirdGenerationHandleV1) noexcept {
        auto& c = *static_cast<Catalog*>(user);
        try {
            FILETIME stamp{}; GetSystemTimeAsFileTime(&stamp);
            const auto root = std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(c.directory_.data()), c.directory_.size())).lexically_normal();
            if (!root.is_absolute()) throw std::runtime_error("Catalog directory must be an absolute path");
            const auto run = std::to_string((static_cast<std::uint64_t>(stamp.dwHighDateTime) << 32) | stamp.dwLowDateTime) + "/";
            std::filesystem::create_directories(root / run);
            auto index = nlohmann::json{{"schemaVersion", 1}, {"source", "active-package-manifests"}, {"resources", nlohmann::json::array()}, {"luaModules", nlohmann::json::array()}};
            std::size_t resourceCount{}, moduleCount{}, part{};
            const auto save = [&](const std::string& name, const nlohmann::json& document) {
                const auto bytes = document.dump(2);
                const auto destination = root / std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(name.data()), name.size()));
                auto temporary = destination; temporary += L".tmp";
                try {
                    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                    file.close();
                    if (!file || !MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                        throw std::runtime_error("Cannot write " + c.directory_ + "/" + name);
                } catch (...) { std::error_code ignored; std::filesystem::remove(temporary, ignored); throw; }
            };
            for (const auto& chunk : c.chunks_) {
                auto resources = nlohmann::json::parse(chunk), modules = nlohmann::json::array();
                for (const auto& row : resources) {
                    const auto path = row.at("path").get<std::string>();
                    const auto lua = path.find("/Lua/");
                    if (lua != std::string::npos && path.ends_with(".lua.bytes")) {
                        const auto src = path.find("/src/", lua);
                        if (src != std::string::npos) {
                            auto module = row; module["module"] = path.substr(src + 5, path.size() - src - 5 - 10);
                            modules.push_back(std::move(module));
                        }
                    }
                }
                // Keep Game batch boundaries instead of building one huge file.
                const auto resourceFile = run + "resources-" + std::to_string(part) + ".json";
                save(resourceFile, {{"schemaVersion", 1}, {"entries", resources}});
                index["resources"].push_back({{"file", resourceFile}, {"count", resources.size()}});
                resourceCount += resources.size();
                if (!modules.empty()) {
                    const auto moduleFile = run + "lua-modules-" + std::to_string(part) + ".json";
                    save(moduleFile, {{"schemaVersion", 1}, {"entries", modules}});
                    index["luaModules"].push_back({{"file", moduleFile}, {"count", modules.size()}});
                    moduleCount += modules.size();
                }
                ++part;
            }
            index["resourceCount"] = resourceCount; index["luaModuleCount"] = moduleCount;
            save("index.json", index); // Publish only complete inventories.
            const auto count = std::to_string(resourceCount), luaCount = std::to_string(moduleCount);
            const auto encoded = (root / L"index.json").u8string();
            const std::string path(reinterpret_cast<const char*>(encoded.data()), encoded.size());
            const auto line = c.text_->Format("log.catalog-written", "Wrote {0} resources and {1} Lua modules to {2}", std::array<std::string_view, 3>{count, luaCount, path});
            c.core_->log(c.core_->user, CABBIRD_CORE_LOG_LEVEL_V1_INFO, StringView(line));
            { std::lock_guard lock(c.result_mutex_); c.result_ = line; }
            c.chunks_.clear(); c.phase_.store(Phase::Complete);
        } catch (const std::exception& e) { try { c.Failure(e.what()); } catch (...) {} }
        catch (...) { try { c.Failure("Unexpected catalog writer exception"); } catch (...) {} }
    }
    static CabbirdStatusV1 CABBIRD_CALL ClearRuntime(void* user, const CabbirdIl2CppApiV1* api) noexcept {
        auto& c = *static_cast<Catalog*>(user);
        try { il2cpp::Context rt(*api); c.Eval(rt, "rawset(_G, '__cabbird_unity_dump_catalog', nil)"); c.lua_state_ = false; return Ok(); }
        catch (...) { return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}}; }
    }
    static CabbirdStatusV1 CABBIRD_CALL Cleanup(void* user) noexcept {
        auto& c = *static_cast<Catalog*>(user);
        try { return c.lua_state_ ? c.runtime_->with_runtime(c.runtime_->user, ClearRuntime, &c) : Ok(); }
        catch (...) { return {CABBIRD_STATUS_V1_FAILED, 0, {}}; }
    }
public:
    bool Bind(const CabbirdHostApiV1* api, const Localizer* text) {
        const Host host(api); text_ = text;
        runtime_ = Query<CabbirdIl2CppServiceV1>(host, CABBIRD_IL2CPP_SERVICE_V1_ID);
        metadata_ = Query<CabbirdUnityMetadataServiceV1>(host, CABBIRD_UNITY_METADATA_SERVICE_V1_ID);
        scheduler_ = Query<CabbirdSchedulerServiceV1>(host, CABBIRD_SCHEDULER_SERVICE_V1_ID);
        core_ = Query<CabbirdCoreServiceV1>(host, CABBIRD_CORE_SERVICE_V1_ID);
        const auto* lifecycle = Query<CabbirdGameLifecycleServiceV1>(host, CABBIRD_GAME_LIFECYCLE_SERVICE_V1_ID);
        CabbirdGenerationHandleV1 cleanup{};
        return runtime_ && metadata_ && scheduler_ && core_ && lifecycle &&
            Succeeded(lifecycle->register_stop_cleanup(lifecycle->user, Cleanup, this, &cleanup));
    }
    void Request(std::string directory) {
        if (Busy()) return;
        directory_ = std::move(directory);
        { std::lock_guard lock(result_mutex_); result_.clear(); }
        phase_.store(Phase::Requested);
    }
    std::string Result() const { std::lock_guard lock(result_mutex_); return result_; }
    bool Busy() const { const auto phase = phase_.load(); return phase == Phase::Requested || phase == Phase::Capture || phase == Phase::Write; }
    bool Failed() const { return phase_.load() == Phase::Failed; }
    void Update() noexcept {
        const auto phase = phase_.load();
        if (phase != Phase::Requested && phase != Phase::Capture) return;
        try {
            if (phase == Phase::Requested) {
                if (lua_state_ && !Succeeded(runtime_->with_runtime(runtime_->user, ClearRuntime, this))) {
                    Failure("Cannot clear the previous catalog capture"); return;
                }
                chunks_.clear(); phase_.store(Phase::Capture);
            }
            const auto status = runtime_->with_runtime(runtime_->user, Capture, this);
            if (!Succeeded(status) && phase_.load() != Phase::Failed) Failure("Game runtime is unavailable");
        } catch (...) { try { Failure("Unexpected Game callback exception"); } catch (...) {} }
    }
};
}

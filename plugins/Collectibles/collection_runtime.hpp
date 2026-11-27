#pragma once

#include "cabbird/sdk/cpp.hpp"
#include "cabbird/sdk/il2cpp.hpp"
#include "cabbird/sdk/profile.hpp"
#include "cabbird/sdk/services/unity_metadata.h"
#include "collection_receipt.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <string>
#include <sstream>
#include <map>
#include <unordered_set>
#include <vector>

namespace collectibles {
using namespace cabbird::sdk;
using Runtime = il2cpp::Context;
using Object = CabbirdIl2CppObjectV1;
using Class = CabbirdIl2CppClassV1;
using Method = CabbirdMethodInfoV1;
using Field = CabbirdFieldInfoV1;
struct Vector3 { float x{}, y{}, z{}; };
enum class CompletionState { Unknown, Incomplete, Complete, Unavailable };
enum class RouteState { AlreadyComplete, Queued, Moving, Waiting, Interacting, Complete, Skipped, UnknownTimeout, Unsupported, InteractionTimeout, Stopped, Failed, NotSpawned, Filtered };
enum class PresenceState { MayLoad, NotSpawned, Unavailable };
struct TargetPresence { CompletionState completion{CompletionState::Unavailable}; PresenceState presence{PresenceState::Unavailable};
    // A failed native read is not the same as "this target is unavailable": the route can retry
    // or skip one target instead of ending the whole run.
    bool transient{}; };
enum class PickupKind { None, Collection, Book, Special };
struct Target {
    std::int32_t mapId{};
    std::array<double, 3> position{};
    bool chest{};
    CompletionState completion{CompletionState::Unknown};
    RouteState route{RouteState::Queued};
    std::string kind;
    std::int32_t collectionId{}, categoryId{};
    std::string categoryLabel, name;
    bool official{};
    PickupKind nativePickup{PickupKind::None};
    bool livePosition{};
};
enum class InteractionState { Waiting, Discovering, Prepared, Accepted, Unsupported, Unavailable };
enum class ScanState { Pending, Complete, Unavailable };
enum class ScanWorld { Current, Changed, Unavailable };

// All names below are managed runtime identifiers. They are matched exactly and
// are compatibility data rather than user-visible text.
constexpr auto kWorld = "Lens.Gameplay.Modules.Azur.AzurWorld";
constexpr auto kMap = "Azur.Gameplay.World.Service.Map.MapService";
constexpr auto kScene = "Azur.Gameplay.World.Service.SceneService";
constexpr auto kObjectType = "Lens.Gameplay.Modules.Azur.EWorldObjectType";
constexpr auto kMapManager = "Azur.Gameplay.World.WorldMapMgr";
constexpr auto kInteractManager = "Azur.Gameplay.World.WorldInteract.WorldInteractManager";
constexpr auto kInteract = "Azur.Gameplay.World.WorldInteract.WorldInteractObject";
constexpr auto kHost = "Azur.Gameplay.World.WorldInteract.WorldInteractHost";
constexpr auto kWorldEntity = "Lens.Gameplay.Modules.BigWorld.WorldEntity";
constexpr auto kList = "System.Collections.Generic.List<System.Int32>";
enum class Call : std::size_t {
    World, WorldId, Scene, MapManager, InteractManager, Prepared, Objects, Position, Completion,
    Host, WorldEntity, WorldGuid, Sid, Enabled, CanTrigger, Action, HasInteractionAction, Trigger, DirectChest, CompleteChest,
    MapRow, MapSpawner, CollectId, CollectType, CollectName, CollectSpawners, CategoryId, CategoryName, CategoryShow, LangValue, TableReady,
    Aoi, UpdateAoi, HostValid, WorldUnitManager, WorldUnit, UnitMapId,
    InstanceUnitId, ComponentInParent, NativeChestClick, EnumParse,
    RunningGraphs, GraphTarget, GraphEntity, GraphMapId, GraphBinder, GraphNodes, GraphRunning, GraphPaused, NewFlow, FinishPuzzle, LegacyPuzzleFinish, Response, FlowOutputCall, PortConnected, GateInput, ImmediateChestAction, PortParent,
    ObjectActive, MapUnit, UnitDisposed, UnitInstance, HostPosition, ScenePosition, ArrayCreate,
    SceneTransform, ComponentObject, ObjectHierarchyActive, UnitLogicActive, UnitViewActive, Count
};
struct Shape {
    const char* namespaze;
    const char* klass;
    const char* method;
    const char* result;
    bool isStatic{};
    std::initializer_list<const char*> parameters;
    const char* symbol{};
};
inline const Shape kShapes[] = {
    {"Lens.Gameplay.Modules.Azur", "AzurWorld", "get_Instance", kWorld, true},
    {"Lens.Gameplay.Modules.Azur", "AzurWorld", "get_CurWorldId", "System.Int32"},
    {"Lens.Gameplay.Modules.Azur", "AzurWorld", "get_service", kScene, true},
    {"Lens.Gameplay.Modules.Azur", "AzurWorld", "get_WorldMapMgr", kMapManager, true},
    {"Lens.Gameplay.Modules.Azur", "AzurWorld", "get_InteractMgr", kInteractManager, true},
    {"Azur.Gameplay.World.Service.Map", "MapService", "get_IsScenePrepared", "System.Boolean"},
    {"Azur.Gameplay.World.Service.Map", "LocalMap", "GetObjectsIdByType", kList, false, {kObjectType}, "objectsByType"},
    {"Azur.Gameplay.World", "WorldMapMgr", "GetWorldMapCreatePos", "System.Boolean", false, {"System.Int32", "UnityEngine.Vector3&"}, "createPosition"},
    {"Azur.Gameplay.World.Service.Map", "ServerMap", "GetObjectServerCompleteState", "Azur.Gameplay.World.Service.Map.EObjectServerCompleteState", false, {"System.Int32"}, "completion"},
    {"Azur.Gameplay.World.WorldInteract", "WorldInteractObject", "get_Host", kHost},
    {"Azur.Gameplay.World.WorldInteract", "WorldInteractHost", "GetWorldEntity", kWorldEntity},
    {"Lens.Gameplay.Modules.BigWorld", "WorldEntity", "GetWorldGuid", "System.Int32"},
    {"Azur.Gameplay.World.WorldInteract", "WorldInteractObject", "GetSID", "System.Int32"},
    {"Azur.Gameplay.World.WorldInteract", "WorldInteractObject", "get_IsEnabled", "System.Boolean"},
    {"Azur.Gameplay.World.WorldInteract", "WorldInteractObject", "get_CanTrigger", "System.Boolean"},
    {"Azur.Gameplay.World.WorldInteract", "WorldInteractObject", "GetAction", "Azur.Gameplay.World.WorldInteract.WorldInteractActionBase"},
    {"Azur.Gameplay.World.WorldInteract", "WorldInteractObject", "get_HasInteractionAction", "System.Boolean"},
    {"Azur.Gameplay.World.WorldInteract", "WorldInteractManager", "TryTriggerButtonClick", "System.Boolean", false, {"System.Int32"}, "triggerButton"},
    {"Azur.Gameplay.World.WorldInteract", "DirectSendMessageAction", "OnTriggerInteract", "System.Void", false,
        {"Azur.Gameplay.World.WorldInteract.WorldInteractActionInvoker.InteractReason"}, "directChest"},
    {"Azur.Gameplay.World.WorldInteract", "SetWorldObjComplete", "OnTriggerInteract", "System.Void", false,
        {"Azur.Gameplay.World.WorldInteract.WorldInteractActionInvoker.InteractReason"}, "completeChest"},
    {"Azur.Gameplay.Table", "TDWorldmapTable", "GetData", "Azur.Gameplay.Table.TDWorldmap", true, {"System.Int32", "System.Int32"}, "mapRow"},
    {"Azur.Gameplay.Table", "TDWorldmap", "get_spawnerId", "System.Int32", false, {}, "mapSpawner"},
    {"Azur.Gameplay.Table", "TDWorldCollecting", "get_id", "System.Int32", false, {}, "collectId"},
    {"Azur.Gameplay.Table", "TDWorldCollecting", "get_type", "System.Int32", false, {}, "collectType"},
    {"Azur.Gameplay.Table", "TDWorldCollecting", "get_name", "Lens.Gameplay.Modules.BigWorld.Config.LangString", false, {}, "collectName"},
    {"Azur.Gameplay.Table", "TDWorldCollecting", "get_spawnerId", kList, false, {}, "collectSpawners"},
    {"Azur.Gameplay.Table", "TDWorldCollectingType", "get_id", "System.Int32", false, {}, "categoryId"},
    {"Azur.Gameplay.Table", "TDWorldCollectingType", "get_name", "Lens.Gameplay.Modules.BigWorld.Config.LangString", false, {}, "categoryName"},
    {"Azur.Gameplay.Table", "TDWorldCollectingType", "get_show", "System.Int32", false, {}, "categoryShow"},
    {"Lens.Gameplay.Modules.BigWorld.Config", "LangString", "get_value", "System.String", false, {}, "langValue"},
    {"Azur.Gameplay.Core.Table", "TableMgr", "TableLoadReady", "System.Boolean", true, {}, "tableReady"},
    {"Lens.Gameplay.Modules.Azur", "AzurWorld", "get_AOI", "Lens.Gameplay.Modules.Azur.WorldAOIMgr", true},
    {"Lens.Gameplay.Modules.Azur", "WorldAOIMgr", "UpdateAOI", "System.Void", false, {"UnityEngine.Vector3"}, "updateArrivalAoi"},
    {"Azur.Gameplay.World.WorldInteract", "WorldInteractHost", "IsHostValid", "System.Boolean"},
    {"Lens.Gameplay.Modules.Azur", "AzurWorld", "get_worldUnitMgr", "Azur.Gameplay.Units.WorldUnitMgr", true},
    {"Azur.Gameplay.Units", "WorldUnitMgr", "GetWorldUnit", "Azur.Gameplay.Units.World.WorldUnit", false, {"System.UInt64"}},
    {"Azur.Gameplay.Units.World", "WorldUnit", "get_WorldMapId", "System.Int32"},
    {"Azur.Gameplay.Units.World", "WorldInstance", "get_worldUnitID", "System.UInt64"},
    {"UnityEngine", "GameObject", "GetComponentInParent", "UnityEngine.Component", false, {"System.Type", "System.Boolean"}},
    {"Azur.Gameplay.World.WorldInteract", "WorldInteractObject", "TriggerInteractEvt", "System.Void", false,
        {"Azur.Gameplay.World.WorldInteract.WorldInteractActionInvoker.InteractReason"}, "nativeChestClick"},
    {"System", "Enum", "Parse", "System.Object", true, {"System.Type", "System.String"}},
    {"Lens.Gameplay.Modules.BigWorld.NodeCanvas.Framework", "Graph", "get_runningGraphs",
        "System.Collections.Generic.IEnumerable<Lens.Gameplay.Modules.BigWorld.NodeCanvas.Framework.Graph>", true},
    {"Lens.Gameplay.Modules.BigWorld.NodeCanvas.Framework", "Graph", "get_graphTarget", "Lens.Gameplay.Modules.BigWorld.IGraphTarget"},
    {"Lens.Gameplay.Modules.BigWorld.NodeCanvas.Framework", "Graph", "get_entity", "Lens.Gameplay.Modules.BigWorld.EntityHandle"},
    {"Lens.Gameplay.Modules.BigWorld", "GraphHelper", "GetGraphWorldObjectId", "System.Int32", true,
        {"Lens.Gameplay.Modules.BigWorld.EntityHandle", "Lens.Gameplay.Modules.BigWorld.IGraphTarget"}},
    {"Lens.Gameplay.Modules.BigWorld", "GraphHelper", "GetGraphContinerObjectId", "Azur.Gameplay.World.Service.Map.ICommonStateBinder", true,
        {"Lens.Gameplay.Modules.BigWorld.EntityHandle", "Lens.Gameplay.Modules.BigWorld.IGraphTarget"}},
    {"Lens.Gameplay.Modules.BigWorld.NodeCanvas.Framework", "Graph", "get_allNodes",
        "System.Collections.Generic.List<Lens.Gameplay.Modules.BigWorld.NodeCanvas.Framework.Node>"},
    {"Lens.Gameplay.Modules.BigWorld.NodeCanvas.Framework", "Graph", "get_isRunning", "System.Boolean"},
    {"Lens.Gameplay.Modules.BigWorld.NodeCanvas.Framework", "Graph", "get_isPaused", "System.Boolean"},
    {"Lens.Gameplay.Modules.BigWorld.FlowCanvas", "Flow", "get_New", "Lens.Gameplay.Modules.BigWorld.FlowCanvas.Flow", true},
    {"Azur.Gameplay.Graph.Blueprint", "PuzzleSetStateNode", "OnFlowInput", "System.Void", false,
        {"Lens.Gameplay.Modules.BigWorld.FlowCanvas.Flow"}, "nativePuzzleFinish"},
    {"Azur.Gameplay.Graph.Blueprint", "SetPuzzleState", "OnFlowComplete", "System.Void", false,
        {"Lens.Gameplay.Modules.BigWorld.FlowCanvas.Flow"}, "nativeLegacyPuzzleFinish"},
    {"Azur.Gameplay.World.DropItems", "DropItemManager", "OnWorldObjInteract", "System.Void", false,
        {"System.Int32", "System.Object"}, "receiptResponse"},
    {"Lens.Gameplay.Modules.BigWorld.FlowCanvas", "FlowOutput", "Call", "System.Void", false,
        {"Lens.Gameplay.Modules.BigWorld.FlowCanvas.Flow"}, "puzzleReadyOutput"},
    {"Lens.Gameplay.Modules.BigWorld.FlowCanvas", "Port", "get_isConnected", "System.Boolean"},
    {"Azur.Gameplay.Graph.Blueprint", "InitPuzzleStateCheck", "OnFlowInput", "System.Void", false,
        {"Lens.Gameplay.Modules.BigWorld.FlowCanvas.Flow"}, "puzzleGateInput"},
    {"Azur.Gameplay.World.WorldInteract", "WorldInteractActionBase", "InvokeInteractionAction", "System.Void", false,
        {"System.Int32"}, "nativeChestAction"},
    {"Lens.Gameplay.Modules.BigWorld.FlowCanvas", "Port", "get_parent", "Lens.Gameplay.Modules.BigWorld.FlowCanvas.FlowNode"},
    {"Azur.Gameplay.World.Service.Map", "MapService", "IsObjectActive", "System.Boolean", false, {"System.Int32"}},
    {"Azur.Gameplay.World", "WorldMapMgr", "GetWorldMapUnit", "Azur.Gameplay.Units.World.WorldUnit", false, {"System.Int32"}},
    {"Azur.Gameplay.Units.World", "WorldUnit", "IsDispose", "System.Boolean"},
    {"Azur.Gameplay.Units.World", "WorldUnit", "get_WorldInstance", "Azur.Gameplay.Units.World.IWorldInstance"},
    {"Azur.Gameplay.World.WorldInteract", "WorldInteractHost", "GetWorldPosition", "UnityEngine.Vector3"},
    {"Azur.Gameplay.Units.World", "SceneComponent", "TryGetCurrentPos", "System.Boolean", false, {"UnityEngine.Vector3&"}},
    {"System", "Array", "CreateInstance", "System.Array", true, {"System.Type", "System.Int32"}},
    {"Azur.Gameplay.Units.World", "SceneComponent", "TryGetCurrentTransform", "System.Boolean", false, {"UnityEngine.Transform&"}},
    {"UnityEngine", "Component", "get_gameObject", "UnityEngine.GameObject"},
    {"UnityEngine", "GameObject", "get_activeInHierarchy", "System.Boolean"},
    {"Azur.Gameplay.Units.World", "WorldUnit", "get_LogicActive", "System.Boolean"},
    {"Azur.Gameplay.Units.World", "WorldUnit", "get_ViewActive", "System.Boolean"}
};
static_assert(std::size(kShapes) == static_cast<std::size_t>(Call::Count));

class Root final {
public:
    Root(const Runtime& runtime, Object* object) : api_(runtime.Functions()), handle_(object ? api_.il2cpp_gchandle_new(object, false) : nullptr) {}
    ~Root() { if (handle_) api_.il2cpp_gchandle_free(handle_); }
    Root(const Root&) = delete;
    Root& operator=(const Root&) = delete;
    Object* get() const { return handle_ ? api_.il2cpp_gchandle_get_target(handle_) : nullptr; }
private:
    const CabbirdIl2CppApiV1& api_;
    CabbirdIl2CppGCHandleV1 handle_{};
};
inline bool Invoke(const Runtime& runtime, const Method* method, Object* object, void** args, Object*& result) {
    return runtime.Invoke(method, object, args, &result);
}
template<class T> bool Value(const Runtime& runtime, const Method* method, Object* object, void** args, T& value) {
    Object* result{};
    if (!Invoke(runtime, method, object, args, result) || !result) return false;
    const auto* storage = runtime.Functions().il2cpp_object_unbox(result);
    if (!storage) return false;
    std::memcpy(&value, storage, sizeof(value));
    return true;
}
inline const Method* Member(const Runtime& runtime, Object* object, const char* name,
        std::initializer_list<const char*> parameters, std::string_view result = {}) {
    if (!object) return nullptr;
    const auto* method = runtime.FindMethodBySignature(runtime.Functions().il2cpp_object_get_class(object), name, parameters);
    if (!method || (!result.empty() && runtime.TypeName(runtime.Functions().il2cpp_method_get_return_type(method)) != result)) return nullptr;
    std::uint32_t implementation{};
    return (runtime.Functions().il2cpp_method_get_flags(method, &implementation) & 0x10u) == 0 ? method : nullptr;
}

inline bool StructInvoke(const Runtime& runtime, const Method* method, Object* boxed, Object*& output) {
    auto* receiver = boxed ? runtime.Functions().il2cpp_object_unbox(boxed) : nullptr;
    return receiver && Invoke(runtime, method, reinterpret_cast<Object*>(receiver), nullptr, output);
}
template<class T> bool StructValue(const Runtime& runtime, const Method* method, Object* boxed, T& output) {
    auto* receiver = boxed ? runtime.Functions().il2cpp_object_unbox(boxed) : nullptr;
    return receiver && Value(runtime, method, reinterpret_cast<Object*>(receiver), nullptr, output);
}
} // namespace collectibles
#include "collection_catalog.hpp"
namespace collectibles {
inline bool ClassifyTarget(Target& target, const CatalogEntry* entry, PickupKind native = PickupKind::None) {
    if (target.chest) return true;
    if (entry) {
        target.official = true;
        target.collectionId = entry->collectionId; target.categoryId = entry->typeId;
        target.categoryLabel = entry->category; target.name = entry->name;
        return true;
    }
    if (native != PickupKind::None) { target.nativePickup = native; return true; }
    return target.kind == "Oculus" || target.kind == "KiboEgg";
}

class Collector final {
public:
    void ResetScanReport() { stats_ = {}; worldId_ = 0; if (available_) failure_.clear(); }
    std::string_view LastFailure() const noexcept { return failure_; }
    std::string_view LastInteractionDetails() const noexcept { return interactionDetails_; }
    bool CancelInteraction(const CabbirdIl2CppServiceV1* service) const {
        receipt_.Cancel(); puzzlePreparedMap_ = 0; puzzlePreparedDetails_.clear();
        std::vector<CabbirdIl2CppGCHandleV1> handles;
        for (auto handle : {puzzleGraphs_, puzzleNodes_, puzzleSelected_, puzzleSelectedGraph_, puzzleOwner_}) if (handle) handles.push_back(handle);
        for (auto handle : puzzleChildren_) handles.push_back(handle);
        if (!handles.empty() && (!service || !service->release_handles || !Succeeded(service->release_handles(service->user, handles.data(), handles.size())))) return false;
        puzzleGraphs_ = puzzleNodes_ = puzzleSelected_ = puzzleSelectedGraph_ = puzzleOwner_ = nullptr;
        puzzleChildren_.clear(); puzzleVisited_.clear(); puzzleNodeTypes_.clear(); puzzleGateDetails_.clear(); puzzleChildIndex_ = 0;
        puzzleMap_ = 0; puzzleGraphIndex_ = puzzleNodeIndex_ = puzzleGraphMatches_ = puzzleFinishMatches_ = 0;
        return true;
    }
    bool CancelScan(const CabbirdIl2CppServiceV1* service) {
        bool released = CancelInteraction(service);
        if (!catalog_.Cancel(service)) released = false;
        if (loadedSnapshot_) {
            if (service && service->release_handles && Succeeded(service->release_handles(service->user, &loadedSnapshot_, 1))) loadedSnapshot_ = nullptr;
            else released = false;
        }
        loadedDone_ = true;
        return released;
    }
    bool ScanDegraded() const { return (scanChests_ && !loadedFailure_.empty()) || (collectCatalog_ && stats_.excludedUnclassified != 0 && !catalog_.Ready()); }
    std::string ScanSummary(std::size_t queueCount) const {
        std::ostringstream out;
        out << "worldId=" << worldId_ << " enumKinds=" << chests_.size() + collections_.size() << " selectedKinds=" << stats_.kinds.size() << " kinds=[";
        for (std::size_t i{}; i < stats_.kinds.size(); ++i) {
            const auto& kind = stats_.kinds[i];
            if (i) out << ',';
            out << kind.name << '(' << kind.value << "){count=" << kind.count << ",duplicate=" << kind.duplicates
                << ",complete=" << kind.complete << ",unknown=" << kind.unknown << ",positionSuccess=" << kind.positions << '}';
        }
        out << "] positionSuccess=" << stats_.positionSuccess << " completeExcluded=" << stats_.completeExcluded
            << " unknownRetained=" << stats_.unknownRetained << " excludedUnclassified=" << stats_.excludedUnclassified
            << " officialMatches=" << stats_.officialMatches << " queueCount=" << queueCount << ' ' << catalog_.Summary();
        out << " loadedInteractions=" << loadedInspected_ << " loadedPickupObjects=" << loadedPickups_.size()
            << " aoiFallbackMatches=" << stats_.nativeMatches << " loadedChestObjects=" << loadedChests_.size()
            << " loadedChestAdded=" << stats_.loadedChestAdded;
        if (!loadedFailure_.empty()) out << " loadedPickupDetails=" << loadedFailure_;
        out << " categoryMatches=[";
        for (const auto& [id, count] : stats_.categories) out << id << ':' << count << ',';
        out << ']';
        if (!failure_.empty()) out << " failure=" << failure_;
        return out.str();
    }
    bool Bind(const CabbirdHostApiV1* host) {
        const Host view(host);
        const auto core = view.Query<CabbirdCoreServiceV1>(CABBIRD_CORE_SERVICE_V1_ID);
        const auto metadata = view.Query<CabbirdUnityMetadataServiceV1>(CABBIRD_UNITY_METADATA_SERVICE_V1_ID);
        if (!core || core->struct_size < sizeof(*core.get()) || !core->read_memory || !core->module_base ||
            !metadata || metadata->struct_size < sizeof(*metadata.get()) || !metadata->resolve_method || !metadata->resolve_instance_field) return Error("cabbird.core/cabbird.unity.metadata service unavailable");
        core_ = core.get(); metadata_ = metadata.get();
        moduleBase_ = core_->module_base(core_->user, StringView("GameAssembly.dll"));
        auto parsed = LoadProfile(view, "compatibility.profile.json", {"ap", "GameAssembly.dll", "il2cpp", "collectibles-current-map-v1"});
        if (!parsed) return Error("compatibility.profile.json: " + parsed.error());
        profile_ = std::move(parsed).value();
        for (std::size_t i{}; i < std::size(kShapes); ++i) {
            if (!kShapes[i].symbol) continue;
            const auto entry = ResolveProfileSymbolRva(core_, profile_, kShapes[i].symbol);
            if (!entry) return Error(std::string(kShapes[i].klass) + '.' + kShapes[i].method + " profile: " + entry.error);
            entries_[i] = entry.address;
        }
        if (!receipt_.Bind(view, profile_, core_)) return Error("native reward response observer profile unavailable");
        if (!receipt_.CheckProof(profile_, "nativeChestAction")) return Error("native immediate chest callback proof unavailable");
        if (!receipt_.CheckProof(profile_, "puzzleGateStateRoute")) return Error("native puzzle state-output route proof unavailable");
        const auto& route = profile_.symbol("puzzleGateStateRoute").candidates.front().values;
        if (moduleBase_ + route.at("rva") != entries_[static_cast<std::size_t>(Call::GateInput)] + route.at("ownerOffset"))
            return Error("native puzzle state-output route owner mismatch");
        available_ = true;
        return true;
    }
    bool Available() const noexcept { return available_; }
    bool Resolve(const Runtime& runtime) {
        if (ready_) return true;
        if (!available_) return false;
        const auto& api = runtime.Functions();
        for (std::size_t i{}; i < std::size(kShapes); ++i) {
            const auto& shape = kShapes[i];
            std::vector<CabbirdStringViewV1> parameters;
            for (const auto* parameter : shape.parameters) parameters.push_back(StringView(parameter));
            CabbirdUnityMethodRequestV1 request{sizeof(request)};
            request.method_kind = shape.isStatic ? CABBIRD_UNITY_METHOD_V1_STATIC : CABBIRD_UNITY_METHOD_V1_INSTANCE;
            request.module_name = StringView("GameAssembly.dll");
            const auto name = std::string(shape.namespaze) + '.' + shape.klass;
            request.type_name = StringView(name); request.method_name = StringView(shape.method);
            request.return_type = StringView(shape.result); request.parameter_types = parameters.data();
            request.parameter_count = parameters.size();
            CabbirdUnityMethodResultV1 result{sizeof(result)};
            const auto status = metadata_->resolve_method(metadata_->user, &request, &result);
            if (!Succeeded(status) || (shape.symbol && entries_[i] != result.address)) {
                const std::string statusMessage = status.message.data && status.message.size ? std::string(status.message.data, status.message.size) : std::string{};
                std::ostringstream detail;
                detail << name << '.' << shape.method << " status=" << status.code << " actualRva=0x" << std::hex << result.rva;
                if (shape.symbol) detail << " expectedRva=0x" << entries_[i] - moduleBase_;
                if (!statusMessage.empty()) detail << ' ' << statusMessage;
                if (!Succeeded(status)) detail << DescribeNamedMethods(runtime, shape);
                else detail << " pinMismatch";
                return Error(detail.str());
            }
            auto* klass = runtime.FindClassAnyImage(shape.namespaze, shape.klass);
            const auto* method = klass ? runtime.FindMethodBySignature(klass, shape.method, shape.parameters) : nullptr;
            std::uint32_t implementation{};
            if (!method || runtime.TypeName(api.il2cpp_method_get_return_type(method)) != shape.result ||
                (((api.il2cpp_method_get_flags(method, &implementation) & 0x10u) != 0) != shape.isStatic)) {
                std::ostringstream detail; detail << name << '.' << shape.method << " runtime shape mismatch actualRva=0x" << std::hex << result.rva;
                return Error(detail.str());
            }
            methods_[i] = method;
        }
        if (!BindField(runtime, "Azur.Gameplay.World.Service", "SceneService", "map", kMap, map_) ||
            !BindField(runtime, "Azur.Gameplay.World.Service.Map", "MapService", "m_Local", "Azur.Gameplay.World.Service.Map.LocalMap", local_) ||
            !BindField(runtime, "Azur.Gameplay.World.Service.Map", "MapService", "m_Server", "Azur.Gameplay.World.Service.Map.ServerMap", server_) ||
            !BindField(runtime, "Azur.Gameplay.Units.World", "WorldUnit", "ObjectType", kObjectType, unitObjectType_, sizeof(std::int32_t)) ||
            !BindField(runtime, "Azur.Gameplay.Units.World", "WorldUnit", "m_SceneComponent", "Azur.Gameplay.Units.World.SceneComponent", unitScene_) ||
            !BindField(runtime, "Azur.Gameplay.World.WorldInteract", "WorldInteractManager", "m_AllWorldInteracts",
                "System.Collections.Generic.Dictionary<System.Int32,Azur.Gameplay.World.WorldInteract.WorldInteractObject>", interactions_) ||
            !BindField(runtime, "Azur.Gameplay.World.WorldInteract", "WorldInteractHost", "m_Go", "UnityEngine.GameObject", hostGo_) ||
            !BindField(runtime, "Azur.Gameplay.Graph.Blueprint", "PuzzleSetStateNode", "m_IsFinishPuzzle",
                "Lens.Gameplay.Modules.BigWorld.FlowCanvas.ValueInput<System.Boolean>", puzzleFinish_) ||
            !BindField(runtime, "Azur.Gameplay.Graph.Blueprint", "InitPuzzleStateCheck", "stateTypes",
                "System.Collections.Generic.List<Lens.Gameplay.Modules.BigWorld.EWorldObjectStateType>", puzzleStates_) ||
            !BindField(runtime, "Azur.Gameplay.Graph.Blueprint", "InitPuzzleStateCheck", "m_Outputs",
                "System.Collections.Generic.List<Lens.Gameplay.Modules.BigWorld.FlowCanvas.FlowOutput>", puzzleOutputs_) ||
            !BindField(runtime, "Azur.Gameplay.Graph.Blueprint", "InitPuzzleStateCheck", "m_entityInput",
                "Lens.Gameplay.Modules.BigWorld.EntityInput<Lens.Gameplay.Modules.BigWorld.EntityHandle>", puzzleEntityInput_) ||
            !BindField(runtime, "Lens.Gameplay.Modules.BigWorld.FlowCanvas", "FlowOutput", "TargetFlowInput",
                "Lens.Gameplay.Modules.BigWorld.FlowCanvas.FlowInput", puzzleOutputTarget_) ||
            !BindField(runtime, "Azur.Gameplay.World.WorldInteract", "WorldInteractActionBase", "m_InteractAnimProcessID", "System.Int32", interactionProcess_, sizeof(std::int32_t)) ||
            !BindField(runtime, "Azur.Gameplay.World.WorldInteract", "WorldInteractActionBase", "m_InteractionActionVersion", "System.Int32", interactionVersion_, sizeof(std::int32_t))) return false;
        puzzleGate_ = runtime.FindClassAnyImage("Azur.Gameplay.Graph.Blueprint", "InitPuzzleStateCheck");
        auto* stateType = runtime.FindClassAnyImage("Lens.Gameplay.Modules.BigWorld", "EWorldObjectStateType");
        auto* readyField = stateType ? runtime.FindField(stateType, "OverAndNotReward") : nullptr;
        if (!puzzleGate_ || !readyField || (api.il2cpp_field_get_flags(readyField) & 0x40u) == 0 ||
            runtime.TypeName(api.il2cpp_field_get_type(readyField)) != "Lens.Gameplay.Modules.BigWorld.EWorldObjectStateType")
            return Error("native puzzle unclaimed-reward state literal unavailable");
        api.il2cpp_field_static_get_value(readyField, &puzzleReadyState_);
        legacyPuzzleNode_ = runtime.FindClassAnyImage("Azur.Gameplay.Graph.Blueprint", "SetPuzzleState");
        puzzleNode_ = runtime.FindClassAnyImage("Azur.Gameplay.Graph.Blueprint", "PuzzleSetStateNode");
        graphAssignable_ = runtime.FindClassAnyImage("Lens.Gameplay.Modules.BigWorld.NodeCanvas.Framework", "IGraphAssignable");
        if (!puzzleNode_ || !legacyPuzzleNode_ || !graphAssignable_) return Error("native puzzle completion node types unavailable");
        worldInstance_ = runtime.FindClassAnyImage("Azur.Gameplay.Units.World", "WorldInstance");
        if (!worldInstance_ || !api.il2cpp_type_get_object || !api.il2cpp_string_new)
            return Error("native interaction host types or enum APIs unavailable");
        auto* reasonType = api.il2cpp_type_get_object(api.il2cpp_method_get_param(Get(Call::NativeChestClick), 0));
        auto* reasonName = api.il2cpp_string_new("BtnClick");
        void* reasonArgs[]{reasonType, reasonName};
        if (!reasonType || !reasonName || !Value(runtime, Get(Call::EnumParse), nullptr, reasonArgs, btnClick_))
            return Error("native InteractReason.BtnClick enum unavailable");
        auto* kind = runtime.FindClassAnyImage("Lens.Gameplay.Modules.Azur", "EWorldObjectType");
        if (!kind) return Error(std::string(kObjectType) + " class unavailable");
        chests_.clear(); collections_.clear(); actions_.clear(); chestActions_.clear();
        const auto add = [&](const char* name, std::vector<ScanType>& destination, bool chest) {
            auto* field = runtime.FindField(kind, name);
            if (!field || runtime.TypeName(api.il2cpp_field_get_type(field)) != kObjectType ||
                (api.il2cpp_field_get_flags(field) & 0x40u) == 0) return Error(std::string(kObjectType) + '.' + name + " literal unavailable");
            std::int32_t value{}; api.il2cpp_field_static_get_value(field, &value);
            destination.push_back({value, chest, name}); return true;
        };
        // Enum values come from the runtime's literal metadata, never guessed
        // numeric values or compatibility offsets embedded in plugin code.
        for (const auto* name : {"Chest", "ChestWooden", "ChestSilver", "ChestIron", "ChestGold", "PuzzleChestItem"})
            if (!add(name, chests_, true)) return false;
        for (const auto* name : {"Item", "BreakableItem", "PlayableBreakableItem", "Oculus", "KiboEgg"})
            if (!add(name, collections_, false)) return false;
        auto* completeState = runtime.FindClassAnyImage("Azur.Gameplay.World.Service.Map", "EObjectServerCompleteState");
        if (!completeState) return Error("Azur.Gameplay.World.Service.Map.EObjectServerCompleteState class unavailable");
        auto* completeField = runtime.FindField(completeState, "Complete");
        auto* incompleteField = runtime.FindField(completeState, "UnComplete");
        auto* noDataField = runtime.FindField(completeState, "NoData");
        if (!completeField || !incompleteField || !noDataField ||
            (api.il2cpp_field_get_flags(completeField) & 0x40u) == 0 ||
            (api.il2cpp_field_get_flags(incompleteField) & 0x40u) == 0 ||
            (api.il2cpp_field_get_flags(noDataField) & 0x40u) == 0) return Error("EObjectServerCompleteState.Complete/UnComplete/NoData literal unavailable");
        api.il2cpp_field_static_get_value(completeField, &complete_);
        api.il2cpp_field_static_get_value(incompleteField, &incomplete_);
        api.il2cpp_field_static_get_value(noDataField, &noData_);
        for (const auto* name : {"SimpleCollectionAction", "PickUpBookAction", "PickupAstraxiomAction"}) {
            auto* klass = runtime.FindClassAnyImage("Azur.Gameplay.World.WorldInteract", name);
            if (!klass) return Error(std::string("Azur.Gameplay.World.WorldInteract.") + name + " class unavailable");
            actions_.push_back(klass);
        }
        for (const auto* name : {"DirectSendMessageAction", "SetWorldObjComplete"}) {
            auto* klass = runtime.FindClassAnyImage("Azur.Gameplay.World.WorldInteract", name);
            if (!klass) return Error(std::string("Azur.Gameplay.World.WorldInteract.") + name + " class unavailable");
            chestActions_.push_back(klass);
        }
        defaultChestAction_ = runtime.FindClassAnyImage("Azur.Gameplay.World.WorldInteract", "DefaultWorldInteractAction");
        chestTipsAction_ = runtime.FindClassAnyImage("Azur.Gameplay.World.WorldInteract", "ChestTipsAction");
        if (!defaultChestAction_ || !chestTipsAction_) return Error("DefaultWorldInteractAction/ChestTipsAction class unavailable");
        if (!receipt_.Validate(runtime, metadata_) || !receipt_.Install(entries_[static_cast<std::size_t>(Call::Response)])) return Error("native reward response observer unavailable");
        ready_ = true;
        catalog_.Bind({Get(Call::CategoryId), Get(Call::CategoryName), Get(Call::CategoryShow),
            Get(Call::CollectId), Get(Call::CollectType), Get(Call::CollectName), Get(Call::CollectSpawners),
            Get(Call::LangValue), Get(Call::TableReady)});
        failure_.clear();
        return true;
    }
    bool BeginScan(const Runtime& runtime, bool chests, bool collections) {
        Object *world{}, *map{}, *local{}, *manager{};
        if (!WorldState(runtime, world, map, local, manager)) return false;
        std::int32_t worldId{};
        if (!Value(runtime, Get(Call::WorldId), world, nullptr, worldId) || !worldId) return Error("AzurWorld.get_CurWorldId unavailable");
        worldId_ = worldId;
        scannedMap_ = reinterpret_cast<std::uintptr_t>(map);
        scannedLocal_ = reinterpret_cast<std::uintptr_t>(local);
        scanKinds_.clear(); scanSeen_.clear(); scanKind_ = 0; scanIndex_ = 0;
        if (chests) scanKinds_.insert(scanKinds_.end(), chests_.begin(), chests_.end());
        if (collections) scanKinds_.insert(scanKinds_.end(), collections_.begin(), collections_.end());
        stats_.kinds = scanKinds_;
        collectCatalog_ = collections;
        catalog_.Begin();
        scanChests_ = chests;
        loadedPickups_.clear(); loadedChests_.clear(); loadedChestIndex_.clear(); loadedChestMerge_ = 0;
        loadedStarted_ = loadedDone_ = false; loadedInspected_ = loadedIndex_ = 0; loadedFailure_.clear();
        return true;
    }
    ScanWorld CheckScanWorld(const Runtime& runtime) const {
        Object *world{}, *map{}, *local{}, *manager{};
        if (!WorldState(runtime, world, map, local, manager)) return ScanWorld::Unavailable;
        std::int32_t current{};
        if (!Value(runtime, Get(Call::WorldId), world, nullptr, current) || !current) {
            Error("AzurWorld.get_CurWorldId unavailable"); return ScanWorld::Unavailable;
        }
        if (current == worldId_ && reinterpret_cast<std::uintptr_t>(map) == scannedMap_ &&
            reinterpret_cast<std::uintptr_t>(local) == scannedLocal_) return ScanWorld::Current;
        Error("scanned map changed: scannedWorldId=" + std::to_string(worldId_) + " currentWorldId=" + std::to_string(current) +
            " mapChanged=" + std::to_string(reinterpret_cast<std::uintptr_t>(map) != scannedMap_) +
            " localMapChanged=" + std::to_string(reinterpret_cast<std::uintptr_t>(local) != scannedLocal_));
        return ScanWorld::Changed;
    }
    bool RefreshArrival(const Runtime& runtime, const Target& target) const {
        Object *aoi{}, *ignored{};
        if (!Invoke(runtime, Get(Call::Aoi), nullptr, nullptr, aoi) || !aoi) return Error("AzurWorld.get_AOI unavailable");
        // WorldMapSPC.OnMyPlayerPosUpdate and the native transfer loading task
        // submit this same one-shot world-space probe. It requests area loading;
        // it does not change the player's position or interaction eligibility.
        Vector3 position{static_cast<float>(target.position[0]), static_cast<float>(target.position[1]), static_cast<float>(target.position[2])};
        void* parameters[]{&position};
        return Invoke(runtime, Get(Call::UpdateAoi), aoi, parameters, ignored) || Error("WorldAOIMgr.UpdateAOI arrival notification failed");
    }
    ScanState ScanStep(const Runtime& runtime, std::vector<Target>& targets) {
        Object *world{}, *map{}, *local{}, *manager{};
        if (!WorldState(runtime, world, map, local, manager)) return ScanState::Unavailable;
        std::int32_t worldId{};
        if (!Value(runtime, Get(Call::WorldId), world, nullptr, worldId) || worldId != worldId_) return ScanError("AzurWorld.get_CurWorldId changed during scan");
        if (collectCatalog_ && catalog_.Pending()) { catalog_.Step(runtime); return ScanState::Pending; }
        if ((collectCatalog_ || scanChests_) && !loadedDone_) { ScanLoadedPickups(runtime, manager); return ScanState::Pending; }
        std::uint32_t budget = 32;
        while (scanKind_ < scanKinds_.size() && budget) {
            const auto type = scanKinds_[scanKind_];
            auto kind = type.value;
            void* parameters[]{&kind}; Object* ids{};
            if (!Invoke(runtime, Get(Call::Objects), local, parameters, ids)) return ScanError(std::string("LocalMap.GetObjectsIdByType ") + type.name + " invocation failed");
            if (!ids) { ++scanKind_; scanIndex_ = 0; continue; }
            Root root(runtime, ids);
            const auto* countMethod = Member(runtime, ids, "get_Count", {}, "System.Int32");
            const auto* itemMethod = Member(runtime, ids, "get_Item", {"System.Int32"}, "System.Int32");
            std::int32_t count{};
            if (!root.get() || !Value(runtime, countMethod, root.get(), nullptr, count) || count < 0) return ScanError(std::string("LocalMap.GetObjectsIdByType ") + type.name + " list.get_Count failed");
            stats_.kinds[scanKind_].count = static_cast<std::uint32_t>(count);
            while (scanIndex_ < count && budget) {
                auto index = scanIndex_++; --budget;
                std::int32_t id{}; void* indexArgs[]{&index};
                if (!Value(runtime, itemMethod, root.get(), indexArgs, id) || id <= 0) return ScanError(std::string("LocalMap.GetObjectsIdByType ") + type.name + " list.get_Item failed");
                if (!scanSeen_.insert(id).second) { ++stats_.kinds[scanKind_].duplicates; continue; }
                Target target; target.mapId = id; target.chest = type.chest; target.kind = type.name;
                if (!type.chest) {
                    Object* row{}; void* rowArgs[]{&id, &worldId};
                    if (!Invoke(runtime, Get(Call::MapRow), nullptr, rowArgs, row) || !row) return ScanError("TDWorldmapTable.GetData current-map invocation failed");
                    Root rowRoot(runtime, row); std::int32_t spawner{};
                    if (!StructValue(runtime, Get(Call::MapSpawner), rowRoot.get(), spawner)) return ScanError("TDWorldmap.get_spawnerId invocation failed");
                    const auto* entry = catalog_.Find(spawner);
                    const auto native = loadedPickups_.find(id);
                    if (!ClassifyTarget(target, entry, native == loadedPickups_.end() ? PickupKind::None : native->second)) {
                        ++stats_.excludedUnclassified; continue;
                    }
                    if (target.official) { ++stats_.officialMatches; ++stats_.categories[target.categoryId]; }
                    else if (target.nativePickup != PickupKind::None) ++stats_.nativeMatches;
                }
                const auto completion = Completion(runtime, target);
                if (completion == CompletionState::Unavailable) return ScanError("ServerMap.GetObjectServerCompleteState unavailable");
                if (completion == CompletionState::Complete) {
                    ++stats_.kinds[scanKind_].complete;
                    // Chest lifecycle completion can coexist with a visible,
                    // unopened reward action. Keep it until the exact native
                    // action and this request's reward response are checked.
                    if (!target.chest) { ++stats_.completeExcluded; continue; }
                }
                if (completion == CompletionState::Unknown) ++stats_.kinds[scanKind_].unknown;
                target.completion = completion;
                const auto live = loadedChestIndex_.find(id);
                if (target.chest && live != loadedChestIndex_.end()) {
                    target.position = loadedChests_[live->second].position; target.livePosition = true;
                    target.kind = loadedChests_[live->second].kind;
                } else {
                    Vector3 position{}; bool found{}; void* positionArgs[]{&id, &position};
                    if (!Value(runtime, Get(Call::Position), manager, positionArgs, found)) return ScanError("WorldMapMgr.GetWorldMapCreatePos invocation failed");
                    if (!found) continue;
                    target.position = {position.x, position.y, position.z};
                }
                if (std::all_of(target.position.begin(), target.position.end(), [](double v) { return std::isfinite(v) && std::abs(v) <= 1e6; })) {
                    ++stats_.positionSuccess;
                    ++stats_.kinds[scanKind_].positions;
                    if (completion == CompletionState::Unknown) ++stats_.unknownRetained;
                    targets.push_back(std::move(target));
                }
            }
            if (scanIndex_ >= count) { ++scanKind_; scanIndex_ = 0; }
        }
        if (scanKind_ != scanKinds_.size()) return ScanState::Pending;
        for (; loadedChestMerge_ < loadedChests_.size() && budget; --budget, ++loadedChestMerge_) {
            auto target = loadedChests_[loadedChestMerge_];
            if (!scanSeen_.insert(target.mapId).second) continue;
            target.completion = Completion(runtime, target);
            if (target.completion == CompletionState::Unavailable) return ScanError("loaded chest server state unavailable");
            if (target.completion == CompletionState::Unknown) ++stats_.unknownRetained;
            targets.push_back(std::move(target)); ++stats_.loadedChestAdded; ++stats_.positionSuccess;
        }
        if (loadedChestMerge_ != loadedChests_.size()) return ScanState::Pending;
        scanKinds_.clear(); scanSeen_.clear();
        return ScanState::Complete;
    }
    bool RewardConfirmed() const { return receipt_.Confirmed(); }
    std::string RewardDetails() const { return receipt_.Details(); }
    bool StopReceipt() { return receipt_.Stop(); }
    CompletionState Completion(const Runtime& runtime, const Target& target, bool* transient = nullptr) const {
        if (transient) *transient = false;
        Object *world{}, *map{}, *local{}, *manager{};
        if (!WorldState(runtime, world, map, local, manager)) return CompletionState::Unavailable;
        std::int32_t current{};
        if (!Value(runtime, Get(Call::WorldId), world, nullptr, current) || current != worldId_) return CompletionState::Unavailable;
        const auto state = ServerCompletion(runtime, map, target.mapId);
        if (state == CompletionState::Unavailable && transient) *transient = true;
        return state;
    }
    TargetPresence ProbeTarget(const Runtime& runtime, Target& target) const {
        Object *world{}, *map{}, *local{}, *manager{};
        if (!WorldState(runtime, world, map, local, manager)) return {};
        std::int32_t current{}, id = target.mapId;
        if (!Value(runtime, Get(Call::WorldId), world, nullptr, current) || current != worldId_) return {};
        bool noData{};
        TargetPresence result{ServerCompletion(runtime, map, id, &noData)};
        const auto transientFailure = [&] {
            result.transient = true;
            return result;
        };
        if (result.completion == CompletionState::Unavailable) return transientFailure();
        Object *unit{}, *instance{}; bool active{}, disposed{}, hierarchy{}, logic{}, view{};
        void* parameters[]{&id};
        if (!Value(runtime, Get(Call::ObjectActive), map, parameters, active) ||
            !Invoke(runtime, Get(Call::MapUnit), manager, parameters, unit)) return transientFailure();
        if (unit) {
            std::int32_t actual{};
            if (!Value(runtime, Get(Call::UnitMapId), unit, nullptr, actual) || actual != id ||
                !Value(runtime, Get(Call::UnitDisposed), unit, nullptr, disposed)) return transientFailure();
            if (!disposed && !Invoke(runtime, Get(Call::UnitInstance), unit, nullptr, instance)) return transientFailure();
            if (!disposed) {
                if (!Value(runtime, Get(Call::UnitLogicActive), unit, nullptr, logic) ||
                    !Value(runtime, Get(Call::UnitViewActive), unit, nullptr, view)) return transientFailure();
                Object* scene{}; runtime.Functions().il2cpp_field_get_value(unit, unitScene_, &scene);
                if (scene) {
                    Object *transform{}, *gameObject{}; bool found{}; void* transformArgs[]{&transform};
                    if (!Value(runtime, Get(Call::SceneTransform), scene, transformArgs, found)) return transientFailure();
                    if (found && transform) {
                        if (!Invoke(runtime, Get(Call::ComponentObject), transform, nullptr, gameObject) || !gameObject ||
                            !Value(runtime, Get(Call::ObjectHierarchyActive), gameObject, nullptr, hierarchy)) return transientFailure();
                        if (hierarchy && view) {
                            Vector3 position{}; void* positionArgs[]{&position};
                            if (!Value(runtime, Get(Call::ScenePosition), scene, positionArgs, found)) return transientFailure();
                            if (found && ValidPosition(position)) { target.position = {position.x, position.y, position.z}; target.livePosition = true; }
                        }
                    }
                }
            }
        }
        // This is the game's actual WorldMapMgr creation gate, independent of
        // object-complete/reward semantics. An active exact view still wins;
        // logical activation is independent of visibility and may occur later.
        // unknown server data may be waiting for its normal scene query.
        const bool known = result.completion != CompletionState::Unknown || noData;
        const bool body = hierarchy && view;
        result.presence = !active && !body && known ? PresenceState::NotSpawned : PresenceState::MayLoad;
        if (result.presence == PresenceState::NotSpawned) {
            std::ostringstream details;
            details << "mapId=" << id << " nativeActive=0 worldUnit=" << (unit != nullptr)
                << " unitDisposed=" << disposed << " worldInstance=" << (instance != nullptr)
                << " hierarchyActive=" << hierarchy << " logicActive=" << logic << " viewActive=" << view
                << " serverStateKnown=1 serverNoData=" << noData
                << " reason=native creation gate is inactive and no exact active body exists; reward remains unconfirmed";
            interactionDetails_ = details.str();
        }
        return result;
    }
private:
    CompletionState ServerCompletion(const Runtime& runtime, Object* map, std::int32_t id, bool* noData = nullptr) const {
        if (noData) *noData = false;
        Object* server{};
        runtime.Functions().il2cpp_field_get_value(map, server_, &server);
        if (!server) return CompletionState::Unknown;
        std::int32_t result{};
        void* parameters[]{&id};
        // MapService.GetObjectCompleteState folds TDWorldmap.InitialComplete
        // into Unknown/NoData. That configuration default is not a server
        // completion and must neither exclude a target nor bypass its action.
        if (!Value(runtime, Get(Call::Completion), server, parameters, result)) return CompletionState::Unavailable;
        if (noData) *noData = result == noData_;
        if (result == complete_) return CompletionState::Complete;
        return result == incomplete_ ? CompletionState::Incomplete : CompletionState::Unknown;
    }
public:
    InteractionState Interact(const Runtime& runtime, const Target& target) const {
        interactionDetails_.clear();
        std::ostringstream details;
        details << "mapId=" << target.mapId;
        const auto finish = [&](InteractionState state, std::string_view reason) {
            details << " result=" << reason;
            interactionDetails_ = details.str();
            return state;
        };
        if (target.kind == "PuzzleChestItem" && puzzleMap_ == target.mapId && puzzleGraphs_)
            return finish(FinishPuzzle(runtime, target.mapId, details), "native puzzle completion flow");
        Object* manager{};
        if (!Invoke(runtime, Get(Call::InteractManager), nullptr, nullptr, manager) || !manager)
            return finish(InteractionState::Unavailable, "WorldInteractManager unavailable");
        Object* dictionary{}; runtime.Functions().il2cpp_field_get_value(manager, interactions_, &dictionary);
        if (!dictionary) return finish(InteractionState::Waiting, "interaction registry not ready");
        Object* values{};
        if (!Invoke(runtime, Member(runtime, dictionary, "get_Values", {}), dictionary, nullptr, values) || !values)
            return finish(InteractionState::Unavailable, "registry.get_Values failed");
        Root valuesRoot(runtime, values); Object* enumerator{};
        if (!valuesRoot.get() || !Invoke(runtime, Member(runtime, valuesRoot.get(), "GetEnumerator", {}), valuesRoot.get(), nullptr, enumerator) || !enumerator)
            return finish(InteractionState::Unavailable, "registry.GetEnumerator failed");
        Root enumeration(runtime, enumerator);
        const auto* next = Member(runtime, enumerator, "MoveNext", {}, "System.Boolean");
        const auto* current = Member(runtime, enumerator, "get_Current", {}, kInteract);
        if (!next || !current || !enumeration.get()) return finish(InteractionState::Unavailable, "registry enumerator shape unavailable");
        bool unsupported{}, waiting{}, consumption{};
        std::uint32_t registered{}, entityHosts{}, targetMatches{};
        for (;;) {
            bool more{};
            auto* storage = reinterpret_cast<Object*>(runtime.Functions().il2cpp_object_unbox(enumeration.get()));
            if (!storage || !Value(runtime, next, storage, nullptr, more)) return finish(InteractionState::Unavailable, "registry.MoveNext failed");
            if (!more) {
                details << " registered=" << registered << " entityHosts=" << entityHosts << " targetMatches=" << targetMatches;
                // A puzzle owner may only expose a discovery-voice action. The
                // configured graph finish, rather than that voice event, owns
                // puzzle synchronization and the subsequent reward flow.
                if (target.kind == "PuzzleChestItem" && !consumption)
                    return finish(FinishPuzzle(runtime, target.mapId, details), "native puzzle completion flow");
                return finish(unsupported && !waiting ? InteractionState::Unsupported : InteractionState::Waiting,
                    unsupported && !waiting ? "no supported ready action" : "waiting for native pickup readiness");
            }
            storage = reinterpret_cast<Object*>(runtime.Functions().il2cpp_object_unbox(enumeration.get()));
            Object* interaction{};
            if (!Invoke(runtime, current, storage, nullptr, interaction) || !interaction) return finish(InteractionState::Unavailable, "registry.get_Current failed");
            ++registered;
            Object* host{}; std::int32_t id{}; std::string_view identity;
            if (!Invoke(runtime, Get(Call::Host), interaction, nullptr, host) || !host) continue;
            if (!HostMapId(runtime, host, id, identity)) return finish(InteractionState::Unavailable, "native host map identity query failed");
            if (identity == "entity") ++entityHosts;
            if (id != target.mapId) continue;
            ++targetMatches;
            details << " hostIdentity=" << identity;
            Object* action{};
            if (!Invoke(runtime, Get(Call::Action), interaction, nullptr, action) || !action) continue;
            auto* klass = runtime.Functions().il2cpp_object_get_class(action);
            const bool collection = std::find(actions_.begin(), actions_.end(), klass) != actions_.end();
            const bool chest = target.chest && (klass == defaultChestAction_ ||
                std::find(chestActions_.begin(), chestActions_.end(), klass) != chestActions_.end());
            const auto& api = runtime.Functions();
            const auto actionName = runtime.TypeName(api.il2cpp_class_get_type(klass));
            details << " action=" << actionName;
            bool enabled{}, canTrigger{};
            if (!Value(runtime, Get(Call::Enabled), interaction, nullptr, enabled) || !Value(runtime, Get(Call::CanTrigger), interaction, nullptr, canTrigger))
                return finish(InteractionState::Unavailable, "native readiness query failed");
            details << " enabled=" << enabled << " canTrigger=" << canTrigger;
            // ChestTips is an AOI/screen hint, not the chest's button action. It
            // can be registered before the graph installs the actual pickup.
            if (klass == chestTipsAction_) { details << " reason=chest hint only"; continue; }
            if (!enabled && !chest) { waiting = true; details << " reason=native action not enabled"; continue; }
            if (!collection && !chest) {
                unsupported = true;
                if (actionName.ends_with(".RandomChestAction")) { consumption = true; details << " reason=random chest reward requires user confirmation"; }
                else if (actionName.ends_with(".RepairWorldItemAction") || actionName.ends_with(".ChangeUnitStateAction")) { consumption = true; details << " reason=repair or item consumption requires user confirmation"; }
                else if (actionName.ends_with(".ChestEnemyAction") || actionName.ends_with(".DungeonChestAction") || actionName.ends_with(".SuYaDungeonChestAction") || actionName.ends_with(".WorldBossChestAction"))
                    details << " reason=combat or dungeon reward action is not automatic";
                else details << " reason=action has no supported automatic pickup contract";
                continue;
            }
            // Default actions carry the graph's SetInteractionAction callback.
            // The native button path invokes that callback through the base
            // action; DefaultWorldInteractAction.OnTriggerInteract alone is not
            // the pickup implementation.
            if (klass == defaultChestAction_) {
                bool hasAction{};
                if (!Value(runtime, Get(Call::HasInteractionAction), interaction, nullptr, hasAction))
                    return finish(InteractionState::Unavailable, "native graph callback query failed");
                if (!hasAction) { waiting = true; details << " reason=chest graph callback not bound"; continue; }
                std::int32_t process{}, version{};
                api.il2cpp_field_get_value(action, interactionProcess_, &process);
                api.il2cpp_field_get_value(action, interactionVersion_, &version);
                details << " animProcess=" << process << " actionVersion=" << version;
                if (process != -1) return finish(InteractionState::Waiting, "native interaction animation process is already active");
                // InteractActionInvoke uses this same version-checked callback
                // immediately in its mounted branch. Select it before starting
                // any presentation process, so no animation can call it twice.
                receipt_.Arm(target.mapId);
                void* invokeArgs[]{&version}; Object* ignored{};
                if (!Invoke(runtime, Get(Call::ImmediateChestAction), action, invokeArgs, ignored))
                    return finish(InteractionState::Unavailable, "native version-checked chest callback failed");
                return finish(InteractionState::Accepted, "native version-checked chest callback dispatched; awaiting completed target reward response");
            }
            std::int32_t sid{};
            if (!Value(runtime, Get(Call::Sid), interaction, nullptr, sid)) return finish(InteractionState::Unavailable, "WorldInteractObject.GetSID failed");
            details << " sid=" << sid;
            bool accepted{}; void* parameters[]{&sid};
            // CanTrigger is the previous interaction update's cached result.
            // The native button path refreshes it before enforcing readiness.
            receipt_.Arm(target.mapId);
            if (enabled && !Value(runtime, Get(Call::Trigger), manager, parameters, accepted)) return finish(InteractionState::Unavailable, "native TryTriggerButtonClick failed");
            if (!accepted && chest) {
                // Direct opening is authorized for ordinary/puzzle chests. Use
                // the same native action event as a successful button click;
                // retain the action invoker and graph callback's own contracts.
                // No readiness/completion field is changed, and cost/reward
                // actions outside the existing whitelist never reach this call.
                Object* ignored{}; auto reason = btnClick_; void* clickArgs[]{&reason};
                if (!Invoke(runtime, Get(Call::NativeChestClick), interaction, clickArgs, ignored))
                    return finish(InteractionState::Unavailable, "native chest BtnClick dispatch failed");
                return finish(InteractionState::Accepted, "native chest BtnClick dispatched; awaiting target reward response");
            }
            return finish(accepted ? InteractionState::Accepted : InteractionState::Waiting,
                accepted ? "native button accepted; awaiting target reward response" : "native button declined");
        }
    }
private:
    bool ReadyPuzzleOutput(const Runtime& runtime, Object* node, std::int32_t mapId, Object*& output, std::ostringstream& details) const {
        const auto& api = runtime.Functions(); output = nullptr;
        Object *input{}, *entity{}, *target{}, *states{}, *outputs{};
        api.il2cpp_field_get_value(node, puzzleEntityInput_, &input);
        if (!input || !Invoke(runtime, Member(runtime, input, "get_value", {}, "Lens.Gameplay.Modules.BigWorld.EntityHandle"), input, nullptr, entity) ||
            !Invoke(runtime, Member(runtime, node, "get_graphTarget", {}, "Lens.Gameplay.Modules.BigWorld.IGraphTarget"), node, nullptr, target)) return false;
        std::int32_t actual{}; void* identity[]{entity, target};
        if (!Value(runtime, Get(Call::GraphMapId), nullptr, identity, actual)) return false;
        if (actual != mapId) { details << " gateMapId=" << actual; return true; }
        api.il2cpp_field_get_value(node, puzzleStates_, &states); api.il2cpp_field_get_value(node, puzzleOutputs_, &outputs);
        std::int32_t count{}, outputCount{};
        if (!states || !outputs || !Value(runtime, Member(runtime, states, "get_Count", {}, "System.Int32"), states, nullptr, count) ||
            !Value(runtime, Member(runtime, outputs, "get_Count", {}, "System.Int32"), outputs, nullptr, outputCount) || count < 0 || count > 32 || outputCount != count) return false;
        std::int32_t selected = -1;
        details << " gateStates=[";
        for (std::int32_t i{}; i<count; ++i) {
            std::int32_t state{}; void* parameters[]{&i};
            if (!Value(runtime, Member(runtime, states, "get_Item", {"System.Int32"}, "Lens.Gameplay.Modules.BigWorld.EWorldObjectStateType"), states, parameters, state)) return false;
            details << state << ',';
            if (state == puzzleReadyState_) { if (selected != -1) return false; selected = i; }
        }
        details << "] readyState=" << puzzleReadyState_;
        if (selected == -1) {
            // An ordinary configured step has no universal reward meaning.
            // Inspect only its single real bound input for the terminal
            // diagnostic; never execute a guessed step or completion output.
            if (count == 1) {
                std::int32_t index{}; void* parameters[]{&index}; Object *configured{}, *inputTarget{}, *parent{};
                if (!Invoke(runtime, Member(runtime, outputs, "get_Item", {"System.Int32"}, "Lens.Gameplay.Modules.BigWorld.FlowCanvas.FlowOutput"), outputs, parameters, configured) || !configured) return false;
                api.il2cpp_field_get_value(configured, puzzleOutputTarget_, &inputTarget);
                if (inputTarget) {
                    if (!Invoke(runtime, Get(Call::PortParent), inputTarget, nullptr, parent) || !parent) return false;
                    details << " configuredStateTarget=" << runtime.TypeName(api.il2cpp_class_get_type(api.il2cpp_object_get_class(parent)));
                } else details << " configuredStateTarget=unbound";
            }
            return true;
        }
        void* parameters[]{&selected}; bool connected{};
        if (!Invoke(runtime, Member(runtime, outputs, "get_Item", {"System.Int32"}, "Lens.Gameplay.Modules.BigWorld.FlowCanvas.FlowOutput"), outputs, parameters, output) ||
            !output || !Value(runtime, Get(Call::PortConnected), output, nullptr, connected)) return false;
        if (!connected) output = nullptr;
        details << " readyPortConnected=" << connected;
        return true;
    }
    void EndPuzzle(const Runtime& runtime) const {
        for (auto handle : {puzzleGraphs_, puzzleNodes_, puzzleSelected_, puzzleSelectedGraph_, puzzleOwner_})
            if (handle) runtime.Functions().il2cpp_gchandle_free(handle);
        for (auto handle : puzzleChildren_) runtime.Functions().il2cpp_gchandle_free(handle);
        puzzleGraphs_ = puzzleNodes_ = puzzleSelected_ = puzzleSelectedGraph_ = puzzleOwner_ = nullptr;
        puzzleChildren_.clear(); puzzleVisited_.clear(); puzzleNodeTypes_.clear(); puzzleGateDetails_.clear(); puzzleChildIndex_ = 0;
        puzzleMap_ = 0; puzzleGraphIndex_ = puzzleNodeIndex_ = puzzleGraphMatches_ = puzzleFinishMatches_ = 0;
    }
    InteractionState FinishPuzzle(const Runtime& runtime, std::int32_t mapId, std::ostringstream& details) const {
        const auto& api = runtime.Functions();
        if (puzzlePreparedMap_ == mapId) { details << puzzlePreparedDetails_ << " reason=unclaimed-reward branch already prepared; awaiting native chest action"; return InteractionState::Waiting; }
        const auto finish = [&](InteractionState state, std::string_view reason) {
            details << " puzzleSnapshot=" << (puzzleGraphs_ ? api.il2cpp_array_length(api.il2cpp_gchandle_get_target(puzzleGraphs_)) : 0)
                << puzzleGateDetails_ << " puzzleGraphs=" << puzzleGraphMatches_ << " finishNodes=" << puzzleFinishMatches_ << " subgraphs=" << puzzleChildren_.size() << " reason=" << reason;
            if (state == InteractionState::Unsupported && puzzleFinishMatches_ == 0) { details << " nodeTypes=["; for (const auto& [name,count] : puzzleNodeTypes_) details << name << ':' << count << ','; details << ']'; }
            if (state != InteractionState::Discovering) EndPuzzle(runtime);
            return state;
        };
        const auto snapshot = [&](Object* list, const char* elementType, CabbirdIl2CppGCHandleV1& destination) {
            Object* array{};
            if (!list || !Invoke(runtime, Member(runtime, list, "ToArray", {}, elementType), list, nullptr, array) || !array) return false;
            destination = api.il2cpp_gchandle_new(array, false);
            return destination != nullptr;
        };
        const auto graphMatches = [&](Object* graph, bool& matches) {
            Object *target{}, *entity{};
            if (!Invoke(runtime, Get(Call::GraphTarget), graph, nullptr, target) ||
                !Invoke(runtime, Get(Call::GraphEntity), graph, nullptr, entity)) return false;
            if (!target && !entity) { matches = false; return true; }
            std::int32_t id{}; void* identityArgs[]{entity, target};
            if (!Value(runtime, Get(Call::GraphMapId), nullptr, identityArgs, id)) return false;
            if (id != mapId) { matches = false; return true; }
            bool running{}, paused{};
            if (!Value(runtime, Get(Call::GraphRunning), graph, nullptr, running) ||
                !Value(runtime, Get(Call::GraphPaused), graph, nullptr, paused)) return false;
            Object* binder{};
            if (running && !paused && !Invoke(runtime, Get(Call::GraphBinder), nullptr, identityArgs, binder)) return false;
            matches = running && !paused && binder;
            return true;
        };
        const auto isFinish = [&](Object* node, bool& complete) {
            if (api.il2cpp_class_is_assignable_from(puzzleGate_, api.il2cpp_object_get_class(node))) {
                Object* output{}; std::ostringstream gate;
                if (!ReadyPuzzleOutput(runtime, node, mapId, output, gate)) return false;
                puzzleGateDetails_ = gate.str();
                complete = output != nullptr; return true;
            }
            if (api.il2cpp_class_is_assignable_from(legacyPuzzleNode_, api.il2cpp_object_get_class(node))) { complete = true; return true; }
            Object* input{}; api.il2cpp_field_get_value(node, puzzleFinish_, &input);
            return input && Value(runtime, Member(runtime, input, "get_value", {}, "System.Boolean"), input, nullptr, complete);
        };
        if (puzzleMap_ != mapId) EndPuzzle(runtime);
        if (!puzzleGraphs_) {
            Object* graphs{};
            if (!Invoke(runtime, Get(Call::RunningGraphs), nullptr, nullptr, graphs) ||
                !snapshot(graphs, "Lens.Gameplay.Modules.BigWorld.NodeCanvas.Framework.Graph[]", puzzleGraphs_))
                return finish(InteractionState::Unavailable, "running puzzle graph snapshot unavailable");
            puzzleMap_ = mapId;
        }
        // Native List<Graph>.ToArray makes one stable managed-reference snapshot
        // during streaming. Discovery then visits at most 32 graph/node entries
        // and two milliseconds per Game callback. Only handles and indices cross
        // callbacks; pending discovery advances every frame within the route's
        // existing deadline. Only an exact target's node list is inspected.
        const auto started = std::chrono::steady_clock::now();
        for (std::uint32_t inspected{}; inspected < 32; ++inspected) {
            if (puzzleNodes_) {
                auto* nodes = api.il2cpp_gchandle_get_target(puzzleNodes_);
                if (puzzleNodeIndex_ == api.il2cpp_array_length(nodes)) {
                    api.il2cpp_gchandle_free(puzzleNodes_); puzzleNodes_ = nullptr; puzzleNodeIndex_ = 0;
                    api.il2cpp_gchandle_free(puzzleOwner_); puzzleOwner_ = nullptr;
                    continue;
                }
                Object* node{};
                if (!runtime.ArrayReferenceAt(nodes, puzzleNodeIndex_++, &node) || !node)
                    return finish(InteractionState::Unavailable, "puzzle node snapshot entry unavailable");
                auto* nodeClass = api.il2cpp_object_get_class(node);
                const auto nodeName = runtime.TypeName(api.il2cpp_class_get_type(nodeClass));
                if (puzzleNodeTypes_.size() < 24 || puzzleNodeTypes_.contains(nodeName)) ++puzzleNodeTypes_[nodeName];
                if (api.il2cpp_class_is_assignable_from(graphAssignable_, nodeClass)) {
                    const auto* current = Member(runtime, node, "get_currentInstance", {});
                    if (!current) current = Member(runtime, node, "Lens.Gameplay.Modules.BigWorld.NodeCanvas.Framework.IGraphAssignable.get_currentInstance", {});
                    Object* child{};
                    if (!current || !Invoke(runtime, current, node, nullptr, child)) return finish(InteractionState::Unavailable, "native instantiated subgraph query failed");
                    if (child && !puzzleVisited_.contains(reinterpret_cast<std::uintptr_t>(child))) {
                        if (puzzleChildren_.size() == 64) return finish(InteractionState::Unsupported, "target instantiated graph tree exceeds discovery budget");
                        const auto handle = api.il2cpp_gchandle_new(child, false);
                        if (!handle) return finish(InteractionState::Unavailable, "subgraph handle unavailable");
                        puzzleChildren_.push_back(handle);
                    }
                }
                if (api.il2cpp_class_is_assignable_from(puzzleGate_, nodeClass) || api.il2cpp_class_is_assignable_from(puzzleNode_, nodeClass) || api.il2cpp_class_is_assignable_from(legacyPuzzleNode_, api.il2cpp_object_get_class(node))) {
                    bool complete{};
                    if (!isFinish(node, complete)) return finish(InteractionState::Unavailable, "configured puzzle finish input unavailable");
                    if (complete && ++puzzleFinishMatches_ == 1) {
                        puzzleSelected_ = api.il2cpp_gchandle_new(node, false);
                        Object* graph{};
                        if (!puzzleSelected_ || !(graph = api.il2cpp_gchandle_get_target(puzzleOwner_)))
                            return finish(InteractionState::Unavailable, "selected puzzle graph unavailable");
                        puzzleSelectedGraph_ = api.il2cpp_gchandle_new(graph, false);
                        if (!puzzleSelectedGraph_) return finish(InteractionState::Unavailable, "selected puzzle graph handle unavailable");
                    }
                }
            } else {
                auto* graphs = api.il2cpp_gchandle_get_target(puzzleGraphs_);
                if (puzzleGraphIndex_ != api.il2cpp_array_length(graphs) || puzzleChildIndex_ != puzzleChildren_.size()) {
                    Object* graph{}; bool matches{};
                    if (puzzleGraphIndex_ != api.il2cpp_array_length(graphs)) {
                        if (!runtime.ArrayReferenceAt(graphs, puzzleGraphIndex_++, &graph)) return finish(InteractionState::Unavailable, "graph snapshot entry unavailable");
                    } else graph = api.il2cpp_gchandle_get_target(puzzleChildren_[puzzleChildIndex_++]);
                    if (puzzleVisited_.contains(reinterpret_cast<std::uintptr_t>(graph))) continue;
                    if (!graph || !graphMatches(graph, matches))
                        return finish(InteractionState::Unavailable, "native graph identity query failed");
                    if (matches) {
                        puzzleVisited_.insert(reinterpret_cast<std::uintptr_t>(graph));
                        ++puzzleGraphMatches_;
                        puzzleOwner_ = api.il2cpp_gchandle_new(graph, false);
                        if (!puzzleOwner_) return finish(InteractionState::Unavailable, "target graph owner handle unavailable");
                        Object* nodes{};
                        if (!Invoke(runtime, Get(Call::GraphNodes), graph, nullptr, nodes) ||
                            !snapshot(nodes, "Lens.Gameplay.Modules.BigWorld.NodeCanvas.Framework.Node[]", puzzleNodes_))
                            return finish(InteractionState::Unavailable, "target puzzle node snapshot unavailable");
                    }
                } else {
                    if (puzzleFinishMatches_ != 1)
                        return finish(puzzleGraphMatches_ ? InteractionState::Unsupported : InteractionState::Waiting,
                            puzzleGraphMatches_ ? "target has no unique configured puzzle finish node" : "target puzzle graph not loaded");
                    auto* selected = api.il2cpp_gchandle_get_target(puzzleSelected_);
                    auto* graph = api.il2cpp_gchandle_get_target(puzzleSelectedGraph_);
                    bool matches{}, complete{};
                    if (!selected || !graph || !graphMatches(graph, matches) || !matches || !isFinish(selected, complete) || !complete)
                        return finish(InteractionState::Unavailable, "selected puzzle owner or finish configuration is no longer active");
                    Object* flow{};
                    if (!Invoke(runtime, Get(Call::NewFlow), nullptr, nullptr, flow) || !flow)
                        return finish(InteractionState::Unavailable, "native Flow.New unavailable");
                    Root flowRoot(runtime, flow);
                    auto* value = flowRoot.get() ? api.il2cpp_object_unbox(flowRoot.get()) : nullptr;
                    void* parameters[]{value}; Object* ignored{};
                    if (api.il2cpp_class_is_assignable_from(puzzleGate_, api.il2cpp_object_get_class(selected))) {
                        Object* output{};
                        if (!ReadyPuzzleOutput(runtime, selected, mapId, output, details) || !output)
                            return finish(InteractionState::Unavailable, "configured unclaimed-reward output is no longer available");
                        receipt_.Arm(mapId);
                        if (!value || !Invoke(runtime, Get(Call::FlowOutputCall), output, parameters, ignored))
                            return finish(InteractionState::Unavailable, "native unclaimed-reward output dispatch failed");
                        puzzlePreparedMap_ = mapId; puzzlePreparedDetails_ = puzzleGateDetails_;
                        return finish(InteractionState::Prepared, "configured OverAndNotReward output dispatched; awaiting native chest action");
                    }
                    receipt_.Arm(mapId);
                    const auto call = api.il2cpp_class_is_assignable_from(legacyPuzzleNode_, api.il2cpp_object_get_class(selected)) ? Call::LegacyPuzzleFinish : Call::FinishPuzzle;
                    if (!value || !Invoke(runtime, Get(call), selected, parameters, ignored))
                        return finish(InteractionState::Unavailable, "native configured puzzle finish failed");
                    return finish(InteractionState::Accepted, "configured puzzle finish dispatched; awaiting completed target reward response");
                }
            }
            if (std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(2)) break;
        }
        return finish(InteractionState::Discovering, "bounded puzzle graph discovery pending");
    }
    bool UnitMapId(const Runtime& runtime, std::uint64_t unitId, std::int32_t& id) const {
        if (!unitId) return true;
        Object *manager{}, *unit{};
        if (!Invoke(runtime, Get(Call::WorldUnitManager), nullptr, nullptr, manager) || !manager) return false;
        void* parameters[]{&unitId};
        if (!Invoke(runtime, Get(Call::WorldUnit), manager, parameters, unit)) return false;
        return !unit || Value(runtime, Get(Call::UnitMapId), unit, nullptr, id);
    }
    bool HostMapId(const Runtime& runtime, Object* host, std::int32_t& id, std::string_view& source) const {
        id = 0; source = "unmapped";
        bool valid{};
        if (!Value(runtime, Get(Call::HostValid), host, nullptr, valid)) return false;
        if (!valid) return true;
        const auto& api = runtime.Functions();
        Object* entity{};
        if (!Invoke(runtime, Get(Call::WorldEntity), host, nullptr, entity)) return false;
        if (entity) {
            source = "entity";
            if (!Value(runtime, Get(Call::WorldGuid), entity, nullptr, id)) return false;
            if (id > 0) return true;

        }
        Object* go{}; api.il2cpp_field_get_value(host, hostGo_, &go);
        if (go) {
            auto* type = api.il2cpp_type_get_object(api.il2cpp_class_get_type(worldInstance_));
            bool inactive = true; void* parameters[]{type, &inactive}; Object* instance{};
            if (!type || !Invoke(runtime, Get(Call::ComponentInParent), go, parameters, instance)) return false;
            if (instance) {
                std::uint64_t unitId{};
                if (!api.il2cpp_class_is_assignable_from(worldInstance_, api.il2cpp_object_get_class(instance)) ||
                    !Value(runtime, Get(Call::InstanceUnitId), instance, nullptr, unitId) || !UnitMapId(runtime, unitId, id)) return false;
                if (id > 0) { source = "gameobject-unit"; return true; }
            }
        }
        return true;
    }
    static bool ValidPosition(const Vector3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
            std::abs(value.x) <= 1e6 && std::abs(value.y) <= 1e6 && std::abs(value.z) <= 1e6;
    }
    void ScanLoadedPickups(const Runtime& runtime, Object* mapManager) {
        const auto finish = [&](std::string_view failure = {}) {
            if (loadedSnapshot_) runtime.Functions().il2cpp_gchandle_free(loadedSnapshot_);
            loadedSnapshot_ = nullptr; loadedDone_ = true; loadedFailure_ = failure;
        };
        if (!loadedStarted_) {
            loadedStarted_ = true;
            Object *manager{}, *dictionary{}, *values{}, *snapshot{};
            if (!Invoke(runtime, Get(Call::InteractManager), nullptr, nullptr, manager) || !manager) { finish("WorldInteractManager unavailable"); return; }
            runtime.Functions().il2cpp_field_get_value(manager, interactions_, &dictionary);
            if (!dictionary) { finish(); return; }
            if (!Invoke(runtime, Member(runtime, dictionary, "get_Values", {}), dictionary, nullptr, values) || !values) { finish("registry.get_Values failed"); return; }
            Root valuesRoot(runtime, values);
            std::int32_t count{};
            if (!Value(runtime, Member(runtime, values, "get_Count", {}, "System.Int32"), values, nullptr, count) || count < 0 || count > 4096) { finish("registry snapshot count unavailable or exceeds budget"); return; }
            const auto& api = runtime.Functions();
            auto* klass = runtime.FindClassAnyImage("Azur.Gameplay.World.WorldInteract", "WorldInteractObject");
            auto* type = klass ? api.il2cpp_type_get_object(api.il2cpp_class_get_type(klass)) : nullptr;
            void* parameters[]{type, &count};
            if (!type || !Invoke(runtime, Get(Call::ArrayCreate), nullptr, parameters, snapshot) || !snapshot) { finish("registry snapshot allocation unavailable"); return; }
            Root root(runtime, snapshot); std::int32_t index{}; Object* ignored{}; void* copy[]{root.get(), &index};
            if (!root.get() || !Invoke(runtime, Member(runtime, values, "CopyTo", {"Azur.Gameplay.World.WorldInteract.WorldInteractObject[]", "System.Int32"}, "System.Void"), values, copy, ignored)) { finish("registry snapshot copy failed"); return; }
            loadedSnapshot_ = api.il2cpp_gchandle_new(root.get(), false);
            if (!loadedSnapshot_) { finish("registry snapshot handle unavailable"); return; }
        }
        const auto started = std::chrono::steady_clock::now();
        for (std::uint32_t budget = 32; budget; --budget) {
            if (budget != 32 && std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(2)) return;
            auto* snapshot = runtime.Functions().il2cpp_gchandle_get_target(loadedSnapshot_);
            if (loadedIndex_ == runtime.Functions().il2cpp_array_length(snapshot)) { finish(); return; }
            Object* interaction{};
            if (!runtime.ArrayReferenceAt(snapshot, loadedIndex_++, &interaction) || !interaction) { finish("registry snapshot entry unavailable"); return; }
            ++loadedInspected_;
            Object* action{};
            if (!Invoke(runtime, Get(Call::Action), interaction, nullptr, action) || !action) continue;
            auto* klass = runtime.Functions().il2cpp_object_get_class(action);
            const auto kind = klass == actions_[0] ? PickupKind::Collection : klass == actions_[1] ? PickupKind::Book : klass == actions_[2] ? PickupKind::Special : PickupKind::None;
            const bool chestAction = scanChests_ && (klass == defaultChestAction_ || std::find(chestActions_.begin(), chestActions_.end(), klass) != chestActions_.end());
            if ((!collectCatalog_ || kind == PickupKind::None) && !chestAction) continue;
            Object* host{}; std::int32_t id{}; std::string_view source;
            if (!Invoke(runtime, Get(Call::Host), interaction, nullptr, host) || !host ||
                !HostMapId(runtime, host, id, source) || id <= 0) continue;
            if (collectCatalog_ && kind != PickupKind::None) {
                const auto [found, inserted] = loadedPickups_.try_emplace(id, kind);
                if (!inserted && found->second == PickupKind::Collection) found->second = kind;
            }
            if (chestAction && !loadedChestIndex_.contains(id)) {
                Object* unit{}; std::int32_t actual{}, objectType{}; bool disposed{}; void* parameters[]{&id};
                if (!Invoke(runtime, Get(Call::MapUnit), mapManager, parameters, unit) || !unit ||
                    !Value(runtime, Get(Call::UnitMapId), unit, nullptr, actual) || actual != id ||
                    !Value(runtime, Get(Call::UnitDisposed), unit, nullptr, disposed) || disposed) continue;
                runtime.Functions().il2cpp_field_get_value(unit, unitObjectType_, &objectType);
                const auto type = std::find_if(chests_.begin(), chests_.end(), [objectType](const auto& entry) { return entry.value == objectType; });
                if (type == chests_.end()) continue;
                Vector3 position{};
                if (!Value(runtime, Get(Call::HostPosition), host, nullptr, position) || !ValidPosition(position)) continue;
                Target target; target.mapId = id; target.chest = target.livePosition = true; target.kind = type->name;
                target.position = {position.x, position.y, position.z};
                loadedChestIndex_.emplace(id, loadedChests_.size()); loadedChests_.push_back(std::move(target));
            }
            if (std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(2)) return;
        }
    }
    std::string DescribeNamedMethods(const Runtime& runtime, const Shape& shape) const {
        const auto& api = runtime.Functions();
        auto* klass = runtime.FindClassAnyImage(shape.namespaze, shape.klass);
        if (!klass) return {};
        const auto fullName = std::string(shape.namespaze) + '.' + shape.klass;
        std::ostringstream out; out << " observed=[";
        void* iterator{};
        while (const auto* method = api.il2cpp_class_get_methods(klass, &iterator)) {
            const auto* name = api.il2cpp_method_get_name(method);
            if (!name || std::strcmp(name, shape.method) != 0) continue;
            std::vector<std::string> types;
            for (std::uint32_t i{}; i < api.il2cpp_method_get_param_count(method); ++i)
                types.push_back(runtime.TypeName(api.il2cpp_method_get_param(method, i)));
            std::vector<CabbirdStringViewV1> parameters;
            for (const auto& type : types) parameters.push_back(StringView(type));
            std::uint32_t implementation{};
            const bool isStatic = (api.il2cpp_method_get_flags(method, &implementation) & 0x10u) != 0;
            const auto resultType = runtime.TypeName(api.il2cpp_method_get_return_type(method));
            CabbirdUnityMethodRequestV1 request{sizeof(request)};
            request.method_kind = isStatic ? CABBIRD_UNITY_METHOD_V1_STATIC : CABBIRD_UNITY_METHOD_V1_INSTANCE;
            request.module_name = StringView("GameAssembly.dll"); request.type_name = StringView(fullName);
            request.method_name = StringView(shape.method); request.return_type = StringView(resultType);
            request.parameter_types = parameters.data(); request.parameter_count = parameters.size();
            CabbirdUnityMethodResultV1 actual{sizeof(actual)};
            const auto status = metadata_->resolve_method(metadata_->user, &request, &actual);
            out << (isStatic ? "static " : "instance ") << resultType << '(';
            for (std::size_t i{}; i < types.size(); ++i) { if (i) out << ','; out << types[i]; }
            out << ") actualRva=0x" << std::hex << actual.rva << std::dec << " status=" << status.code << ';';
        }
        out << ']';
        return out.str();
    }
    bool Error(std::string message) const { failure_ = std::move(message); return false; }
    ScanState ScanError(std::string message) { Error(std::move(message)); return ScanState::Unavailable; }
    const Method* Get(Call call) const { return methods_[static_cast<std::size_t>(call)]; }
    bool BindField(const Runtime& runtime, const char* namespaze, const char* klass, const char* name, const char* type, Field*& destination, std::uint32_t storage = sizeof(void*)) {
        const auto fullName = std::string(namespaze) + '.' + klass;
        CabbirdUnityFieldRequestV1 request{sizeof(request), storage, StringView(fullName), StringView(name), StringView(type)};
        CabbirdUnityFieldResultV1 result{sizeof(result)};
        const auto status = metadata_->resolve_instance_field(metadata_->user, &request, &result);
        if (!Succeeded(status)) {
            std::string message = fullName + '.' + name + " field status=" + std::to_string(status.code);
            if (status.message.data && status.message.size) message.append(" ").append(status.message.data, status.message.size);
            return Error(std::move(message));
        }
        destination = runtime.FindField(runtime.FindClassAnyImage(namespaze, klass), name);
        return (destination && runtime.TypeName(runtime.Functions().il2cpp_field_get_type(destination)) == type) || Error(fullName + '.' + name + " runtime field type mismatch");
    }
    bool WorldState(const Runtime& runtime, Object*& world, Object*& map, Object*& local, Object*& manager) const {
        Object* scene{};
        if (!Invoke(runtime, Get(Call::World), nullptr, nullptr, world) || !world) return Error("AzurWorld.get_Instance unavailable");
        if (!Invoke(runtime, Get(Call::Scene), nullptr, nullptr, scene) || !scene) return Error("AzurWorld.get_service unavailable");
        if (!Invoke(runtime, Get(Call::MapManager), nullptr, nullptr, manager) || !manager) return Error("AzurWorld.get_WorldMapMgr unavailable");
        const auto& api = runtime.Functions();
        api.il2cpp_field_get_value(scene, map_, &map);
        if (!map) return Error("SceneService.map is null");
        bool prepared{};
        if (!Value(runtime, Get(Call::Prepared), map, nullptr, prepared) || !prepared) return Error("MapService.get_IsScenePrepared is false or unavailable");
        api.il2cpp_field_get_value(map, local_, &local);
        return local != nullptr || Error("MapService.m_Local is null");
    }
    const CabbirdCoreServiceV1* core_{};
    const CabbirdUnityMetadataServiceV1* metadata_{};
    std::uintptr_t moduleBase_{};
    Profile profile_;
    std::array<const Method*, static_cast<std::size_t>(Call::Count)> methods_{};
    std::array<std::uintptr_t, static_cast<std::size_t>(Call::Count)> entries_{};
    Field *map_{}, *local_{}, *server_{}, *interactions_{}, *hostGo_{}, *unitObjectType_{}, *unitScene_{}, *puzzleFinish_{}, *puzzleStates_{}, *puzzleOutputs_{}, *puzzleEntityInput_{}, *puzzleOutputTarget_{}, *interactionProcess_{}, *interactionVersion_{};
    Class *worldInstance_{}, *puzzleNode_{}, *legacyPuzzleNode_{}, *graphAssignable_{}, *puzzleGate_{};
    std::int32_t puzzleReadyState_{};
    mutable std::int32_t puzzlePreparedMap_{};
    mutable std::string puzzleGateDetails_, puzzlePreparedDetails_;
    mutable RewardReceipt receipt_;
    std::int32_t btnClick_{};
    struct ScanType { std::int32_t value; bool chest; const char* name; std::uint32_t count{}, duplicates{}, complete{}, unknown{}, positions{}; };
    std::vector<ScanType> chests_, collections_;
    std::vector<Class*> actions_, chestActions_;
    Class *defaultChestAction_{}, *chestTipsAction_{};
    std::vector<ScanType> scanKinds_;
    struct ScanStatistics { std::vector<ScanType> kinds; std::uint32_t positionSuccess{}, completeExcluded{}, unknownRetained{}, excludedUnclassified{}, officialMatches{}, nativeMatches{}, loadedChestAdded{}; std::map<std::int32_t, std::uint32_t> categories; } stats_;
    std::unordered_map<std::int32_t, PickupKind> loadedPickups_;
    std::vector<Target> loadedChests_;
    std::unordered_map<std::int32_t, std::size_t> loadedChestIndex_;
    std::size_t loadedChestMerge_{};
    CabbirdIl2CppGCHandleV1 loadedSnapshot_{};
    bool loadedStarted_{}, loadedDone_{};
    std::uint32_t loadedInspected_{}, loadedIndex_{};
    std::string loadedFailure_;
    CollectionCatalog catalog_;
    mutable std::string failure_;
    mutable std::string interactionDetails_;
    mutable CabbirdIl2CppGCHandleV1 puzzleGraphs_{}, puzzleNodes_{}, puzzleSelected_{}, puzzleSelectedGraph_{}, puzzleOwner_{};
    mutable std::vector<CabbirdIl2CppGCHandleV1> puzzleChildren_;
    mutable std::unordered_set<std::uintptr_t> puzzleVisited_;
    mutable std::map<std::string, std::uint32_t> puzzleNodeTypes_;
    mutable std::size_t puzzleChildIndex_{};
    mutable std::int32_t puzzleMap_{};
    mutable std::uint32_t puzzleGraphIndex_{}, puzzleNodeIndex_{}, puzzleGraphMatches_{}, puzzleFinishMatches_{};
    std::unordered_set<std::int32_t> scanSeen_;
    std::size_t scanKind_{};
    std::int32_t scanIndex_{}, complete_{}, incomplete_{}, noData_{};
    std::int32_t worldId_{};
    std::uintptr_t scannedMap_{}, scannedLocal_{};
    bool available_{}, ready_{}, collectCatalog_{}, scanChests_{};
};
} // namespace collectibles

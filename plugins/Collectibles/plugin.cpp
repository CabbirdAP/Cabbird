#include "collection_runtime.hpp"
#include "collection_hold.hpp"
#include "cabbird/sdk/services/game_lifecycle.h"
#include "cabbird/sdk/config_save_queue.hpp"
#include "cabbird/sdk/localization.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using namespace cabbird::sdk;
using namespace collectibles;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr auto kConfig = "settings";
constexpr auto kSchema = R"({"type":"object","required":["chests","collections","settleSeconds"],"additionalProperties":false,"properties":{"chests":{"type":"integer","minimum":0,"maximum":1},"collections":{"type":"integer","minimum":0,"maximum":1},"settleSeconds":{"type":"number","minimum":0.2,"maximum":3},"page":{"type":"integer","minimum":0,"maximum":1000000}}})";
struct Settings {
    int chests{1}, collections{1};
    float settleSeconds{.8f};
    std::uint32_t page{};
    bool operator==(const Settings&) const = default;
};
void to_json(Json& j, const Settings& s) { j = {{"chests",s.chests},{"collections",s.collections},{"settleSeconds",s.settleSeconds},{"page",s.page}}; }
void from_json(const Json& j, Settings& s) { j.at("chests").get_to(s.chests); j.at("collections").get_to(s.collections); j.at("settleSeconds").get_to(s.settleSeconds); s.page = j.value("page", 0u); }
bool Valid(const Settings& s) {
    return (s.chests == 0 || s.chests == 1) && (s.collections == 0 || s.collections == 1) &&
        std::isfinite(s.settleSeconds) && s.settleSeconds >= .2f && s.settleSeconds <= 3 &&
        s.page <= 1000000;
}
enum class Command { None, Scan, Start, Stop };
enum class Phase { Stopped, Scanning, Ready, Moving, Settling, Interacting, Complete, Unavailable };
enum class Failure { None, Services, Profile, Player, Scene, Binding, Scan, Teleport, Interaction, Completion };
struct Model {
    Phase phase{Phase::Stopped};
    Failure failure{};
    std::uint32_t total{}, visited{}, completed{}, skipped{}, unknownSkipped{};
    bool scanValid{};
    bool operator==(const Model&) const = default;
};
constexpr std::size_t kRowsPerPage = 20;
using ResultPage = std::vector<Target>;
struct Results { std::vector<std::shared_ptr<const ResultPage>> pages; std::size_t total{}; };
struct Context {
    explicit Context(const CabbirdHostApiV1* host) : text(host) {}
    Localizer text;
    const CabbirdCoreServiceV1* core{};
    const CabbirdUiServiceV1* ui{};
    const CabbirdWindowServiceV1* windows{};
    const CabbirdConfigServiceV1* config{};
    const CabbirdIl2CppServiceV1* il2cpp{};
    const CabbirdUnityPlayerServiceV1* player{};
    const CabbirdUnityPlayerTeleportServiceV1* teleport{};
    ConfigSaveQueue saves;
    CabbirdGenerationHandleV1 window{};
    Settings settings;
    Collector collector;
    CollectionHold hold;
    const CabbirdGameLifecycleServiceV1* lifecycle{};
    CabbirdGenerationHandleV1 cleanup{};
    std::atomic<std::shared_ptr<const Settings>> options;
    std::atomic<std::shared_ptr<const Model>> model;
    std::atomic<std::shared_ptr<const Results>> results;
    std::atomic<Command> command{};
    std::atomic_bool active{};
    std::vector<Target> queue;
    std::vector<std::shared_ptr<const ResultPage>> gamePages;
    std::set<std::size_t> dirtyPages;
    Clock::time_point resultPublished{};
    bool scanLogPending{};
    std::string lastFailure;
    Model game;
    std::uint64_t scene{}, playerId{};
    std::uint32_t baseline{}, observedSequence{};
    bool haveBaseline{}, sequenceObserved{}, puzzlePrepared{};
    std::array<double, 3> requested{}, departure{};
    Clock::time_point deadline{}, settled{};
};
CabbirdStatusV1 Status(std::uint32_t code) noexcept { return {code, 0, {}}; }
template<class T> const T* Query(const CabbirdHostApiV1* host, std::string_view id) {
    const auto service = Host(host).Query<T>(id);
    return service && service->struct_size >= sizeof(T) ? service.get() : nullptr;
}
bool PlayerValid(const CabbirdUnityPlayerSnapshotV1& player) {
    return (player.flags & CABBIRD_UNITY_PLAYER_V1_VALID) && player.entity_id;
}
bool Executing(Phase phase) {
    return phase == Phase::Scanning || phase == Phase::Moving || phase == Phase::Settling || phase == Phase::Interacting;
}
void ClearResults(Context& c) {
    c.gamePages.clear(); c.dirtyPages.clear();
    c.results.store(std::make_shared<Results>());
    c.resultPublished = Clock::now();
}
void Changed(Context& c, std::size_t index) { c.dirtyPages.insert(index / kRowsPerPage); }
std::string_view FailureName(Failure failure) {
    switch (failure) {
        case Failure::Profile: return "profile";
        case Failure::Player: return "player";
        case Failure::Scene: return "scene";
        case Failure::Binding: return "binding";
        case Failure::Scan: return "scan";
        case Failure::Teleport: return "teleport";
        case Failure::Interaction: return "interaction";
        case Failure::Completion: return "completion";
        default: return "services";
    }
}
std::string StatusDetail(std::string_view api, const CabbirdStatusV1& status) {
    auto detail = std::string(api) + " status=" + std::to_string(status.code);
    if (status.message.data && status.message.size) detail.append(" reason=").append(status.message.data, status.message.size);
    return detail;
}
void Publish(Context& c) {
    const auto old = c.model.load();
    if (!old || *old != c.game) c.model.store(std::make_shared<Model>(c.game));
    if (c.dirtyPages.empty() || Clock::now() - c.resultPublished < std::chrono::milliseconds(250)) return;
    for (auto page : c.dirtyPages) {
        const auto first = page * kRowsPerPage;
        if (first >= c.queue.size()) continue;
        if (c.gamePages.size() <= page) c.gamePages.resize(page + 1);
        c.gamePages[page] = std::make_shared<ResultPage>(c.queue.begin() + first,
            c.queue.begin() + std::min(c.queue.size(), first + kRowsPerPage));
    }
    c.results.store(std::make_shared<Results>(Results{c.gamePages, c.queue.size()}));
    c.dirtyPages.clear(); c.resultPublished = Clock::now();
}
void ScanLog(Context& c) {
    if (!c.scanLogPending || (c.game.phase != Phase::Ready && c.game.phase != Phase::Unavailable)) return;
    c.scanLogPending = false;
    if (!c.core || !c.core->log) return;
    auto detail = c.collector.ScanSummary(c.queue.size());
    if (c.game.phase == Phase::Unavailable) {
        detail += " stage=" + std::string(FailureName(c.game.failure));
        if (c.collector.LastFailure().empty() && !c.lastFailure.empty()) detail += " NativeDetails=" + c.lastFailure;
    }
    const std::string_view args[]{detail};
    const bool degraded = c.game.phase == Phase::Ready && c.collector.ScanDegraded();
    const auto message = degraded
        ? c.text.Format("log.scan_partial", "Collectibles scan classification unavailable: {0}", args)
        : c.game.phase == Phase::Ready ? c.text.Format("log.scan_complete", "Collectibles scan completed: {0}", args)
        : c.text.Format("log.scan_failed", "Collectibles scan failed: {0}", args);
    c.core->log(c.core->user, c.game.phase == Phase::Ready && !degraded ? CABBIRD_CORE_LOG_LEVEL_V1_INFO : CABBIRD_CORE_LOG_LEVEL_V1_WARNING, StringView(message));
}
void Fail(Context& c, Failure failure, std::string_view detail = {}) {
    c.hold.End();
    const bool changed = c.game.phase != Phase::Unavailable || c.game.failure != failure;
    if (Executing(c.game.phase) && c.game.phase != Phase::Scanning && c.game.visited < c.queue.size()) {
        c.queue[c.game.visited].route = RouteState::Failed; Changed(c, c.game.visited);
    }
    c.game.phase = Phase::Unavailable;
    c.game.failure = failure;
    c.lastFailure = detail.empty() ? c.collector.LastFailure() : detail;
    if (changed && !c.scanLogPending && c.core && c.core->log) {
        const std::string_view args[]{FailureName(failure), c.lastFailure};
        const auto message = c.text.Format("log.task_failed", "Collectibles task failed at {0}: {1}", args);
        c.core->log(c.core->user, CABBIRD_CORE_LOG_LEVEL_V1_WARNING, StringView(message));
    }
}
void LogInteractionSkip(Context& c, std::string_view outcome) {
    if (!c.core || !c.core->log) return;
    const std::string detail = std::string(outcome) + "; " + std::string(c.collector.LastInteractionDetails()) + c.collector.RewardDetails();
    const std::string_view args[]{detail};
    const auto message = c.text.Format("log.interaction_skipped", "Collectibles interaction skipped: {0}", args);
    c.core->log(c.core->user, CABBIRD_CORE_LOG_LEVEL_V1_WARNING, StringView(message));
}
void LogTargetResult(Context& c, const Target& target, std::string_view result) {
    if (!c.core || !c.core->log) return;
    const auto detail = "mapId=" + std::to_string(target.mapId) + " kind=" + target.kind + " result=" + std::string(result) + c.collector.RewardDetails();
    const std::string_view args[]{detail};
    const auto message = c.text.Format("log.target_result", "Collectibles target result: {0}", args);
    c.core->log(c.core->user, CABBIRD_CORE_LOG_LEVEL_V1_INFO, StringView(message));
}
void Advance(Context& c, bool completed, bool unknown = false, RouteState result = RouteState::Skipped) {
    c.hold.End();
    if (!c.collector.CancelInteraction(c.il2cpp)) { Fail(c, Failure::Binding, "puzzle discovery handle cleanup failed"); return; }
    c.queue[c.game.visited].route = completed ? RouteState::Complete : unknown ? RouteState::UnknownTimeout : result;
    Changed(c, c.game.visited);
    if (completed) ++c.game.completed;
    else ++c.game.skipped;
    if (unknown) ++c.game.unknownSkipped;
    ++c.game.visited;
    c.game.phase = c.game.visited == c.queue.size() ? Phase::Complete : Phase::Moving;
    c.deadline = {};
}
struct Work {
    Context* context;
    CabbirdUnityPlayerSnapshotV1 player;
    Command command;
};
CabbirdStatusV1 Execute(Work& work, const CabbirdIl2CppApiV1& api) {
    auto& c = *work.context;
    const il2cpp::Context runtime(api);
    if (!c.collector.Resolve(runtime)) {
        Fail(c, Failure::Binding);
        return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
    }
    const bool identityChanged = c.scene != work.player.generation || c.playerId != work.player.entity_id;
    if (work.command != Command::Scan && (work.command == Command::Start || identityChanged)) {
        const auto world = c.collector.CheckScanWorld(runtime);
        if (world != ScanWorld::Current) {
            if (world == ScanWorld::Changed) c.game.scanValid = false;
            Fail(c, Failure::Scene);
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        }
        if (identityChanged) {
            c.hold.End();
            if (!c.collector.CancelInteraction(c.il2cpp)) { Fail(c, Failure::Binding, "puzzle discovery identity cleanup failed"); return Status(CABBIRD_STATUS_V1_UNAVAILABLE); }
            c.scene = work.player.generation;
            c.playerId = work.player.entity_id;
            c.deadline = {}; c.settled = {}; c.haveBaseline = c.sequenceObserved = false;
            if (Executing(c.game.phase) && c.game.phase != Phase::Scanning && c.game.visited < c.queue.size()) {
                c.game.phase = Phase::Moving;
                c.queue[c.game.visited].route = RouteState::Queued; Changed(c, c.game.visited);
            }
        }
    }
    if (work.command == Command::Scan) {
        const auto settings = c.options.load();
        if (!c.collector.BeginScan(runtime, settings->chests != 0, settings->collections != 0)) {
            Fail(c, Failure::Scan);
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        }
        // Only value records survive this callback. Unity objects are resolved
        // again after teleport/streaming and are never retained by the route.
        c.scene = work.player.generation;
        c.playerId = work.player.entity_id;
        c.game.scanValid = false;
        c.game.phase = Phase::Scanning;
        return Ok();
    }
    if (c.game.phase == Phase::Scanning) {
        const auto before = c.queue.size();
        const auto result = c.collector.ScanStep(runtime, c.queue);
        for (auto index = before; index < c.queue.size(); ++index) Changed(c, index);
        c.game.total = static_cast<std::uint32_t>(c.queue.size());
        if (result == ScanState::Complete) { c.game.phase = Phase::Ready; c.game.scanValid = true; }
        else if (result == ScanState::Unavailable) { c.game.scanValid = false; Fail(c, Failure::Scan); }
        return result == ScanState::Unavailable ? Status(CABBIRD_STATUS_V1_UNAVAILABLE) : Ok();
    }
    if (work.command == Command::Start) {
        if (c.queue.empty() || !c.game.scanValid) {
            Fail(c, Failure::Scene);
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        }
        c.game.phase = c.game.visited < c.queue.size() ? Phase::Moving : Phase::Complete;
        c.game.failure = Failure::None;
        c.lastFailure.clear();
        c.deadline = {};
    }
    if (!Executing(c.game.phase)) return Ok();
    auto& target = c.queue.at(c.game.visited);
    const auto now = Clock::now();
    if (c.game.phase == Phase::Moving && c.deadline == Clock::time_point{}) {
        c.puzzlePrepared = false;
        const auto presence = target.chest ? c.collector.ProbeTarget(runtime, target) : TargetPresence{c.collector.Completion(runtime, target), PresenceState::MayLoad};
        const auto state = presence.completion;
        target.completion = state; Changed(c, c.game.visited);
        if (state == CompletionState::Complete && !target.chest && !c.puzzlePrepared) { LogTargetResult(c, target, "previous_complete_no_reward_counted"); Advance(c, false, false, RouteState::AlreadyComplete); return Ok(); }
        if (state == CompletionState::Unavailable) { Fail(c, Failure::Completion); return Status(CABBIRD_STATUS_V1_UNAVAILABLE); }
        if (presence.presence == PresenceState::Unavailable) { Fail(c, Failure::Interaction, "native target presence query failed"); return Status(CABBIRD_STATUS_V1_UNAVAILABLE); }
        if (presence.presence == PresenceState::NotSpawned) { LogInteractionSkip(c, "target is not spawned by the native creation gate"); Advance(c, false, false, RouteState::NotSpawned); return Ok(); }
        CabbirdUnityPlayerWriteResultV1 previous{sizeof(previous)};
        const auto previousStatus = c.teleport->state(c.teleport->user, &previous);
        if (!Succeeded(previousStatus) && previousStatus.code != CABBIRD_STATUS_V1_NOT_FOUND) {
            Fail(c, Failure::Teleport, StatusDetail("player-teleport.state", previousStatus));
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        }
        // NOT_FOUND is the documented state before the first applied request.
        // Invalidation clears that result without resetting the service counter.
        c.haveBaseline = Succeeded(previousStatus);
        c.baseline = previous.request_sequence;
        c.sequenceObserved = false;
        CabbirdUnityPlayerWriteRequestV1 request{sizeof(request)};
        if (!c.hold.Begin(runtime, work.player.entity_id)) {
            Fail(c, Failure::Binding, c.hold.LastFailure());
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        }
        request.entity_id = work.player.entity_id;
        c.requested = target.position;
        std::copy(std::begin(work.player.position), std::end(work.player.position), c.departure.begin());
        std::copy(c.requested.begin(), c.requested.end(), request.position);
        const auto submitted = c.teleport->teleport(c.teleport->user, &request);
        if (!Succeeded(submitted)) {
            Fail(c, Failure::Teleport, StatusDetail("player-teleport.teleport", submitted));
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        }
        c.deadline = now + std::chrono::seconds(10);
        target.route = RouteState::Moving; Changed(c, c.game.visited);
        return Ok();
    }
    if (c.game.phase == Phase::Moving) {
        CabbirdUnityPlayerWriteResultV1 result{sizeof(result)};
        const auto status = c.teleport->state(c.teleport->user, &result);
        if (!Succeeded(status) && status.code != CABBIRD_STATUS_V1_NOT_FOUND) {
            Fail(c, Failure::Teleport, StatusDetail("player-teleport.state", status));
            return status;
        }
        if (Succeeded(status) && (c.sequenceObserved || !c.haveBaseline || result.request_sequence != c.baseline)) {
            const bool ours = (!c.sequenceObserved || result.request_sequence == c.observedSequence) && result.entity_id == c.playerId &&
                std::equal(c.requested.begin(), c.requested.end(), result.applied_position);
            if (!ours || (result.result_code != CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_OK &&
                          result.result_code != CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_PENDING)) {
                const auto coordinates = [](const auto& values) {
                    return '(' + std::to_string(values[0]) + ',' + std::to_string(values[1]) + ',' + std::to_string(values[2]) + ')';
                };
                auto detail = "player-teleport.state mapId=" + std::to_string(target.mapId) + " result=" + std::to_string(result.result_code) +
                    " sequence=" + std::to_string(result.request_sequence) + " sequenceObserved=" + std::to_string(c.sequenceObserved) +
                    " sequenceMismatch=" + std::to_string(c.sequenceObserved && result.request_sequence != c.observedSequence) +
                    " playerMismatch=" + std::to_string(result.entity_id != c.playerId) +
                    " targetMismatch=" + std::to_string(!std::equal(c.requested.begin(), c.requested.end(), result.applied_position)) +
                    " positionError=" + std::to_string(result.position_error) + " applyMicros=" + std::to_string(result.apply_micros) +
                    " departure=" + coordinates(c.departure) + " requested=" + coordinates(c.requested) +
                    " measured=" + coordinates(result.measured_position) +
                    " delta=(" + std::to_string(result.measured_position[0] - c.requested[0]) + ',' +
                    std::to_string(result.measured_position[1] - c.requested[1]) + ',' +
                    std::to_string(result.measured_position[2] - c.requested[2]) + ')';
                if (status.message.data && status.message.size) detail.append(" nativeReason=").append(status.message.data, status.message.size);
                detail += " " + c.hold.Diagnostics();
                Fail(c, Failure::Teleport, detail);
                return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
            }
            c.observedSequence = result.request_sequence;
            c.sequenceObserved = true;
            if (result.result_code == CABBIRD_UNITY_PLAYER_WRITE_V1_RESULT_OK) {
                if (!c.collector.RefreshArrival(runtime, target)) {
                    Fail(c, Failure::Interaction);
                    return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
                }
                c.game.phase = Phase::Settling;
                target.route = RouteState::Waiting; Changed(c, c.game.visited);
                c.settled = now + std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(c.options.load()->settleSeconds));
                c.deadline = now + std::chrono::seconds(10);
            }
        }
        if (now >= c.deadline) Fail(c, Failure::Teleport, "player-teleport.state confirmation timeout");
        return Ok();
    }
    if (c.game.phase == Phase::Settling) {
        if (now < c.settled) return Ok();
        if (c.puzzlePrepared && c.collector.RewardConfirmed()) { LogTargetResult(c, target, "reward_confirmed"); Advance(c, true); return Ok(); }
        const auto presence = target.chest ? c.collector.ProbeTarget(runtime, target) : TargetPresence{c.collector.Completion(runtime, target), PresenceState::MayLoad};
        const auto state = presence.completion;
        if (target.completion != state) { target.completion = state; Changed(c, c.game.visited); }
        if (state == CompletionState::Complete && !target.chest && !c.puzzlePrepared) { LogTargetResult(c, target, "previous_complete_no_reward_counted"); Advance(c, false, false, RouteState::AlreadyComplete); return Ok(); }
        if (state == CompletionState::Unavailable) { Fail(c, Failure::Completion); return Status(CABBIRD_STATUS_V1_UNAVAILABLE); }
        if (presence.presence == PresenceState::Unavailable) { Fail(c, Failure::Interaction, "native target presence query failed"); return Status(CABBIRD_STATUS_V1_UNAVAILABLE); }
        if (presence.presence == PresenceState::NotSpawned && !c.puzzlePrepared) { LogInteractionSkip(c, "target is no longer spawned by the native creation gate"); Advance(c, false, false, RouteState::NotSpawned); return Ok(); }
        if (now >= c.deadline) { LogInteractionSkip(c, "native interaction readiness timed out"); Advance(c, false, state == CompletionState::Unknown, RouteState::InteractionTimeout); return Ok(); }
        const auto action = c.collector.Interact(runtime, target);
        if (action == InteractionState::Prepared) {
            c.puzzlePrepared = true;
            c.settled = now;
        } else if (action == InteractionState::Accepted) {
            c.game.phase = Phase::Interacting;
            target.route = RouteState::Interacting; Changed(c, c.game.visited);
            c.deadline = now + std::chrono::seconds(10);
        } else if (action == InteractionState::Unavailable) {
            Fail(c, Failure::Interaction, c.collector.LastInteractionDetails());
        } else if (action == InteractionState::Unsupported) {
            LogInteractionSkip(c, "unsupported native action");
            Advance(c, false, false, RouteState::Unsupported);
        } else {
            c.settled = action == InteractionState::Discovering ? now : now + std::chrono::milliseconds(200);
        }
        return Ok();
    }
    if (c.game.phase == Phase::Interacting) {
        const auto state = c.collector.Completion(runtime, target);
        if (target.completion != state) { target.completion = state; Changed(c, c.game.visited); }
        if (c.collector.RewardConfirmed()) { LogTargetResult(c, target, "reward_confirmed"); Advance(c, true); }
        else if (state == CompletionState::Unavailable) Fail(c, Failure::Completion);
        else if (now >= c.deadline) { LogInteractionSkip(c, "native action dispatched but no matching completed reward response was confirmed"); Advance(c, false, state == CompletionState::Unknown, RouteState::InteractionTimeout); }
    }
    return Ok();
}
CabbirdStatusV1 ExecuteEntry(void* raw, const CabbirdIl2CppApiV1* api) noexcept {
    try {
        if (!api || api->struct_size < sizeof(*api) || api->api_version != 1)
            return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        return Execute(*static_cast<Work*>(raw), *api);
    } catch (...) { return Status(CABBIRD_STATUS_V1_FAILED); }
}
CabbirdStatusV1 CABBIRD_CALL RuntimeEntry(void* raw, const CabbirdIl2CppApiV1* api) noexcept {
    __try { return ExecuteEntry(raw, api); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Status(CABBIRD_STATUS_V1_FAILED); }
}
void CABBIRD_CALL Update(void* raw, double) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        if (!c.active.load()) return;
        const auto command = c.command.exchange(Command::None);
        if (command == Command::Stop) {
            c.hold.End();
            const bool released = c.collector.CancelScan(c.il2cpp);
            if (c.game.phase == Phase::Scanning) { c.game.scanValid = false; c.scanLogPending = false; }
            else if (Executing(c.game.phase) && c.game.visited < c.queue.size()) {
                c.queue[c.game.visited].route = RouteState::Stopped; Changed(c, c.game.visited);
            }
            c.game.phase = Phase::Stopped; c.game.failure = {};
            if (!released) Fail(c, Failure::Binding, "il2cpp.release_handles failed");
            Publish(c); return;
        }
        if (command == Command::Scan) {
            c.hold.End();
            if (!c.collector.CancelScan(c.il2cpp)) { Fail(c, Failure::Binding, "il2cpp.release_handles failed"); Publish(c); return; }
            c.queue.clear(); ClearResults(c); c.game = {};
            c.collector.ResetScanReport(); c.scanLogPending = true;
        }
        if (command == Command::None && !Executing(c.game.phase)) { Publish(c); return; }
        if (!c.collector.Available()) { Fail(c, Failure::Profile); ScanLog(c); Publish(c); return; }
        CabbirdUnityPlayerSnapshotV1 player{sizeof(player)};
        const auto playerStatus = c.player->snapshot(c.player->user, &player);
        if (!Succeeded(playerStatus) || !PlayerValid(player)) {
            c.collector.CancelScan(c.il2cpp);
            Fail(c, Failure::Player, StatusDetail("player.snapshot", playerStatus)); ScanLog(c); Publish(c); return;
        }
        Work work{&c, player, command};
        if (!Succeeded(c.il2cpp->with_runtime(c.il2cpp->user, RuntimeEntry, &work)) &&
            c.game.failure == Failure::None) Fail(c, Failure::Binding);
        if (c.game.phase == Phase::Unavailable) c.collector.CancelScan(c.il2cpp);
        ScanLog(c); Publish(c);
    } catch (...) {
        try { auto& c = *static_cast<Context*>(raw); c.collector.CancelScan(c.il2cpp); Fail(c, Failure::Binding); ScanLog(c); Publish(c); } catch (...) {}
    }
}
std::string CompletionText(Context& c, CompletionState state) {
    switch (state) {
        case CompletionState::Complete: return c.text.Text("completion.complete", "Object complete; reward unconfirmed");
        case CompletionState::Incomplete: return c.text.Text("completion.incomplete", "Incomplete");
        default: return c.text.Text("completion.unknown", "Unknown");
    }
}
std::string RouteText(Context& c, RouteState state) {
    switch (state) {
        case RouteState::Moving: return c.text.Text("route.moving", "Teleporting");
        case RouteState::Waiting: return c.text.Text("route.waiting", "Waiting for game state");
        case RouteState::AlreadyComplete: return c.text.Text("route.already_complete", "Previously complete; skipped");
        case RouteState::NotSpawned: return c.text.Text("route.not_spawned", "Not spawned; reward unconfirmed");
        case RouteState::Interacting: return c.text.Text("route.interacting", "Interaction sent");
        case RouteState::Complete: return c.text.Text("route.complete", "Confirmed complete");
        case RouteState::Skipped: return c.text.Text("route.skipped", "Skipped");
        case RouteState::UnknownTimeout: return c.text.Text("route.unknown_timeout", "Skipped: state remained unknown");
        case RouteState::Unsupported: return c.text.Text("route.unsupported", "Skipped: unsupported interaction");
        case RouteState::InteractionTimeout: return c.text.Text("route.interaction_timeout", "Skipped: no confirmed completion");
        case RouteState::Stopped: return c.text.Text("route.stopped", "Stopped");
        case RouteState::Failed: return c.text.Text("route.failed", "Failed");
        default: return c.text.Text("route.queued", "Queued");
    }
}
std::string PositionText(const Target& target) {
    char buffer[128]{};
    std::snprintf(buffer, sizeof(buffer), "%.1f, %.1f, %.1f", target.position[0], target.position[1], target.position[2]);
    return buffer;
}
std::string CategoryText(Context& c, const Target& target) {
    if (target.official) {
        if (!target.categoryLabel.empty()) return target.categoryLabel;
        if (target.categoryId == 0) return c.text.Text("category.catalogue", "Catalog collectible");
        const auto id = std::to_string(target.categoryId); const std::string_view args[]{id};
        return c.text.Format("category.official_id", "Collection type {0}", args);
    }
    if (target.kind == "ChestWooden") return c.text.Text("category.wooden", "Wooden chest");
    if (target.kind == "ChestSilver") return c.text.Text("category.silver", "Silver chest");
    if (target.kind == "ChestIron") return c.text.Text("category.iron", "Iron chest");
    if (target.kind == "ChestGold") return c.text.Text("category.gold", "Gold chest");
    if (target.kind == "PuzzleChestItem") return c.text.Text("category.puzzle", "Puzzle chest");
    if (target.chest) return c.text.Text("category.chest", "Chest");
    if (target.kind == "Oculus") return c.text.Text("category.special", "Special collectible");
    if (target.kind == "KiboEgg") return c.text.Text("category.egg", "Egg collectible");
    if (target.nativePickup == PickupKind::Collection) return c.text.Text("category.collection", "Collectible");
    if (target.nativePickup == PickupKind::Book) return c.text.Text("category.book", "Book");
    if (target.nativePickup == PickupKind::Special) return c.text.Text("category.special", "Special collectible");
    return c.text.Text("category.unknown", "Unclassified");
}
std::string NameText(Context& c, const Target& target) {
    if (!target.name.empty()) return target.name;
    if (target.official && target.collectionId > 0) {
        const auto id = std::to_string(target.collectionId); const std::string_view args[]{id};
        return c.text.Format("name.collection_id", "Collectible {0}", args);
    }
    return CategoryText(c, target);
}
void CABBIRD_CALL Draw(void* raw, const CabbirdUiServiceV1*) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        CabbirdWindowStateV1 state{sizeof(state)};
        if (!c.window.id || !Succeeded(c.windows->state(c.windows->user, c.window, &state)) || !state.open) return;
        int visible{};
        if (!Succeeded(c.windows->begin(c.windows->user, c.window, 0, &visible))) return;
        struct End { Context& c; ~End() { c.windows->end(c.windows->user, c.window); } } end{c};
        if (!visible) return;
        const auto model = c.model.load();
        const bool busy = model && Executing(model->phase);
        const auto before = c.settings;
        c.ui->checkbox(c.ui->user, StringView(c.text.Text("settings.chests", "Chests")), &c.settings.chests);
        c.ui->checkbox(c.ui->user, StringView(c.text.Text("settings.collections", "Collectibles")), &c.settings.collections);
        c.ui->slider_float(c.ui->user, StringView(c.text.Text("settings.settle", "Wait after teleport (seconds)")), &c.settings.settleSeconds, .2f, 3);
        if (c.ui->button_enabled(c.ui->user, StringView(c.text.Text("action.scan", "Scan")), 0, 0, !busy))
            c.command.store(Command::Scan);
        const bool resume = model && (model->phase == Phase::Stopped || model->phase == Phase::Unavailable);
        const auto startLabel = resume ? c.text.Text("action.resume", "Resume") : c.text.Text("action.start", "Start");
        if (c.ui->button_enabled(c.ui->user, StringView(startLabel), 0, 0,
                !busy && model && model->scanValid && model->visited < model->total &&
                (model->phase == Phase::Ready || resume))) c.command.store(Command::Start);
        if (c.ui->button_enabled(c.ui->user, StringView(c.text.Text("action.stop", "Stop")), 0, 0, busy))
            { c.hold.Cancel(); c.command.store(Command::Stop); }
        if (model && (model->total || model->phase == Phase::Ready)) {
            if (!model->scanValid) c.ui->text(c.ui->user, StringView(c.text.Text("queue.map_invalid", "The map changed or scanning did not finish. Scan again before continuing.")));
            const std::string total = std::to_string(model->total), completed = std::to_string(model->completed),
                skipped = std::to_string(model->skipped), visited = std::to_string(model->visited), unknown = std::to_string(model->unknownSkipped);
            const std::string_view args[]{total, visited, completed, skipped, unknown};
            c.ui->text(c.ui->user, StringView(c.text.Format("queue.progress", "{0} targets; {1} visited; {2} rewards confirmed; {3} skipped ({4} unknown-state timeouts)", args)));
        }
        const auto results = c.results.load();
        if (results && results->total) {
            const auto page = std::min<std::size_t>(c.settings.page, results->pages.size() - 1);
            c.settings.page = static_cast<std::uint32_t>(page);
            const std::string pageNumber = std::to_string(page + 1), pageCount = std::to_string(results->pages.size());
            const std::string_view pageArgs[]{pageNumber, pageCount};
            c.ui->text(c.ui->user, StringView(c.text.Format("table.page", "Page {0}/{1}", pageArgs)));
            if (c.ui->begin_table(c.ui->user, StringView("collectibles-results"), 7, CABBIRD_UI_TABLE_V1_SIZING_STRETCH_PROP, 0, 0)) {
                const auto cell = [&](std::string_view text) {
                    c.ui->table_next_column(c.ui->user);
                    if (c.ui->text_wrapped) c.ui->text_wrapped(c.ui->user, StringView(text));
                    else c.ui->text(c.ui->user, StringView(text));
                };
                c.ui->table_next_row(c.ui->user);
                cell(c.text.Text("table.kind", "Category")); cell(c.text.Text("table.name", "Name")); cell(c.text.Text("table.id", "Map ID"));
                cell(c.text.Text("table.position", "World coordinates")); cell(c.text.Text("table.completion", "Completion"));
                cell(c.text.Text("table.result", "Route result"));
                cell(c.text.Text("table.source", "Source"));
                for (const auto& target : *results->pages[page]) {
                    c.ui->table_next_row(c.ui->user);
                    cell(CategoryText(c, target)); cell(NameText(c, target));
                    cell(std::to_string(target.mapId)); cell(PositionText(target));
                    cell(CompletionText(c, target.completion)); cell(RouteText(c, target.route));
                    cell(target.official ? c.text.Text("source.catalog", "Catalog") : target.nativePickup != PickupKind::None || target.livePosition
                        ? c.text.Text("source.loaded", "Loaded interaction") : c.text.Text("source.map", "Map"));
                }
                c.ui->end_table(c.ui->user);
            }
            if (c.ui->button_enabled(c.ui->user, StringView(c.text.Text("table.previous", "Previous page")), 0, 0, page > 0))
                c.settings.page = static_cast<std::uint32_t>(page - 1);
            if (c.ui->button_enabled(c.ui->user, StringView(c.text.Text("table.next", "Next page")), 0, 0, page + 1 < results->pages.size()))
                c.settings.page = static_cast<std::uint32_t>(page + 1);
        }
        if (c.settings != before) {
            c.options.store(std::make_shared<Settings>(c.settings));
            static_cast<void>(c.saves.Queue(Json(c.settings).dump()));
        }
    } catch (...) {}
}
CabbirdStatusV1 CABBIRD_CALL Cleanup(void* raw) noexcept {
    auto& c = *static_cast<Context*>(raw);
    c.hold.Cancel();
    return c.hold.End() ? Ok() : Status(CABBIRD_STATUS_V1_FAILED);
}
CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** out) noexcept {
    if (!host || !out) return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    *out = nullptr;
    try {
        auto c = std::make_unique<Context>(host);
        c->core = Query<CabbirdCoreServiceV1>(host, CABBIRD_CORE_SERVICE_V1_ID);
        c->ui = Query<CabbirdUiServiceV1>(host, CABBIRD_UI_SERVICE_V1_ID);
        c->windows = Query<CabbirdWindowServiceV1>(host, CABBIRD_WINDOW_SERVICE_V1_ID);
        c->config = Query<CabbirdConfigServiceV1>(host, CABBIRD_CONFIG_SERVICE_V1_ID);
        c->il2cpp = Query<CabbirdIl2CppServiceV1>(host, CABBIRD_IL2CPP_SERVICE_V1_ID);
        c->player = Query<CabbirdUnityPlayerServiceV1>(host, CABBIRD_UNITY_PLAYER_SERVICE_V1_ID);
        c->teleport = Query<CabbirdUnityPlayerTeleportServiceV1>(host, CABBIRD_UNITY_PLAYER_TELEPORT_SERVICE_V1_ID);
        c->lifecycle = Query<CabbirdGameLifecycleServiceV1>(host, CABBIRD_GAME_LIFECYCLE_SERVICE_V1_ID);
        const auto* scheduler = Query<CabbirdSchedulerServiceV1>(host, CABBIRD_SCHEDULER_SERVICE_V1_ID);
        if (!c->ui || !c->ui->text || !c->ui->checkbox || !c->ui->slider_float || !c->ui->button_enabled ||
            !c->ui->begin_table || !c->ui->table_next_row || !c->ui->table_next_column || !c->ui->end_table ||
            !c->windows || !c->windows->register_window || !c->windows->release_window || !c->windows->state ||
            !c->windows->begin || !c->windows->end || !c->config || !c->config->register_schema ||
            !c->config->read || !c->config->write_atomic || !scheduler || !scheduler->schedule ||
            !c->il2cpp || !c->il2cpp->with_runtime || !c->il2cpp->release_handles || !c->player || !c->player->snapshot ||
            !c->teleport || !c->teleport->teleport || !c->teleport->state || !c->lifecycle ||
            !c->lifecycle->register_stop_cleanup || !c->lifecycle->unregister_stop_cleanup) return Status(CABBIRD_STATUS_V1_UNAVAILABLE);
        CabbirdGenerationHandleV1 schema{};
        const auto status = c->config->register_schema(c->config->user, StringView(kConfig), 1,
            {reinterpret_cast<const std::uint8_t*>(kSchema), std::strlen(kSchema)}, &schema);
        if (!Succeeded(status)) return status;
        bool migrateSettings{};
        std::size_t size{}; std::uint32_t version{};
        const auto reading = c->config->read(c->config->user, StringView(kConfig), &version, {}, &size);
        if ((Succeeded(reading) || reading.code == CABBIRD_STATUS_V1_BUFFER_TOO_SMALL) && size && size <= 4096 && version == 1) {
            std::vector<std::uint8_t> bytes(size);
            if (Succeeded(c->config->read(c->config->user, StringView(kConfig), &version, {bytes.data(), bytes.size()}, &size)) && version == 1 && size <= bytes.size()) {
                try {
                    const auto stored = Json::parse(bytes.begin(), bytes.begin() + size);
                    const auto settings = stored.get<Settings>();
                    if (Valid(settings)) { c->settings = settings; migrateSettings = stored.contains("landingHeight"); }
                } catch (...) {}
            }
        }
        c->saves.Bind(host, c->config, kConfig, 1);
        if (migrateSettings) static_cast<void>(c->saves.Queue(Json(c->settings).dump()));
        c->options.store(std::make_shared<Settings>(c->settings));
        if (!c->collector.Bind(host) || !c->hold.Bind(host)) Fail(*c, Failure::Profile);
        const auto cleanup = c->lifecycle->register_stop_cleanup(c->lifecycle->user, Cleanup, c.get(), &c->cleanup);
        if (!Succeeded(cleanup)) return cleanup;
        ClearResults(*c);
        Publish(*c);
        *out = c.release();
        return Ok();
    } catch (...) { return Status(CABBIRD_STATUS_V1_FAILED); }
}
CabbirdStatusV1 CABBIRD_CALL Start(void* raw) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        const auto title = c.text.Text("window.title", "Collectibles");
        CabbirdWindowSpecV1 spec{sizeof(spec)};
        spec.id = StringView("collectibles"); spec.title = StringView(title);
        spec.initial_width = 860; spec.initial_height = 520;
        spec.minimum_width = 100; spec.minimum_height = 100;
        spec.maximum_width = 8192; spec.maximum_height = 8192; spec.default_open = 1;
        const auto status = c.windows->register_window(c.windows->user, &spec, &c.window);
        if (Succeeded(status)) c.active.store(true);
        return status;
    } catch (...) { return Status(CABBIRD_STATUS_V1_FAILED); }
}
CabbirdStatusV1 CABBIRD_CALL Stop(void* raw, std::uint32_t) noexcept {
    try {
        auto& c = *static_cast<Context*>(raw);
        c.active.store(false); c.command.store(Command::None);
        if (!c.hold.Release()) return Status(CABBIRD_STATUS_V1_FAILED);
        if (!c.collector.StopReceipt() || !c.collector.CancelScan(c.il2cpp)) return Status(CABBIRD_STATUS_V1_FAILED);
        c.queue.clear(); c.game.phase = Phase::Stopped;
        const bool saved = c.saves.Flush();
        if (c.window.id) {
            const auto status = c.windows->release_window(c.windows->user, c.window);
            if (!Succeeded(status) && status.code != CABBIRD_STATUS_V1_NOT_FOUND) return status;
            c.window = {};
        }
        if (c.cleanup.id) {
            const auto cleanup = c.lifecycle->unregister_stop_cleanup(c.lifecycle->user, c.cleanup);
            if (!Succeeded(cleanup) && cleanup.code != CABBIRD_STATUS_V1_NOT_FOUND) return cleanup;
            c.cleanup = {};
        }
        return saved ? Ok() : Status(CABBIRD_STATUS_V1_FAILED);
    } catch (...) { return Status(CABBIRD_STATUS_V1_FAILED); }
}
void CABBIRD_CALL Unload(void* raw) noexcept { try { delete static_cast<Context*>(raw); } catch (...) {} }
} // namespace
CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(CabbirdPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        cabbird::sdk::StringView("cabbird.collectibles"), cabbird::sdk::StringView("Collectibles"),
        cabbird::sdk::StringView("Cabbird"), cabbird::sdk::StringView("1.0.0"), Load, Start, Stop, Unload, Update, Draw};
    return cabbird::sdk::Ok();
}

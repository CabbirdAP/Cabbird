#include "cabbird/platform_ui_model.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <utility>

namespace cabbird {
namespace {

bool IsStartLike(PlatformUiPluginMutation mutation) noexcept {
    return mutation == PlatformUiPluginMutation::Start ||
        mutation == PlatformUiPluginMutation::Enable;
}

bool IsStopLike(PlatformUiPluginMutation mutation) noexcept {
    return mutation == PlatformUiPluginMutation::Stop ||
        mutation == PlatformUiPluginMutation::Disable;
}

void AddAffected(
    std::map<std::string, AffectedPlugin, std::less<>>& affected,
    std::string id,
    bool directly_requested,
    std::string reason) {
    if (id.empty()) return;
    // Keep the map key intact while moving the display object; argument evaluation
    // order is unspecified and moving `id` inline can otherwise produce an empty key.
    const std::string key = id;
    auto [iterator, inserted] = affected.emplace(
        key, AffectedPlugin{std::move(id), directly_requested, std::move(reason)});
    if (!inserted && directly_requested) iterator->second.directly_requested = true;
}

bool HasDependency(
    const InstalledPluginView& plugin, std::string_view id, bool include_optional) noexcept {
    return std::ranges::any_of(plugin.dependencies, [&](const PluginDependencyManifest& dependency) {
        return dependency.id == id && (include_optional || !dependency.optional);
    });
}

}  // namespace

PlatformUiAffectedSetPreflight PreflightPluginMutation(
    const PlatformUiSnapshot& snapshot, const PlatformUiIntent& intent) {
    PlatformUiAffectedSetPreflight result;
    result.mutation = ResolvePlatformUiMutation(intent);
    result.subject_id = intent.subject_id;

    std::map<std::string, AffectedPlugin, std::less<>> affected;
    const auto add_plugin = [&](std::string_view id, bool direct, std::string reason) {
        AddAffected(affected, std::string(id), direct, std::move(reason));
    };

    if (result.mutation == PlatformUiPluginMutation::ReloadAll) {
        for (const auto& plugin : snapshot.installed_plugins) {
            add_plugin(plugin.id, false, "batch reload");
        }
    } else {
        const auto* target = snapshot.FindPlugin(intent.subject_id);
        if (target == nullptr) {
            result.decision = PlatformUiPreflightDecision::Blocked;
            result.reason = "plugin was not found in the Installed snapshot";
            return result;
        }
        add_plugin(target->id, true, "requested plugin");
        if (IsStartLike(result.mutation) || result.mutation == PlatformUiPluginMutation::Reload) {
            if (target->UiState() == PlatformUiPluginState::Rejected ||
                target->UiState() == PlatformUiPluginState::Incompatible) {
                result.decision = PlatformUiPreflightDecision::Blocked;
                result.reason = std::string(ToString(target->UiState()));
            } else if (target->dependency_state != PluginDependencyState::Ready) {
                result.decision = PlatformUiPreflightDecision::Blocked;
                result.reason = "dependency graph blocks this operation";
            } else if (result.mutation == PlatformUiPluginMutation::Reload &&
                       !target->has_runtime_view) {
                result.decision = PlatformUiPreflightDecision::Blocked;
                result.reason = "plugin has no active generation to reload";
            }
            if (result.mutation == PlatformUiPluginMutation::Reload) {
                // ReloadPackages replaces the selected generation and every
                // transitive dependent that holds imports from it.
                bool changed = true;
                while (changed) {
                    changed = false;
                    for (const auto& plugin : snapshot.installed_plugins) {
                        if (affected.contains(plugin.id)) continue;
                        for (const auto& item : affected) {
                            if (HasDependency(plugin, item.first, true)) {
                                add_plugin(
                                    plugin.id, false,
                                    "dependent generation must reload");
                                changed = true;
                                break;
                            }
                        }
                    }
                }
            } else {
                // Starting a plugin only follows mandatory dependency edges.
                std::unordered_set<std::string> visiting;
                std::function<void(const InstalledPluginView&)> add_dependencies =
                    [&](const InstalledPluginView& plugin) {
                        if (!visiting.insert(plugin.id).second) return;
                        for (const auto& requirement : plugin.dependencies) {
                            if (requirement.optional) continue;
                            const auto* dependency = snapshot.FindPlugin(requirement.id);
                            if (dependency == nullptr) {
                                result.decision = PlatformUiPreflightDecision::Blocked;
                                result.reason = "required dependency is missing: " + requirement.id;
                                continue;
                            }
                            add_plugin(dependency->id, false, "required dependency");
                            if (dependency->dependency_state != PluginDependencyState::Ready ||
                                dependency->UiState() == PlatformUiPluginState::Rejected ||
                                dependency->UiState() == PlatformUiPluginState::Incompatible) {
                                result.decision = PlatformUiPreflightDecision::Blocked;
                                result.reason = "required dependency is blocked: " + dependency->id;
                            }
                            add_dependencies(*dependency);
                        }
                    };
                add_dependencies(*target);
            }
        } else if (IsStopLike(result.mutation)) {
            // Stopping a provider also stops plugins that require it.
            bool changed = true;
            while (changed) {
                changed = false;
                for (const auto& plugin : snapshot.installed_plugins) {
                    if (affected.find(plugin.id) != affected.end()) continue;
                    for (const auto& item : affected) {
                        if (HasDependency(plugin, item.first, false)) {
                            add_plugin(plugin.id, false, "dependent plugin must stop first");
                            changed = true;
                            break;
                        }
                    }
                }
            }
        } else if (result.mutation == PlatformUiPluginMutation::SetVisible) {
            if (!target->visibility_control || !target->IsRunning()) {
                result.decision = PlatformUiPreflightDecision::Blocked;
                result.reason = "plugin does not expose an active visibility control";
            }
        }
    }

    for (auto& [id, item] : affected) result.affected.push_back(std::move(item));
    if (result.decision == PlatformUiPreflightDecision::Blocked) return result;
    if (result.affected.empty()) {
        result.decision = PlatformUiPreflightDecision::Blocked;
        result.reason = "no Installed plugins are available";
        return result;
    }
    const bool confirmation = result.affected.size() > 1 ||
        result.mutation == PlatformUiPluginMutation::ReloadAll;
    result.decision = confirmation
        ? PlatformUiPreflightDecision::ConfirmationRequired
        : PlatformUiPreflightDecision::Direct;
    if (confirmation && result.reason.empty()) {
        result.reason = "operation affects multiple plugins";
    }
    return result;
}

PlatformUiIntentValidation ValidatePlatformUiIntent(
    const PlatformUiSnapshot& snapshot,
    const PlatformUiIntent& intent,
    const std::unordered_set<std::uint64_t>& submitted_intents) {
    PlatformUiIntentValidation result;
    if (intent.intent_id != 0 && submitted_intents.contains(intent.intent_id)) {
        result.duplicate_intent = true;
        result.code = PlatformUiResultCode::DuplicateIntent;
        result.reason = "intent id has already been submitted";
        return result;
    }
    if (intent.intent_id != 0) {
        const auto duplicate = std::find_if(snapshot.operation_results.begin(),
            snapshot.operation_results.end(), [&](const auto& operation) {
                return operation.intent_id == intent.intent_id;
            });
        if (duplicate != snapshot.operation_results.end()) {
            result.duplicate_intent = true;
            result.code = PlatformUiResultCode::DuplicateIntent;
            result.reason = "intent id already has an operation result";
            return result;
        }
    }
    if (intent.expected_revision != snapshot.revision) {
        result.revision_conflict = true;
        result.code = PlatformUiResultCode::RevisionConflict;
        result.reason = "the UI snapshot revision is stale";
        return result;
    }

    switch (intent.kind) {
    case PlatformUiIntentKind::OpenRoute:
        result.accepted = true;
        result.code = PlatformUiResultCode::Accepted;
        return result;
    case PlatformUiIntentKind::ViewPluginLogs:
        if (!intent.subject_id.empty() && snapshot.FindPlugin(intent.subject_id) == nullptr) {
            result.subject_missing = true;
            result.code = PlatformUiResultCode::SubjectNotFound;
            result.reason = "plugin was not found in the Installed snapshot";
            return result;
        }
        result.accepted = true;
        result.code = PlatformUiResultCode::Accepted;
        return result;
    case PlatformUiIntentKind::PlanPluginMutation:
        if (intent.mutation == PlatformUiPluginMutation::None) {
            result.code = PlatformUiResultCode::InvalidIntent;
            result.reason = "planning intent must specify a plugin mutation";
            return result;
        }
        break;
    default:
        break;
    }

    result.preflight = PreflightPluginMutation(snapshot, intent);
    if (!result.preflight->Allowed()) {
        result.code = PlatformUiResultCode::PreflightBlocked;
        result.reason = result.preflight->reason;
        return result;
    }
    if (result.preflight->RequiresConfirmation() && !intent.confirmed) {
        result.code = PlatformUiResultCode::PreflightRequired;
        result.reason = result.preflight->reason;
        return result;
    }
    result.accepted = true;
    result.code = PlatformUiResultCode::Accepted;
    return result;
}

PlatformUiOperationResult MakeSubmittedOperation(
    const PlatformUiIntent& intent,
    const PlatformUiAffectedSetPreflight* preflight,
    std::uint64_t operation_id,
    std::uint64_t observed_revision) {
    PlatformUiOperationResult result;
    result.operation_id = operation_id;
    result.intent_id = intent.intent_id;
    result.expected_revision = intent.expected_revision;
    result.observed_revision = observed_revision;
    result.state = PlatformUiOperationState::Submitted;
    result.code = PlatformUiResultCode::Accepted;
    result.mutation = ResolvePlatformUiMutation(intent);
    result.subject_id = intent.subject_id;
    if (preflight != nullptr) {
        result.plugins.reserve(preflight->affected.size());
        for (const auto& affected : preflight->affected) {
            result.plugins.push_back({affected.id, PluginOperationOutcome::Skipped,
                PluginOperationReason::AlreadyPending, affected.reason, 0, 0, false});
        }
    }
    return result;
}

void FinalizeOperation(PlatformUiOperationResult& operation) {
    if (operation.plugins.empty()) {
        operation.state = PlatformUiOperationState::Failed;
        operation.code = PlatformUiResultCode::BackendFailure;
        operation.message = "operation returned no per-plugin result";
        operation.retryable = true;
        return;
    }
    std::size_t succeeded{};
    std::size_t failed{};
    std::size_t skipped{};
    for (const auto& plugin : operation.plugins) {
        succeeded += plugin.outcome == PluginOperationOutcome::Succeeded;
        failed += plugin.outcome == PluginOperationOutcome::Failed;
        skipped += plugin.outcome == PluginOperationOutcome::Skipped;
        operation.retryable = operation.retryable || plugin.retryable;
    }
    if (failed == 0 && skipped == 0) {
        operation.state = PlatformUiOperationState::Succeeded;
        operation.code = PlatformUiResultCode::Accepted;
        operation.message = "all plugins succeeded";
    } else if (succeeded == 0 && failed == 0 && skipped != 0) {
        operation.state = PlatformUiOperationState::Cancelled;
        operation.code = PlatformUiResultCode::BackendFailure;
        operation.message = "all plugins were skipped";
    } else if (succeeded == 0) {
        operation.state = PlatformUiOperationState::Failed;
        operation.code = PlatformUiResultCode::BackendFailure;
        operation.message = "all plugins failed";
    } else {
        operation.state = PlatformUiOperationState::PartiallyFailed;
        operation.code = PlatformUiResultCode::BackendFailure;
        operation.message = "some plugins did not succeed";
    }
}

}  // namespace cabbird

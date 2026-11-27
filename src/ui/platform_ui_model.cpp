#include "cabbird/platform_ui_model.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <map>
#include <sstream>
#include <utility>

namespace cabbird {
namespace {

std::string Lower(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(static_cast<char>(std::tolower(character)));
    }
    return result;
}

bool ContainsInsensitive(std::string_view value, std::string_view needle) {
    if (needle.empty()) return true;
    return std::search(value.begin(), value.end(), needle.begin(), needle.end(),
        [](const unsigned char left, const unsigned char right) {
            return std::tolower(left) == std::tolower(right);
        }) != value.end();
}

std::string Join(const std::vector<std::string>& values, std::string_view separator) {
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result.append(separator);
        result += value;
    }
    return result;
}

std::string CatalogEntryId(const PluginCatalogEntry& entry) {
    if (!entry.Id().empty()) return std::string(entry.Id());
    return entry.package_root.filename().string();
}

bool IsBlockingDependency(PluginDependencyState state) noexcept {
    return state != PluginDependencyState::Ready;
}

bool IsRejectedCatalog(PluginCatalogStatus status) noexcept {
    return status == PluginCatalogStatus::InvalidManifest ||
        status == PluginCatalogStatus::InvalidPackage ||
        status == PluginCatalogStatus::DuplicateId;
}

void ComputeSummary(PlatformUiSnapshot& snapshot) {
    snapshot.runtime_summary = {};
    snapshot.runtime_summary.installed = snapshot.installed_plugins.size();
    for (const auto& plugin : snapshot.installed_plugins) {
        if (plugin.IsRunning()) ++snapshot.runtime_summary.running;
        if (plugin.IsDisabled()) ++snapshot.runtime_summary.disabled;
        if (plugin.HasIssue()) ++snapshot.runtime_summary.issues;
    }
}

void SetCatalogState(InstalledPluginView& view, const PluginCatalogEntry& entry) {
    view.has_catalog_entry = true;
    view.catalog_status = entry.status;
    view.catalog_issues = entry.issues;
    if (!entry.issues.empty()) {
        view.catalog_reason = entry.issues.front().message;
    }
    if (entry.manifest) {
        const auto& manifest = *entry.manifest;
        if (view.id.empty()) view.id = manifest.id;
        if (view.name.empty()) view.name = manifest.name;
        view.description = manifest.description;
        if (view.author.empty()) view.author = manifest.author;
        // Repository updates compare against the installed package manifest.
        // A loaded descriptor may still describe the previous generation while
        // the catalog worker is publishing a package replacement.
        view.version = manifest.version.ToString();
        view.audience = manifest.audience;
        if (view.source.empty()) view.source = entry.entry_file;
        if (view.package_directory.empty()) view.package_directory = entry.package_root;
        view.dependencies = manifest.dependencies;
        view.dependency_ids.clear();
        view.dependency_ids.reserve(manifest.dependencies.size());
        for (const auto& dependency : manifest.dependencies) {
            view.dependency_ids.push_back(dependency.id);
        }
    }
    if (entry.compatibility) {
        view.compatibility_result = entry.compatibility;
        view.compatible = entry.compatibility->Compatible();
        view.compatibility_issues = entry.compatibility->issues;
        if (!view.compatible && view.catalog_reason.empty() &&
            !view.compatibility_issues.empty()) {
            view.catalog_reason = view.compatibility_issues.front().path;
        }
    }
}

void SetDependencyState(InstalledPluginView& view, const PluginDependencyPlan& plan) {
    const auto* node = plan.Find(view.id);
    if (node == nullptr) return;
    view.dependency_state = node->state;
    if (view.catalog_reason.empty() && !node->diagnostics.empty()) {
        view.catalog_reason = node->diagnostics.front();
    }
}

void ApplyMetadataState(
    InstalledPluginView& view,
    const PluginCatalogEntry* entry,
    const PluginEnablementDecision* decision) {
    if (entry != nullptr && IsRejectedCatalog(entry->status)) {
        view.enabled = false;
        view.state = entry->status == PluginCatalogStatus::InvalidManifest ||
                entry->status == PluginCatalogStatus::InvalidPackage ||
                entry->status == PluginCatalogStatus::DuplicateId
            ? "rejected"
            : "incompatible";
        if (view.status_reason.empty()) view.status_reason = view.catalog_reason;
        return;
    }
    if (entry != nullptr && entry->status == PluginCatalogStatus::Incompatible) {
        view.enabled = false;
        view.state = "incompatible";
        if (view.status_reason.empty()) view.status_reason = view.catalog_reason;
        return;
    }
    if (IsBlockingDependency(view.dependency_state)) {
        view.enabled = false;
        view.state = "dependency-blocked";
        if (view.status_reason.empty()) view.status_reason = view.catalog_reason;
        return;
    }
    if (decision != nullptr) {
        view.enablement_configured = decision->configured;
        view.enablement_default_enabled = decision->enabled;
        view.enabled = decision->enabled;
        if (!decision->enabled) {
            view.state = "disabled";
            if (view.status_reason.empty()) view.status_reason = decision->reason;
            return;
        }
    }
    if (!view.has_runtime_view && view.state.empty()) {
        view.state = "not-loaded";
        if (view.status_reason.empty()) view.status_reason = "catalog entry is not active";
    }
}

std::string StateSortKey(const InstalledPluginView& plugin) {
    return Lower(plugin.state);
}

std::string SortKey(const InstalledPluginView& plugin, PlatformUiPluginSort sort) {
    switch (sort) {
    case PlatformUiPluginSort::State: return StateSortKey(plugin);
    case PlatformUiPluginSort::Author: return Lower(plugin.author);
    case PlatformUiPluginSort::Name: return Lower(plugin.name);
    }
    return {};
}

bool SamePluginControlState(
    const InstalledPluginView& left, const InstalledPluginView& right) noexcept {
    return left.id == right.id && left.generation == right.generation &&
        left.state == right.state && left.enabled == right.enabled &&
        left.visible == right.visible && left.visibility_control == right.visibility_control &&
        left.has_runtime_view == right.has_runtime_view &&
        left.has_catalog_entry == right.has_catalog_entry &&
        left.catalog_status == right.catalog_status && left.compatible == right.compatible &&
        left.dependency_state == right.dependency_state &&
        std::equal(left.dependencies.begin(), left.dependencies.end(),
            right.dependencies.begin(), right.dependencies.end(), [](const auto& left, const auto& right) {
                return left.id == right.id && left.version_expression == right.version_expression &&
                    left.optional == right.optional;
            }) &&
        left.dependency_ids == right.dependency_ids && left.version == right.version &&
        left.package_directory == right.package_directory && left.source == right.source;
}

bool SameInstalledControlState(
    const PlatformUiSnapshot& left, const PlatformUiSnapshot& right) noexcept {
    if (left.installed_plugins.size() != right.installed_plugins.size()) return false;
    // Snapshot builders publish canonical order. The fast path makes normal
    // telemetry refresh linear; a reordered provider still describes the same state.
    if (std::equal(left.installed_plugins.begin(), left.installed_plugins.end(),
            right.installed_plugins.begin(), SamePluginControlState)) {
        return true;
    }
    return std::is_permutation(left.installed_plugins.begin(), left.installed_plugins.end(),
        right.installed_plugins.begin(), right.installed_plugins.end(), SamePluginControlState);
}

}  // namespace

PlatformUiPluginState InstalledPluginView::UiState() const noexcept {
    if (has_catalog_entry) {
        if (catalog_status == PluginCatalogStatus::Incompatible) {
            return PlatformUiPluginState::Incompatible;
        }
        if (IsRejectedCatalog(catalog_status)) return PlatformUiPluginState::Rejected;
    }
    if (dependency_state != PluginDependencyState::Ready) {
        return PlatformUiPluginState::DependencyBlocked;
    }
    const std::string normalized = Lower(this->state);
    if (normalized == "active" || normalized == "running") return PlatformUiPluginState::Active;
    if (normalized == "loaded") return PlatformUiPluginState::Loaded;
    if (normalized == "disabled" || normalized == "not-loaded") {
        return PlatformUiPluginState::Disabled;
    }
    if (normalized == "faulted" || normalized == "fault") return PlatformUiPluginState::Faulted;
    if (normalized == "quarantined" || normalized == "quarantine") {
        return PlatformUiPluginState::Quarantined;
    }
    if (normalized == "waiting-for-service" || normalized == "waiting") {
        return PlatformUiPluginState::WaitingForService;
    }
    if (normalized == "rejected") return PlatformUiPluginState::Rejected;
    if (normalized == "dependency-blocked" || normalized == "blocked") {
        return PlatformUiPluginState::DependencyBlocked;
    }
    if (normalized == "incompatible") return PlatformUiPluginState::Incompatible;
    if (normalized == "stopping") return PlatformUiPluginState::Stopping;
    if (!enabled && !has_runtime_view) return PlatformUiPluginState::Disabled;
    return PlatformUiPluginState::Unknown;
}

bool InstalledPluginView::IsRunning() const noexcept {
    return enabled && UiState() == PlatformUiPluginState::Active;
}

bool InstalledPluginView::IsDisabled() const noexcept {
    const auto typed_state = UiState();
    // Rejected, blocked, and quarantined entries are issues,
    // not ordinary user-disabled entries, even though they cannot run.
    return typed_state == PlatformUiPluginState::Disabled ||
        (typed_state == PlatformUiPluginState::Unknown && !enabled && status_reason.empty());
}

bool InstalledPluginView::HasIssue() const noexcept {
    switch (UiState()) {
    case PlatformUiPluginState::Faulted:
    case PlatformUiPluginState::Quarantined:
    case PlatformUiPluginState::WaitingForService:
    case PlatformUiPluginState::Rejected:
    case PlatformUiPluginState::DependencyBlocked:
    case PlatformUiPluginState::Incompatible:
    case PlatformUiPluginState::Stopping:
        return true;
    default:
        break;
    }
    return enabled && !status_reason.empty() && !IsRunning();
}

std::string_view InstalledPluginView::Reason() const noexcept {
    if (!status_reason.empty()) return status_reason;
    if (!catalog_reason.empty()) return catalog_reason;
    switch (UiState()) {
    case PlatformUiPluginState::Rejected: return "catalog entry was rejected";
    case PlatformUiPluginState::Incompatible: return "plugin is incompatible with this runtime";
    case PlatformUiPluginState::DependencyBlocked: return "a dependency is unavailable";
    case PlatformUiPluginState::WaitingForService: return "required service is not ready";
    default: return {};
    }
}

RepositoryPluginInstallState ResolveRepositoryPluginInstallState(
    const InstalledPluginView* installed,
    const RepositoryPluginView& available) {
    RepositoryPluginInstallState state;
    state.installed = installed != nullptr;
    if (installed != nullptr) {
        const auto current = ParseSemanticVersion(installed->version);
        const auto candidate = ParseSemanticVersion(available.entry.version);
        state.update_available = current && candidate &&
            CompareSemanticVersionPrecedence(*current, *candidate) < 0;
    }
    return state;
}

void UnityCompatibilitySnapshot::NormalizeLevel() noexcept {
    // No id list: `Supported` is tied to the rows this snapshot actually carries.  An
    // empty matrix is never Supported -- "nothing was declared" is not "everything
    // worked", and reporting the second for the first is the failure this rule exists to
    // prevent.
    bool any_available = false;
    bool all_validated = !features.empty();
    for (const auto& feature : features) {
        any_available = any_available || feature.available;
        all_validated = all_validated && feature.available && feature.validated;
    }
    if (level == UnityCompatibilityLevel::Supported && !all_validated) {
        level = any_available ? UnityCompatibilityLevel::Partial : UnityCompatibilityLevel::CoreOnly;
    }
    if (level == UnityCompatibilityLevel::Partial && !any_available) {
        level = UnityCompatibilityLevel::CoreOnly;
    }
}

const UnityFeatureSnapshot* UnityCompatibilitySnapshot::FindFeature(std::string_view id) const noexcept {
    const auto found = std::find_if(features.begin(), features.end(), [&](const auto& feature) {
        return feature.id == id;
    });
    return found == features.end() ? nullptr : &*found;
}

const InstalledPluginView* PlatformUiSnapshot::FindPlugin(std::string_view id) const noexcept {
    const auto found = std::find_if(installed_plugins.begin(), installed_plugins.end(),
        [&](const auto& plugin) { return plugin.id == id; });
    return found == installed_plugins.end() ? nullptr : &*found;
}

std::string_view ToString(PlatformUiRoute route) noexcept {
    switch (route) {
    case PlatformUiRoute::Plugins: return "plugins";
    case PlatformUiRoute::UnityCompatibility: return "unity_compatibility";
    case PlatformUiRoute::Diagnostics: return "diagnostics";
    case PlatformUiRoute::Settings: return "settings";
    }
    return "unknown";
}

std::string_view ToString(PlatformUiPluginState state) noexcept {
    switch (state) {
    case PlatformUiPluginState::Unknown: return "unknown";
    case PlatformUiPluginState::Active: return "active";
    case PlatformUiPluginState::Loaded: return "loaded";
    case PlatformUiPluginState::Disabled: return "disabled";
    case PlatformUiPluginState::Faulted: return "faulted";
    case PlatformUiPluginState::Quarantined: return "quarantined";
    case PlatformUiPluginState::WaitingForService: return "waiting-for-service";
    case PlatformUiPluginState::Rejected: return "rejected";
    case PlatformUiPluginState::DependencyBlocked: return "dependency-blocked";
    case PlatformUiPluginState::Incompatible: return "incompatible";
    case PlatformUiPluginState::Stopping: return "stopping";
    }
    return "unknown";
}

std::string_view ToString(PlatformUiResultCode code) noexcept {
    switch (code) {
    case PlatformUiResultCode::None: return "none";
    case PlatformUiResultCode::Accepted: return "accepted";
    case PlatformUiResultCode::InvalidIntent: return "invalid-intent";
    case PlatformUiResultCode::DuplicateIntent: return "duplicate-intent";
    case PlatformUiResultCode::RevisionConflict: return "revision-conflict";
    case PlatformUiResultCode::SubjectNotFound: return "subject-not-found";
    case PlatformUiResultCode::PreflightRequired: return "preflight-required";
    case PlatformUiResultCode::PreflightBlocked: return "preflight-blocked";
    case PlatformUiResultCode::Busy: return "busy";
    case PlatformUiResultCode::ProviderUnavailable: return "provider-unavailable";
    case PlatformUiResultCode::BackendFailure: return "backend-failure";
    }
    return "invalid";
}

std::string_view ToString(PluginOperationReason reason) noexcept {
    switch (reason) {
    case PluginOperationReason::None: return "none";
    case PluginOperationReason::Active: return "active";
    case PluginOperationReason::Disabled: return "disabled";
    case PluginOperationReason::NotFound: return "not-found";
    case PluginOperationReason::Rejected: return "rejected";
    case PluginOperationReason::Incompatible: return "incompatible";
    case PluginOperationReason::DependencyBlocked: return "dependency-blocked";
    case PluginOperationReason::RevisionConflict: return "revision-conflict";
    case PluginOperationReason::DuplicateIntent: return "duplicate-intent";
    case PluginOperationReason::PreflightRequired: return "preflight-required";
    case PluginOperationReason::ProviderUnavailable: return "provider-unavailable";
    case PluginOperationReason::BackendFailure: return "backend-failure";
    case PluginOperationReason::GenerationChanged: return "generation-changed";
    case PluginOperationReason::NotSupported: return "not-supported";
    case PluginOperationReason::AlreadyPending: return "already-pending";
    }
    return "unknown";
}

PlatformUiPluginState ClassifyPluginState(std::string_view state) noexcept {
    InstalledPluginView view;
    view.state = std::string(state);
    view.has_runtime_view = true;
    return view.UiState();
}

PlatformUiPluginMutation ResolvePlatformUiMutation(
    const PlatformUiIntent& intent) noexcept {
    if (intent.kind == PlatformUiIntentKind::SetPluginEnabled) {
        return intent.bool_value ? PlatformUiPluginMutation::Enable
                                 : PlatformUiPluginMutation::Disable;
    }
    if (intent.kind == PlatformUiIntentKind::ReloadPlugin) {
        return PlatformUiPluginMutation::Reload;
    }
    if (intent.kind == PlatformUiIntentKind::ReloadAllInstalled) {
        return PlatformUiPluginMutation::ReloadAll;
    }
    if (intent.kind == PlatformUiIntentKind::SetPluginVisible) {
        return PlatformUiPluginMutation::SetVisible;
    }
    return intent.mutation;
}

bool IsMutationIntent(PlatformUiIntentKind kind) noexcept {
    return kind == PlatformUiIntentKind::PlanPluginMutation ||
        kind == PlatformUiIntentKind::SetPluginEnabled ||
        kind == PlatformUiIntentKind::ReloadPlugin ||
        kind == PlatformUiIntentKind::ReloadAllInstalled ||
        kind == PlatformUiIntentKind::SetPluginVisible;
}

bool MatchesPluginSearch(const InstalledPluginView& plugin, std::string_view search) noexcept {
    if (search.empty()) return true;
    return ContainsInsensitive(plugin.id, search) || ContainsInsensitive(plugin.name, search) ||
        ContainsInsensitive(plugin.description, search) ||
        ContainsInsensitive(plugin.author, search) || ContainsInsensitive(plugin.version, search) ||
        ContainsInsensitive(plugin.state, search) || ContainsInsensitive(plugin.status_reason, search) ||
        ContainsInsensitive(plugin.catalog_reason, search);
}

bool MatchesPluginFilter(
    const InstalledPluginView& plugin, PlatformUiPluginFilter filter,
    const std::unordered_set<std::string>& favorites) noexcept {
    switch (filter) {
    case PlatformUiPluginFilter::All: return true;
    case PlatformUiPluginFilter::Running: return plugin.IsRunning();
    case PlatformUiPluginFilter::Disabled: return plugin.IsDisabled();
    case PlatformUiPluginFilter::Issues: return plugin.HasIssue();
    case PlatformUiPluginFilter::Favorites: return favorites.contains(plugin.id);
    }
    return false;
}

std::vector<InstalledPluginView> FilterAndSortPlugins(
    const std::vector<InstalledPluginView>& plugins,
    std::string_view search,
    PlatformUiPluginFilter filter,
    PlatformUiPluginSort sort,
    bool include_developer,
    const std::unordered_set<std::string>& favorites) {
    struct SortEntry final {
        const InstalledPluginView* plugin;
        std::string key;
        std::string id;
    };
    std::vector<SortEntry> sorted;
    sorted.reserve(plugins.size());
    for (const auto& plugin : plugins) {
        if (!include_developer && plugin.audience == PluginAudience::Developer) continue;
        if (MatchesPluginSearch(plugin, search) && MatchesPluginFilter(plugin, filter, favorites)) {
            sorted.push_back({&plugin, SortKey(plugin, sort), Lower(plugin.id)});
        }
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        if (left.key != right.key) return left.key < right.key;
        return left.id < right.id;
    });
    std::vector<InstalledPluginView> result;
    result.reserve(sorted.size());
    for (const auto& entry : sorted) result.push_back(*entry.plugin);
    return result;
}

std::vector<std::string> FilteredPluginIds(
    const std::vector<InstalledPluginView>& plugins,
    std::string_view search,
    PlatformUiPluginFilter filter,
    PlatformUiPluginSort sort,
    bool include_developer,
    const std::unordered_set<std::string>& favorites) {
    const auto filtered = FilterAndSortPlugins(
        plugins, search, filter, sort, include_developer, favorites);
    std::vector<std::string> result;
    result.reserve(filtered.size());
    for (const auto& plugin : filtered) result.push_back(plugin.id);
    return result;
}

std::size_t PlatformUiPluginFilterCounts::For(PlatformUiPluginFilter filter) const noexcept {
    switch (filter) {
    case PlatformUiPluginFilter::All: return all;
    case PlatformUiPluginFilter::Running: return running;
    case PlatformUiPluginFilter::Disabled: return disabled;
    case PlatformUiPluginFilter::Issues: return issues;
    case PlatformUiPluginFilter::Favorites: return favorites;
    }
    return 0;
}

PlatformUiPluginFilterCounts CountFilteredPlugins(
    const std::vector<InstalledPluginView>& plugins, std::string_view search,
    bool include_developer, const std::unordered_set<std::string>& favorites) {
    PlatformUiPluginFilterCounts result;
    for (const auto& plugin : plugins) {
        if (!include_developer && plugin.audience == PluginAudience::Developer) continue;
        if (!MatchesPluginSearch(plugin, search)) continue;
        ++result.all;
        if (plugin.IsRunning()) ++result.running;
        if (plugin.IsDisabled()) ++result.disabled;
        if (plugin.HasIssue()) ++result.issues;
        if (favorites.contains(plugin.id)) ++result.favorites;
    }
    return result;
}

std::string ReconcilePluginSelection(
    const std::vector<InstalledPluginView>& sorted_plugins,
    std::string_view previous_selection) {
    if (!previous_selection.empty()) {
        const auto found = std::find_if(sorted_plugins.begin(), sorted_plugins.end(),
            [&](const auto& plugin) { return plugin.id == previous_selection; });
        if (found != sorted_plugins.end()) return std::string(previous_selection);
    }
    return sorted_plugins.empty() ? std::string{} : sorted_plugins.front().id;
}

void ReconcileUiStateSelection(
    const PlatformUiSnapshot& snapshot, PlatformUiState& state,
    bool include_developer) {
    const auto sorted = FilterAndSortPlugins(
        snapshot.installed_plugins, state.search, state.plugin_filter, state.plugin_sort,
        include_developer, state.favorite_plugin_ids);
    state.selected_plugin_id = ReconcilePluginSelection(sorted, state.selected_plugin_id);
}

PlatformUiSnapshot BuildPlatformUiSnapshot(
    std::uint64_t revision, std::vector<unitymem::PluginView> runtime_plugins) {
    PlatformUiSnapshot snapshot;
    snapshot.revision = revision;
    snapshot.installed_plugins.reserve(runtime_plugins.size());
    for (auto& runtime : runtime_plugins) {
        InstalledPluginView view(std::move(runtime));
        view.has_catalog_entry = false;
        view.catalog_status = PluginCatalogStatus::Valid;
        snapshot.installed_plugins.push_back(std::move(view));
    }
    std::sort(snapshot.installed_plugins.begin(), snapshot.installed_plugins.end(),
        [](const auto& left, const auto& right) { return Lower(left.id) < Lower(right.id); });
    ComputeSummary(snapshot);
    return snapshot;
}

PlatformUiSnapshot BuildPlatformUiSnapshot(
    std::uint64_t revision,
    std::vector<unitymem::PluginView> runtime_plugins,
    const PluginCatalogSnapshot& catalog,
    const PluginDependencyPlan& dependencies,
    const std::map<std::string, PluginEnablementDecision, std::less<>>& enablement,
    const PluginDisplayNameMap& display_names,
    const PluginDescriptionMap& descriptions) {
    PlatformUiSnapshot snapshot;
    snapshot.revision = revision;

    std::map<std::string, InstalledPluginView, std::less<>> merged;
    for (auto& runtime : runtime_plugins) {
        if (runtime.id.empty()) continue;
        InstalledPluginView view(std::move(runtime));
        view.catalog_status = PluginCatalogStatus::Valid;
        merged[view.id] = std::move(view);
    }

    for (const auto& entry : catalog.Entries()) {
        const std::string id = CatalogEntryId(entry);
        if (id.empty()) continue;
        auto found = merged.find(id);
        if (found == merged.end()) {
            InstalledPluginView view;
            view.id = id;
            view.name = entry.manifest ? entry.manifest->name : id;
            view.author = entry.manifest ? entry.manifest->author : std::string{};
            view.version = entry.manifest ? entry.manifest->version.ToString() : std::string{};
            view.source = entry.entry_file;
            view.package_directory = entry.package_root;
            view.state.clear();
            view.enabled = false;
            view.visible = false;
            found = merged.emplace(id, std::move(view)).first;
        }
        auto& view = found->second;
        SetCatalogState(view, entry);
        SetDependencyState(view, dependencies);
        const auto decision = enablement.find(id);
        ApplyMetadataState(view, &entry, decision == enablement.end() ? nullptr : &decision->second);
    }

    for (auto& [id, view] : merged) {
        const auto display_name = display_names.find(id);
        if (display_name != display_names.end() && !display_name->second.empty()) {
            view.name = display_name->second;
        }
        const auto description = descriptions.find(id);
        if (description != descriptions.end() && !description->second.empty()) {
            view.description = description->second;
        }
        if (!view.has_catalog_entry) {
            if (view.state.empty()) view.state = view.enabled ? "active" : "disabled";
            continue;
        }
        if (view.state.empty()) view.state = view.has_runtime_view ? "loaded" : "not-loaded";
    }
    snapshot.installed_plugins.reserve(merged.size());
    for (auto& [id, view] : merged) snapshot.installed_plugins.push_back(std::move(view));
    std::sort(snapshot.installed_plugins.begin(), snapshot.installed_plugins.end(),
        [](const auto& left, const auto& right) { return Lower(left.id) < Lower(right.id); });
    ComputeSummary(snapshot);
    return snapshot;
}

UnityCompatibilitySnapshot BuildUnityCompatibilitySnapshot(
    const UnityProfileDocumentStatus& document,
    std::vector<AvailableServiceVersion> services) {
    UnityCompatibilitySnapshot result;
    result.services = std::move(services);
    result.game_id = document.game;
    result.build_id = document.build_id;
    result.profile_source = document.source;
    result.profile_hash = document.hash;

    if (!document.loaded) {
        // Unknown, not CoreOnly.  "Only the core is available" would be a claim about the
        // GAME; this says the PROFILE DATA is missing, which is the fact the operator can act
        // on and the one the old page hid behind "the running build is not known".
        result.level = UnityCompatibilityLevel::Unknown;
        result.reason = document.error.empty()
            ? "no Unity profile document is present"
            : document.error;
        return result;
    }

    result.features.reserve(document.methods.size());
    std::size_t bound = 0;
    for (const auto& method : document.methods) {
        UnityFeatureSnapshot feature;
        feature.id = method.id;
        feature.available = method.bound;
        // A reported binding is already verified: the metadata route hands back the live
        // MethodInfo's own pointer, and the profile route re-reads the recorded prologue
        // before it returns an address.  So here `validated` is not a second opinion, it is
        // the same fact -- and an unbound method carries the reason instead.
        feature.validated = method.bound;
        feature.reason = method.how;
        if (method.bound) ++bound;
        result.features.push_back(std::move(feature));
    }

    if (result.features.empty()) {
        result.level = UnityCompatibilityLevel::CoreOnly;
        result.reason = "the Unity profile declares no methods";
    } else if (bound == result.features.size()) {
        result.level = UnityCompatibilityLevel::Supported;
        result.reason = "every method the Unity profile declares is bound";
    } else if (bound != 0) {
        result.level = UnityCompatibilityLevel::Partial;
        result.reason = "some methods the Unity profile declares are not bound";
    } else {
        result.level = UnityCompatibilityLevel::CoreOnly;
        result.reason = "the Unity profile loaded without a bound method";
    }
    // NormalizeLevel() is deliberately NOT called here.  Its job is to hold `Supported`
    // down when a row that EXISTS is not validated, and this builder has already applied
    // exactly that rule by counting bound methods.  Calling it would be a second opinion
    // on the same facts.
    return result;
}

PlatformUiModel::PlatformUiModel() = default;

PlatformUiModel::PlatformUiModel(PlatformUiSnapshot snapshot)
    : snapshot_(std::move(snapshot)) {
    ReconcileUiStateSelection(snapshot_, state_);
}

void PlatformUiModel::Publish(PlatformUiSnapshot snapshot) {
    snapshot.revision = snapshot_.revision +
        (snapshot_.revision == 0 || !SameInstalledControlState(snapshot_, snapshot) ? 1U : 0U);
    snapshot_ = std::move(snapshot);
    ReconcileUiStateSelection(snapshot_, state_);
    pending_operations_.erase(
        std::remove_if(pending_operations_.begin(), pending_operations_.end(), [&](const auto& pending) {
            return std::any_of(snapshot_.operation_results.begin(), snapshot_.operation_results.end(),
                [&](const auto& completed) { return completed.operation_id == pending.operation_id; });
        }), pending_operations_.end());
}

std::vector<InstalledPluginView> PlatformUiModel::VisiblePlugins(
    bool include_developer) const {
    return FilterAndSortPlugins(snapshot_.installed_plugins, state_.search,
        state_.plugin_filter, state_.plugin_sort, include_developer, state_.favorite_plugin_ids);
}

PlatformUiPluginFilterCounts PlatformUiModel::FilterCounts(
    bool include_developer) const {
    return CountFilteredPlugins(
        snapshot_.installed_plugins, state_.search, include_developer, state_.favorite_plugin_ids);
}

PlatformUiIntent PlatformUiModel::NewIntent(
    PlatformUiIntentKind kind, std::string subject_id, PlatformUiPluginMutation mutation) {
    PlatformUiIntent intent;
    intent.intent_id = next_intent_id_++;
    intent.expected_revision = snapshot_.revision;
    intent.kind = kind;
    intent.mutation = mutation;
    intent.subject_id = std::move(subject_id);
    return intent;
}

PlatformUiIntentSubmission PlatformUiModel::Submit(PlatformUiIntent intent) {
    if (intent.intent_id == 0) intent.intent_id = next_intent_id_++;
    else next_intent_id_ = std::max(next_intent_id_, intent.intent_id + 1);
    if (intent.expected_revision == 0 && snapshot_.revision == 0) {
        intent.expected_revision = snapshot_.revision;
    }
    const auto validation = ValidatePlatformUiIntent(snapshot_, intent, submitted_intents_);
    PlatformUiIntentSubmission result;
    result.code = validation.code;
    result.reason = validation.reason;
    result.preflight = validation.preflight;
    if (!validation.accepted) return result;
    result.accepted = true;
    result.operation_id = next_operation_id_++;
    pending_operations_.push_back(MakeSubmittedOperation(
        intent,
        validation.preflight ? &*validation.preflight : nullptr,
        result.operation_id,
        snapshot_.revision));
    submitted_intents_.insert(intent.intent_id);
    return result;
}

void PlatformUiModel::ApplyOperationResult(PlatformUiOperationResult result) {
    pending_operations_.erase(
        std::remove_if(pending_operations_.begin(), pending_operations_.end(), [&](const auto& pending) {
            const bool operation_match = result.operation_id != 0 &&
                pending.operation_id == result.operation_id;
            const bool intent_match = result.intent_id != 0 &&
                pending.intent_id == result.intent_id;
            return operation_match || intent_match;
        }), pending_operations_.end());
    snapshot_.operation_results.push_back(std::move(result));
}

}  // namespace cabbird

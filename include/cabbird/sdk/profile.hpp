#pragma once

#include "cabbird/sdk/cpp.hpp"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <span>
#include <charconv>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cabbird::sdk {

using ProfileValues = std::map<std::string, std::uint32_t, std::less<>>;

struct ProfileCandidate final {
    std::string pattern;
    std::vector<std::uint8_t> entry;
    std::optional<std::uint32_t> call_offset;
    ProfileValues values;
};

struct ProfileSymbol final {
    std::string name;
    std::vector<ProfileCandidate> candidates;
};

// Expected semantic identity supplied by the consumer; never a build/version selector.
struct ProfileContract final {
    std::string_view game, module, section, contract;
};

// Plugin-owned snapshot. No file I/O or host handles remain after LoadProfile.
struct Profile final {
    std::string game, module, section, contract;
    ProfileValues layout;
    std::vector<ProfileSymbol> symbols;

    [[nodiscard]] bool Matches(const ProfileContract& expected) const noexcept {
        return game == expected.game && module == expected.module &&
               section == expected.section && contract == expected.contract;
    }

    [[nodiscard]] std::uint32_t Value(std::string_view key, std::uint32_t maximum,
                                     std::uint32_t alignment = 1) const {
        const auto found = layout.find(key);
        if (found == layout.end() || found->second > maximum || alignment == 0 ||
            found->second % alignment != 0)
            throw std::runtime_error("unsupported profile layout field: " + std::string(key));
        return found->second;
    }

    [[nodiscard]] const ProfileSymbol& symbol(std::string_view name) const {
        for (const auto& item : symbols) if (item.name == name) return item;
        throw std::out_of_range("undeclared profile symbol");
    }
};

namespace detail {

class ProfileJsonNode final {
public:
    ProfileJsonNode(const CabbirdJsonServiceV1* service, CabbirdGenerationHandleV1 handle)
        : service_(service), handle_(handle) {}
    ~ProfileJsonNode() { if (handle_.id != 0) service_->release(service_->user, handle_); }
    ProfileJsonNode(const ProfileJsonNode&) = delete;
    ProfileJsonNode& operator=(const ProfileJsonNode&) = delete;
    ProfileJsonNode(ProfileJsonNode&& other) noexcept
        : service_(other.service_), handle_(std::exchange(other.handle_, {})) {}

    static void Check(const CabbirdStatusV1 status) {
        if (!Succeeded(status)) throw std::runtime_error(status.message.data != nullptr
            ? std::string(status.message.data, status.message.size) : "profile service failed");
    }
    [[nodiscard]] ProfileJsonNode member(std::string_view key) const {
        CabbirdGenerationHandleV1 child{};
        Check(service_->object_find(service_->user, handle_, StringView(key), &child));
        return {service_, child};
    }
    [[nodiscard]] std::optional<ProfileJsonNode> optional_member(std::string_view key) const {
        CabbirdGenerationHandleV1 child{};
        const auto status = service_->object_find(service_->user, handle_, StringView(key), &child);
        if (status.code == CABBIRD_STATUS_V1_NOT_FOUND) return std::nullopt;
        Check(status);
        return ProfileJsonNode(service_, child);
    }
    [[nodiscard]] ProfileJsonNode item(std::size_t index) const {
        CabbirdGenerationHandleV1 child{};
        Check(service_->array_item(service_->user, handle_, index, &child));
        return {service_, child};
    }
    [[nodiscard]] std::size_t size() const {
        std::size_t count{};
        Check(service_->array_size(service_->user, handle_, &count));
        return count;
    }
    [[nodiscard]] std::string string() const {
        return CopyString([&](char* out, std::size_t* size) {
            return service_->string_value(service_->user, handle_, out, size);
        });
    }
    [[nodiscard]] std::uint32_t number() const {
        double value{};
        Check(service_->number_value(service_->user, handle_, &value));
        // The host schema limits numeric profile values to uint32 integers.
        return static_cast<std::uint32_t>(value);
    }
    [[nodiscard]] ProfileValues values() const {
        std::size_t count{};
        Check(service_->object_size(service_->user, handle_, &count));
        ProfileValues result;
        for (std::size_t i{}; i < count; ++i) {
            auto key = CopyString([&](char* out, std::size_t* size) {
                return service_->object_key_at(service_->user, handle_, i, out, size);
            });
            const auto value = member(key).number();
            result.emplace(std::move(key), value);
        }
        return result;
    }

private:
    template <typename Copy> static std::string CopyString(Copy&& copy) {
        std::size_t size{};
        Check(copy(nullptr, &size));
        if (size == 0 || size > 1024U * 1024U)
            throw std::runtime_error("profile string exceeds service limits");
        std::string value(size, '\0');
        Check(copy(value.data(), &size));
        value.resize(size - 1);
        return value;
    }
    const CabbirdJsonServiceV1* service_;
    CabbirdGenerationHandleV1 handle_;
};

inline CabbirdStatusV1 ResolveProfileRva(
    const CabbirdCoreServiceV1* core, const Profile& profile,
    const ProfileCandidate& candidate, std::uintptr_t* address) {
    *address = 0;
    if (core == nullptr || core->struct_size < offsetof(CabbirdCoreServiceV1, module_base) +
            sizeof(core->module_base) || core->module_base == nullptr)
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    const auto found = candidate.values.find("rva");
    if (found == candidate.values.end() || found->second == 0)
        return {CABBIRD_STATUS_V1_NOT_FOUND, 0, {}};
    const auto base = core->module_base(core->user, StringView(profile.module));
    if (base == 0) return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    *address = base + found->second;
    return Ok();
}

}  // namespace detail

class ProfileResult final {
public:
    [[nodiscard]] explicit operator bool() const noexcept { return profile_.has_value(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] const Profile& value() const & { return profile_.value(); }
    [[nodiscard]] Profile&& value() && { return std::move(profile_.value()); }
private:
    friend ProfileResult LoadProfile(const Host&, std::string_view);
    friend ProfileResult LoadProfile(const Host&, std::string_view, const ProfileContract&);
    std::optional<Profile> profile_;
    std::string error_;
};

[[nodiscard]] inline ProfileResult LoadProfile(const Host& host, std::string_view filename) {
    ProfileResult result;
    try {
        const auto profiles = host.Query<CabbirdProfileServiceV1>(CABBIRD_PROFILE_SERVICE_V1_ID);
        const auto json = host.Query<CabbirdJsonServiceV1>(CABBIRD_JSON_SERVICE_V1_ID);
        if (!profiles || profiles->struct_size < sizeof(CabbirdProfileServiceV1) ||
            profiles->load == nullptr || !json || json->struct_size < sizeof(CabbirdJsonServiceV1) ||
            json->release == nullptr || json->object_find == nullptr || json->array_item == nullptr ||
            json->array_size == nullptr || json->object_size == nullptr || json->object_key_at == nullptr ||
            json->number_value == nullptr || json->string_value == nullptr)
            throw std::runtime_error("profile and JSON services are unavailable");
        CabbirdGenerationHandleV1 handle{};
        detail::ProfileJsonNode::Check(profiles->load(profiles->user, StringView(filename), &handle));
        const detail::ProfileJsonNode root(json.get(), handle);
        Profile profile;
        profile.game = root.member("game").string();
        profile.module = root.member("module").string();
        profile.section = root.member("section").string();
        profile.contract = root.member("contract").string();
        profile.layout = root.member("layout").values();
        const auto symbols = root.member("symbols");
        const auto symbol_count = symbols.size();
        for (std::size_t i{}; i < symbol_count; ++i) {
            const auto source = symbols.item(i);
            ProfileSymbol symbol;
            symbol.name = source.member("name").string();
            const auto candidates = source.member("candidates");
            const auto candidate_count = candidates.size();
            for (std::size_t c{}; c < candidate_count; ++c) {
                const auto candidate_source = candidates.item(c);
                ProfileCandidate candidate;
                if (const auto pattern = candidate_source.optional_member("pattern"))
                    candidate.pattern = pattern->string();
                candidate.values = candidate_source.member("values").values();
                if (const auto call = candidate_source.optional_member("callOffset"))
                    candidate.call_offset = call->number();
                const auto entry = candidate_source.member("entry").string();
                for (std::size_t byte{}; byte < entry.size(); byte += 3) {
                    unsigned value{};
                    const auto parsed = std::from_chars(entry.data() + byte, entry.data() + byte + 2, value, 16);
                    if (parsed.ec != std::errc{} || parsed.ptr != entry.data() + byte + 2)
                        throw std::runtime_error("profile entry is not hexadecimal bytes");
                    candidate.entry.push_back(static_cast<std::uint8_t>(value));
                }
                symbol.candidates.push_back(std::move(candidate));
            }
            profile.symbols.push_back(std::move(symbol));
        }
        result.profile_ = std::move(profile);
    } catch (const std::exception& error) {
        result.error_ = error.what();
    }
    return result;
}

// Same host-owned parsing path, with caller-supplied semantic contract validation.
[[nodiscard]] inline ProfileResult LoadProfile(const Host& host, std::string_view filename,
                                               const ProfileContract& expected) {
    auto result = LoadProfile(host, filename);
    if (result && !result.value().Matches(expected)) {
        result.profile_.reset();
        result.error_ = "profile semantic contract does not match";
    }
    return result;
}

struct ProfileSymbolResult final {
    std::uintptr_t address{};
    const ProfileCandidate* candidate{};  // Borrowed from the supplied Profile.
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return address != 0; }
};

// Uses existing scoped signature and memory services. A match is published only
// when exactly one candidate passes its declared entry-byte check.
template <typename Resolve>
[[nodiscard]] inline ProfileSymbolResult ResolveProfileSymbolWith(
    const CabbirdCoreServiceV1* core, const Profile& profile,
    std::string_view name, Resolve&& resolve) {
    if (core == nullptr || core->struct_size < offsetof(CabbirdCoreServiceV1, read_memory) + sizeof(core->read_memory) ||
        core->service_version < CABBIRD_CORE_SERVICE_V1_VERSION || core->read_memory == nullptr)
        return {0, nullptr, "signature or memory service is unavailable"};
    const ProfileSymbol* symbol{};
    for (const auto& item : profile.symbols) if (item.name == name) { symbol = &item; break; }
    if (symbol == nullptr) return {0, nullptr, "undeclared profile symbol"};
    ProfileSymbolResult result;
    std::string last_candidate_error;
    for (const auto& candidate : symbol->candidates) {
        std::uintptr_t address{};
        const auto scan_status = resolve(candidate, &address);
        if (scan_status.code == CABBIRD_STATUS_V1_CONFLICT)
            return {0, nullptr, "profile signature is ambiguous"};
        if (!Succeeded(scan_status)) {
            last_candidate_error = "profile candidate resolution status=" + std::to_string(scan_status.code);
            continue;
        }
        if (address == 0) {
            last_candidate_error = "signature scan returned an empty address";
            continue;
        }
        if (candidate.call_offset) {
            std::array<std::uint8_t, 5> call{};
            if (!Succeeded(core->read_memory(core->user, address + *candidate.call_offset,
                    {call.data(), call.size()})) || call[0] != 0xE8) continue;
            std::int32_t displacement{};
            std::memcpy(&displacement, call.data() + 1, sizeof(displacement));
            address += *candidate.call_offset + call.size() + displacement;
        }
        if (candidate.entry.empty()) {
            last_candidate_error = "profile candidate has no entry bytes";
            continue;
        }
        std::vector<std::uint8_t> actual(candidate.entry.size());
        if (!Succeeded(core->read_memory(core->user, address, {actual.data(), actual.size()}))) {
            last_candidate_error = "candidate entry bytes could not be read";
            continue;
        }
        if (actual != candidate.entry) {
            last_candidate_error = "candidate entry bytes do not match";
            continue;
        }
        if (result.address != 0) return {0, nullptr, "multiple profile candidates matched"};
        result.address = address;
        result.candidate = &candidate;
    }
    if (!result) result.error = last_candidate_error.empty()
        ? "no profile candidate passed validation"
        : std::move(last_candidate_error);
    return result;
}

[[nodiscard]] inline ProfileSymbolResult ResolveProfileSymbol(
    const CabbirdSignatureServiceV1* signatures, const CabbirdCoreServiceV1* core,
    const Profile& profile, std::string_view name) {
    if (signatures == nullptr || signatures->struct_size < sizeof(CabbirdSignatureServiceV1) ||
        signatures->service_version < CABBIRD_SIGNATURE_SERVICE_V1_VERSION || signatures->resolve == nullptr)
        return {0, nullptr, "signature or memory service is unavailable"};
    return ResolveProfileSymbolWith(core, profile, name,
        [&](const ProfileCandidate& candidate, std::uintptr_t* address) {
            if (candidate.pattern.empty())
                return detail::ResolveProfileRva(core, profile, candidate, address);
            return signatures->resolve(signatures->user, StringView(profile.module),
                StringView(profile.section), StringView(candidate.pattern), address);
        });
}

// Known RVAs use the module's actual base and the same entry-byte/unique-candidate
// checks as scans. Short prologues do not become global scan signatures.
[[nodiscard]] inline ProfileSymbolResult ResolveProfileSymbolRva(
    const CabbirdCoreServiceV1* core, const Profile& profile, std::string_view name) {
    return ResolveProfileSymbolWith(core, profile, name,
        [&](const ProfileCandidate& candidate, std::uintptr_t* address) {
            return detail::ResolveProfileRva(core, profile, candidate, address);
        });
}

[[nodiscard]] inline bool MatchesProfileEntry(const CabbirdCoreServiceV1* core, const Profile& profile, std::string_view symbol,
                                std::uintptr_t address) noexcept {
    try {
        if (!core || core->struct_size < offsetof(CabbirdCoreServiceV1, read_memory) + sizeof(core->read_memory) ||
            core->service_version < CABBIRD_CORE_SERVICE_V1_VERSION || !core->read_memory || !address) return false;
        for (const auto& candidate : profile.symbol(symbol).candidates) {
            std::vector<std::uint8_t> actual(candidate.entry.size());
            if (actual.empty() || !Succeeded(core->read_memory(core->user, address,
                    {actual.data(), actual.size()}))) continue;
            if (actual == candidate.entry) return true;
        }
    } catch (...) {}
    return false;
}


// Per-start scan results. Entry bytes, E8 targets and unique candidates are
// still checked by the SDK resolver when each dependent feature starts.
class ProfileScanCache final {
public:
    void Prepare(const Host& host, const Profile& profile,
                 std::span<const std::string_view> symbols, bool prefer_batch = true) {
        results_.clear();
        batch_ = false;
        for (const auto& symbol : profile.symbols) {
            if (std::find(symbols.begin(), symbols.end(), symbol.name) == symbols.end()) continue;
            for (const auto& candidate : symbol.candidates)
                if (!candidate.pattern.empty()) results_.try_emplace(candidate.pattern);
        }
        const auto v3 = prefer_batch
            ? host.Query<CabbirdSignatureServiceV3>(CABBIRD_SIGNATURE_SERVICE_V3_ID,
                CABBIRD_SIGNATURE_SERVICE_V3_VERSION)
            : ServiceRef<CabbirdSignatureServiceV3>{};
        if (v3 && v3->struct_size >= offsetof(CabbirdSignatureServiceV3, resolve_batch) +
                sizeof(v3->resolve_batch) && v3->resolve_batch) {
            batch_ = true;
            std::vector<CabbirdStringViewV1> patterns;
            std::vector<Result*> outputs;
            for (auto& [pattern, result] : results_) {
                patterns.push_back(StringView(pattern));
                outputs.push_back(&result);
            }
            for (std::size_t begin = 0; begin < patterns.size(); begin += CABBIRD_SIGNATURE_BATCH_MAX_PATTERNS_V3) {
                const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(
                    patterns.size() - begin, CABBIRD_SIGNATURE_BATCH_MAX_PATTERNS_V3));
                std::vector<std::uint32_t> statuses(count, CABBIRD_STATUS_V1_UNAVAILABLE);
                std::vector<std::uintptr_t> addresses(count);
                const auto status = v3->resolve_batch(v3->user, StringView(profile.module),
                    StringView(profile.section), patterns.data() + begin, count,
                    statuses.data(), addresses.data());
                if (!Succeeded(status)) { batch_ = false; break; }
                for (std::size_t i = 0; i < count; ++i)
                    *outputs[begin + i] = Result{statuses[i], statuses[i] == CABBIRD_STATUS_V1_OK ? addresses[i] : 0};
            }
            if (batch_) return;
            for (auto& [pattern, result] : results_) result = {};
        }
        const auto v1 = host.Query<CabbirdSignatureServiceV1>(CABBIRD_SIGNATURE_SERVICE_V1_ID);
        if (!v1 || v1->struct_size < sizeof(CabbirdSignatureServiceV1) ||
            v1->service_version < CABBIRD_SIGNATURE_SERVICE_V1_VERSION || !v1->resolve) return;
        for (auto& [pattern, result] : results_) {
            result.status = v1->resolve(v1->user, StringView(profile.module),
                StringView(profile.section), StringView(pattern), &result.address).code;
            if (result.status != CABBIRD_STATUS_V1_OK) result.address = 0;
        }
    }

    ProfileSymbolResult Resolve(const CabbirdCoreServiceV1* core,
            const Profile& profile, std::string_view name) const {
        return ResolveProfileSymbolWith(core, profile, name,
            [&](const ProfileCandidate& candidate, std::uintptr_t* address) -> CabbirdStatusV1 {
                if (candidate.pattern.empty())
                    return detail::ResolveProfileRva(core, profile, candidate, address);
                const auto found = results_.find(candidate.pattern);
                if (found == results_.end()) return {CABBIRD_STATUS_V1_NOT_FOUND, 0, {}};
                *address = found->second.address;
                return {found->second.status, 0, {}};
            });
    }
    bool batched() const noexcept { return batch_; }
    std::size_t size() const noexcept { return results_.size(); }
    void Clear() { results_.clear(); batch_ = false; }

private:
    struct Result { std::uint32_t status{CABBIRD_STATUS_V1_UNAVAILABLE}; std::uintptr_t address{}; };
    std::map<std::string, Result, std::less<>> results_;
    bool batch_{};
};


// Plugin-owned values and resolved addresses. Load on Lifecycle/Worker before
// publishing callbacks. No host handles remain; do not mutate while callbacks read.
// Failed loads preserve the previous snapshot. Check Load's result before use.
class ProfileSnapshot final {
public:
    [[nodiscard]] bool Load(const CabbirdHostApiV1* api, std::string_view filename,
                            const ProfileContract& expected,
                            std::initializer_list<std::string_view> known_entries = {},
                            bool batch_signatures = true,
                            std::string_view layout_symbol = {},
                            std::string* diagnostic = nullptr,
                            bool require_layout_symbol = true) noexcept {
        const auto set_diagnostic = [diagnostic](const std::string_view message) noexcept {
            if (diagnostic == nullptr) return;
            try { diagnostic->assign(message); } catch (...) {}
        };
        try {
            const Host host(api);
            auto loaded = LoadProfile(host, filename, expected);
            if (!loaded) {
                set_diagnostic(loaded.error());
                return false;
            }
            auto parsed = std::move(loaded).value();
            std::vector<std::string_view> scanned_symbols;
            for (const auto& symbol : parsed.symbols) {
                if (std::find(known_entries.begin(), known_entries.end(), symbol.name) == known_entries.end())
                    scanned_symbols.push_back(symbol.name);
            }
            if (!layout_symbol.empty()) {
                static_cast<void>(parsed.symbol(layout_symbol)); // Require the declared discriminator.
                if (require_layout_symbol &&
                    std::find(scanned_symbols.begin(), scanned_symbols.end(), layout_symbol) == scanned_symbols.end())
                    return false;
            }
            ProfileScanCache scans;
            scans.Prepare(host, parsed, scanned_symbols, batch_signatures);
            const auto core = host.Query<CabbirdCoreServiceV1>(CABBIRD_CORE_SERVICE_V1_ID);
            std::map<std::string, std::uintptr_t, std::less<>> addresses;
            std::map<std::string, std::size_t, std::less<>> selections;
            for (const auto name : scanned_symbols) {
                const auto resolved = scans.Resolve(core.get(), parsed, name);
                if (name == layout_symbol && !resolved && require_layout_symbol) {
                    std::string message = "required profile layout symbol did not resolve: ";
                    message.append(name);
                    message += "; ";
                    message += resolved.error;
                    set_diagnostic(message);
                    return false;
                }
                addresses.emplace(name, resolved.address);
                if (resolved) selections.emplace(name,
                    static_cast<std::size_t>(resolved.candidate - parsed.symbol(name).candidates.data()));
                if (resolved && name == layout_symbol)
                    for (const auto& [key, value] : resolved.candidate->values)
                        parsed.layout.insert_or_assign(key, value);
            }
            profile_ = std::move(parsed);
            addresses_ = std::move(addresses);
            selections_ = std::move(selections);
            set_diagnostic({});
            return true;
        } catch (const std::exception& error) {
            set_diagnostic(error.what());
            return false;
        } catch (...) {
            set_diagnostic("unknown profile snapshot load failure");
            return false;
        }
    }

    [[nodiscard]] const Profile& Document() const noexcept { return profile_; }
    [[nodiscard]] std::uintptr_t Address(std::string_view symbol) const noexcept {
        const auto found = addresses_.find(symbol);
        return found == addresses_.end() ? 0 : found->second;
    }
    [[nodiscard]] const ProfileCandidate* Selected(std::string_view symbol) const {
        const auto found = selections_.find(symbol);
        return found == selections_.end() ? nullptr : &profile_.symbol(symbol).candidates.at(found->second);
    }
    [[nodiscard]] std::string_view Pattern(std::string_view symbol) const {
        const auto* candidate = Selected(symbol);
        if (candidate) return candidate->pattern;
        // An unresolved single seed is useful to member-learning validators,
        // but does not authorize using any address or unvalidated layout.
        const auto& candidates = profile_.symbol(symbol).candidates;
        return candidates.size() == 1 ? std::string_view(candidates.front().pattern) : std::string_view{};
    }
    [[nodiscard]] bool MatchesEntry(const CabbirdCoreServiceV1* core, std::string_view symbol,
                                    std::uintptr_t address) const noexcept {
        return MatchesProfileEntry(core, profile_, symbol, address);
    }
    [[nodiscard]] std::uint32_t Value(std::string_view key, std::uint32_t maximum,
                                     std::uint32_t alignment = 1) const {
        return profile_.Value(key, maximum, alignment);
    }
private:
    Profile profile_;
    std::map<std::string, std::uintptr_t, std::less<>> addresses_;
    std::map<std::string, std::size_t, std::less<>> selections_;
};

}  // namespace cabbird::sdk

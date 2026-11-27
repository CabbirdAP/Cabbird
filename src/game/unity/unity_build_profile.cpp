/* UnityBuildProfile -- see the header for why addresses are data and not constants. */

#include "cabbird/unity_build_profile.hpp"

#include "cabbird/il2cpp.hpp"
#include "cabbird/unity_metadata.hpp"
#include "cabbird/memory.hpp"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <set>
#include <cstring>
#include <fstream>

namespace cabbird {
namespace {

using Json = nlohmann::json;

// Parses "48895C24..." (or with spaces/dashes) into bytes.  Returns false rather than
// truncating on a malformed string: a half-read prologue would compare unequal and be
// reported as a version mismatch, which is a wrong diagnosis for a typo in a config file.
bool ParseHexBytes(std::string_view text, std::vector<unsigned char>* out) {
    out->clear();
    std::string digits;
    digits.reserve(text.size());
    for (const char character : text) {
        if (character == ' ' || character == '-' || character == '\t') continue;
        digits.push_back(character);
    }
    if (digits.empty() || digits.size() % 2 != 0) return false;
    for (std::size_t index = 0; index < digits.size(); index += 2) {
        const auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int high = nibble(digits[index]);
        const int low = nibble(digits[index + 1]);
        if (high < 0 || low < 0) return false;
        out->push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return true;
}

bool ParseRva(const Json& value, std::uint64_t* out) {
    if (value.is_number_unsigned()) {
        *out = value.get<std::uint64_t>();
        return true;
    }
    if (!value.is_string()) return false;
    std::string_view text = value.get_ref<const std::string&>();
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *out, base);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

// Module base and size, or false when the module is not loaded.  The size check is what makes
// a method pointer trustworthy: a code pointer lands inside the module's range and heap
// garbage almost never does.
bool ModuleBounds(const std::wstring& name, std::uintptr_t* base, std::uintptr_t* size) {
    const HMODULE module = GetModuleHandleW(name.c_str());
    if (module == nullptr) return false;
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const unsigned char*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (base != nullptr) *base = reinterpret_cast<std::uintptr_t>(module);
    if (size != nullptr) *size = nt->OptionalHeader.SizeOfImage;
    return true;
}

bool ExecutableRange(std::uintptr_t address, std::size_t size) noexcept {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) == 0 ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    const DWORD protection = region.Protect & 0xFF;
    if (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
        return false;
    const auto offset = address - reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    return offset < region.RegionSize && size <= region.RegionSize - offset;
}

std::string Hex(std::uint64_t value) {
    constexpr char kDigits[] = "0123456789ABCDEF";
    if (value == 0) return "0";
    std::string text;
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const auto nibble = static_cast<unsigned>((value >> shift) & 0xFULL);
        if (nibble != 0 || started) {
            started = true;
            text.push_back(kDigits[nibble]);
        }
    }
    return text;
}

std::string HexBytes(const unsigned char* bytes, std::size_t count) {
    std::string text;
    text.reserve(count * 3);
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) text.push_back(' ');
        text += Hex(bytes[index]);
    }
    return text;
}

// Managed resolution is used only from an already admitted Game tick. Lifecycle
// uses the independently verified profile RVA to bootstrap that very frame clock.
UnityResolvedMethod ResolveThroughMetadata(const UnityBuildProfile::EntryView& entry,
                                           const std::string& assembly_name) {
    UnityResolvedMethod result;
    std::vector<CabbirdStringViewV1> parameters;
    for (const auto& parameter : entry.parameter_types)
        parameters.push_back({parameter.data(), parameter.size()});
    CabbirdUnityMethodRequestV1 request{};
    request.struct_size = sizeof(request);
    request.method_kind = entry.is_static ? CABBIRD_UNITY_METHOD_V1_STATIC : CABBIRD_UNITY_METHOD_V1_INSTANCE;
    request.module_name = {assembly_name.data(), assembly_name.size()};
    request.type_name = {entry.type_name.data(), entry.type_name.size()};
    request.method_name = {entry.method_name.data(), entry.method_name.size()};
    request.return_type = {entry.return_type.data(), entry.return_type.size()};
    request.parameter_types = parameters.data();
    request.parameter_count = parameters.size();
    CabbirdUnityMethodResultV1 method{sizeof(method)};
    const auto status = ResolveUnityMetadataMethod(&request, &method);
    if (status.code != CABBIRD_STATUS_V1_OK) {
        result.how = status.message.data ? std::string(status.message.data, status.message.size)
                                         : "metadata resolution failed";
        return result;
    }
    result.resolved = true;
    result.address = method.address;
    result.rva = method.rva;
    result.how = "metadata";
    result.detail = entry.type_name + "." + entry.method_name + " " + entry.signature;
    return result;
}

// Profile route: use the recorded RVA, but only after re-reading the recorded prologue bytes
// from the live process.  A relocated or reshaped function therefore fails safe -- refused,
// with a diff in the reason -- instead of being hooked.
UnityResolvedMethod ResolveThroughProfile(const UnityBuildProfile::EntryView& entry,
                                          const std::wstring& assembly_name) {
    UnityResolvedMethod result;
    result.rva = entry.rva;
    result.detail = entry.type_name + "." + entry.method_name;
    if (entry.rva == 0) {
        result.how = "profile has no rva for this method";
        return result;
    }
    if (entry.prologue.empty()) {
        result.how = "profile fallback requires verified prologue bytes";
        return result;
    }

    std::uintptr_t module_base = 0;
    std::uintptr_t module_size = 0;
    if (!ModuleBounds(assembly_name, &module_base, &module_size)) {
        result.how = "module not loaded";
        return result;
    }
    if (entry.rva >= module_size || entry.prologue.size() > module_size - entry.rva) {
        result.how = "profile rva 0x" + Hex(entry.rva) + " is outside the module (size 0x" +
                     Hex(module_size) + "); profile does not match this build";
        return result;
    }

    const auto address = module_base + entry.rva;
    if (!ExecutableRange(address, entry.prologue.size())) {
        result.how = "profile address is not executable module memory";
        return result;
    }
    if (!entry.prologue.empty()) {
        const auto bytes = mem::ReadMemory(address, entry.prologue.size());
        if (!bytes.has_value() || bytes->size() != entry.prologue.size()) {
            result.how = "cannot read 0x" + Hex(address) + " to verify the prologue";
            return result;
        }
        if (std::memcmp(bytes->data(), entry.prologue.data(), entry.prologue.size()) != 0) {
            result.how = "prologue mismatch at 0x" + Hex(address) + ": expected [" +
                         HexBytes(entry.prologue.data(), entry.prologue.size()) + "] found [" +
                         HexBytes(bytes->data(), bytes->size()) + "]";
            return result;
        }
    }
    result.resolved = true;
    result.address = address;
    result.how = "profile";
    return result;
}

}  // namespace

std::filesystem::path DefaultUnityBuildProfilePath(const std::filesystem::path& runtime_root) {
    return runtime_root / L"profiles" / L"unity-build-profiles.json";
}

UnityBuildProfile UnityBuildProfile::LoadFromFile(const std::filesystem::path& path,
                                                  std::string* error) {
    return Select(path, {}, error);
}

UnityBuildProfile UnityBuildProfile::Select(const std::filesystem::path& path,
                                            std::string_view preferred_id,
                                            std::string* error) {
    UnityBuildProfile profile;
    const auto fail = [&](std::string message) {
        profile.valid_ = false;
        profile.entries_.clear();
        profile.error_ = std::move(message);
        if (error != nullptr) *error = profile.error_;
        return profile;
    };
    std::ifstream stream(path);
    if (!stream) return fail("cannot open " + path.string());
    try {
        Json document;
        stream >> document;
        if (!document.is_object() || !document.contains("schema") ||
            !document["schema"].is_number_integer() || document["schema"] != 1) {
            return fail("Unity profile requires schema 1");
        }
        const auto profiles = document.find("profiles");
        if (profiles == document.end() || !profiles->is_array() || profiles->empty()) {
            return fail("no nonempty \"profiles\" array in " + path.string());
        }
        const Json* selected = nullptr;
        std::set<std::string> ids;
        for (const auto& candidate : *profiles) {
            if (!candidate.is_object()) return fail("profile entry must be an object");
            const auto id = candidate.value("id", std::string{});
            if (id.empty()) return fail("profile entry has no \"id\"");
            if (!ids.insert(id).second) return fail("duplicate profile id: " + id);
            if ((!selected && preferred_id.empty()) || id == preferred_id) selected = &candidate;
        }
        if (selected == nullptr) {
            return fail("requested profile \"" + std::string(preferred_id) + "\" was not found");
        }
        profile.build_id_ = selected->at("id").get<std::string>();
        profile.assembly_name_ = selected->value("gameAssembly", std::string{"GameAssembly.dll"});
        if (profile.assembly_name_.empty()) return fail("profile has an empty gameAssembly");
        const auto methods = selected->find("methods");
        if (methods == selected->end() || !methods->is_object() || methods->empty()) {
            return fail("profile \"" + profile.build_id_ + "\" has no nonempty methods object");
        }
        for (auto iterator = methods->begin(); iterator != methods->end(); ++iterator) {
            const auto& value = iterator.value();
            if (!value.is_object()) return fail("method \"" + iterator.key() + "\" must be an object");
            Entry parsed;
            parsed.key = iterator.key();
            parsed.type_name = value.value("type", std::string{});
            parsed.method_name = value.value("method", std::string{});
            parsed.signature = value.value("signature", std::string{});
            if (parsed.key.empty() || parsed.type_name.empty() || parsed.method_name.empty()) {
                return fail("method \"" + parsed.key + "\" is missing its key, type or method");
            }
            parsed.return_type = value.at("returnType").get<std::string>();
            parsed.parameter_types = value.at("parameterTypes").get<std::vector<std::string>>();
            parsed.is_static = value.at("static").get<bool>();
            if (parsed.return_type.empty() || parsed.parameter_types.size() > 256 ||
                std::any_of(parsed.parameter_types.begin(), parsed.parameter_types.end(),
                            [](const std::string& type) { return type.empty(); }))
                return fail("method \"" + parsed.key + "\" has an incomplete metadata shape");
            parsed.parameter_count = static_cast<int>(parsed.parameter_types.size());
            const auto parameter_count = value.find("parameterCount");
            if (parameter_count != value.end()) {
                if (!parameter_count->is_number_integer() || *parameter_count < 0 ||
                    *parameter_count > 256) {
                    return fail("method \"" + parsed.key + "\" has an invalid parameterCount");
                }
                if (parameter_count->get<int>() != parsed.parameter_count)
                    return fail("method \"" + parsed.key + "\" has inconsistent parameterCount");
            }
            const auto rva = value.find("rva");
            if (rva != value.end() && !ParseRva(*rva, &parsed.rva)) {
                return fail("method \"" + parsed.key + "\" has an unreadable rva");
            }
            const auto prologue = value.find("prologue");
            if (prologue != value.end() && (!prologue->is_string() ||
                !ParseHexBytes(prologue->get_ref<const std::string&>(), &parsed.prologue))) {
                return fail("method \"" + parsed.key + "\" has a malformed prologue hex string");
            }
            profile.entries_.push_back(std::move(parsed));
        }
        profile.valid_ = true;
        if (error != nullptr) error->clear();
        return profile;
    } catch (const std::exception& exception) {
        return fail("cannot parse " + path.string() + ": " + exception.what());
    }
}

std::vector<std::string> UnityBuildProfile::MethodKeys() const {
    std::vector<std::string> keys;
    keys.reserve(entries_.size());
    for (const auto& entry : entries_) keys.push_back(entry.key);
    return keys;
}

std::vector<unsigned char> UnityBuildProfile::PrologueFor(std::string_view key) const {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [key](const Entry& entry) { return entry.key == key; });
    if (found == entries_.end()) return {};
    return found->prologue;
}

UnityResolvedMethod UnityBuildProfile::Resolve(std::string_view key) const {
    if (!valid_) {
        UnityResolvedMethod result;
        result.how = "profile is not loaded: " + error_;
        return result;
    }
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [key](const Entry& entry) { return entry.key == key; });
    if (found == entries_.end()) {
        UnityResolvedMethod result;
        result.how = "profile \"" + build_id_ + "\" has no method \"" + std::string(key) + "\"";
        return result;
    }

    EntryView view{found->type_name, found->method_name, found->signature, found->rva,
                   found->prologue, found->parameter_count, found->return_type,
                   found->parameter_types, found->is_static};
    return ResolveThroughProfile(view, std::wstring(assembly_name_.begin(), assembly_name_.end()));
}

UnityResolvedMethod UnityBuildProfile::ResolveInGame(std::string_view key) const {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [key](const Entry& entry) { return entry.key == key; });
    if (!valid_ || found == entries_.end()) return Resolve(key);
    EntryView view{found->type_name, found->method_name, found->signature, found->rva,
                   found->prologue, found->parameter_count, found->return_type,
                   found->parameter_types, found->is_static};
    auto by_metadata = ResolveThroughMetadata(view, assembly_name_);
    if (by_metadata.resolved) return by_metadata;
    auto by_profile = Resolve(key);
    if (!by_profile.resolved)
        by_profile.how = "metadata: " + by_metadata.how + "; profile: " + by_profile.how;
    return by_profile;
}

}  // namespace cabbird

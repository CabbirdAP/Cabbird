#include "cabbird/unity_metadata.hpp"

#include "cabbird/il2cpp.hpp"
#include "cabbird/memory.hpp"
#include "cabbird/thread_local_value.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cabbird {
namespace {

ThreadLocalObject<std::string> g_status_message;
CabbirdStatusV1 Status(std::uint32_t code, std::string_view message = {}) {
    auto& stored = g_status_message.Get();
    stored.assign(message);
    return {code, 0, {stored.data(), stored.size()}};
}

bool ValidText(CabbirdStringViewV1 text) noexcept {
    return text.data != nullptr && text.size != 0 && text.size <= 512 &&
        std::memchr(text.data, 0, text.size) == nullptr;
}

std::string Copy(CabbirdStringViewV1 text) { return {text.data, text.size}; }

std::optional<std::string> TypeName(const il2cpp::Api& api, const il2cpp::Il2CppType* type) {
    if (type == nullptr) return std::nullopt;
    const char* name = api.il2cpp_type_get_name(type);
    if (name == nullptr) return std::nullopt;
    struct OwnedName {
        const il2cpp::Api& api;
        const char* value;
        ~OwnedName() { api.il2cpp_free(const_cast<char*>(value)); }
    } owned{api, name};
    return std::string(name);
}

CabbirdStatusV1 FindClass(const il2cpp::Api& api, std::string_view full_name,
                          il2cpp::Il2CppClass** found) {
    *found = nullptr;
    const auto* domain = api.il2cpp_domain_get();
    if (domain == nullptr) return Status(CABBIRD_STATUS_V1_UNAVAILABLE, "IL2CPP domain is not ready");
    std::size_t count = 0;
    const auto* assemblies = api.il2cpp_domain_get_assemblies(domain, &count);
    if (assemblies == nullptr) return Status(CABBIRD_STATUS_V1_UNAVAILABLE, "IL2CPP images are unavailable");
    const auto dot = full_name.rfind('.');
    const std::string namespaze(full_name.substr(0, dot == std::string_view::npos ? 0 : dot));
    const std::string name(full_name.substr(dot == std::string_view::npos ? 0 : dot + 1));
    for (std::size_t index = 0; index < count; ++index) {
        if (assemblies[index] == nullptr) continue;
        auto* image = api.il2cpp_assembly_get_image(assemblies[index]);
        if (image == nullptr) continue;
        auto* klass = api.il2cpp_class_from_name(image, namespaze.c_str(), name.c_str());
        if (klass == nullptr) continue;
        if (*found != nullptr && *found != klass)
            return Status(CABBIRD_STATUS_V1_CONFLICT, "type name is ambiguous across loaded images");
        *found = klass;
    }
    return *found ? Status(CABBIRD_STATUS_V1_OK)
                  : Status(CABBIRD_STATUS_V1_NOT_FOUND,
                      "type " + std::string(full_name) + " was not found in " +
                      std::to_string(count) + " loaded IL2CPP images");
}

bool LookupAvailable(const il2cpp::Api& api) noexcept {
    return il2cpp::Ready() && api.il2cpp_domain_get && api.il2cpp_domain_get_assemblies &&
        api.il2cpp_assembly_get_image && api.il2cpp_class_from_name &&
        api.il2cpp_type_get_name && api.il2cpp_free;
}

bool ExecutableInModule(std::uintptr_t address, const MODULEINFO& module) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(module.lpBaseOfDll);
    if (address < base || address - base >= module.SizeOfImage) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) == 0 ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        reinterpret_cast<std::uintptr_t>(memory.AllocationBase) != base) return false;
    switch (memory.Protect & 0xFF) {
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
        return true;
    default: return false;
    }
}

}  // namespace

CabbirdStatusV1 ResolveUnityMetadataMethod(
    const CabbirdUnityMethodRequestV1* request, CabbirdUnityMethodResultV1* result) {
    if (request == nullptr || result == nullptr || request->struct_size < sizeof(*request) ||
        result->struct_size < sizeof(*result) || !ValidText(request->module_name) ||
        !ValidText(request->type_name) || !ValidText(request->method_name) ||
        !ValidText(request->return_type) || request->parameter_count > 256 ||
        (request->parameter_count != 0 && request->parameter_types == nullptr) ||
        request->method_kind > CABBIRD_UNITY_METHOD_V1_STATIC)
        return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "invalid method metadata request");
    *result = {sizeof(*result), 0, 0, 0};
    const auto& api = il2cpp::Functions();
    if (!LookupAvailable(api) || !api.il2cpp_class_get_methods || !api.il2cpp_method_get_name ||
        !api.il2cpp_method_get_param_count || !api.il2cpp_method_get_param ||
        !api.il2cpp_method_get_return_type || !api.il2cpp_method_get_flags)
        return Status(CABBIRD_STATUS_V1_UNAVAILABLE, "IL2CPP method metadata APIs are unavailable");
    std::vector<std::string> parameters;
    parameters.reserve(request->parameter_count);
    for (std::size_t index = 0; index < request->parameter_count; ++index) {
        if (!ValidText(request->parameter_types[index]))
            return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "invalid method parameter type");
        parameters.push_back(Copy(request->parameter_types[index]));
    }
    const auto module_name = Copy(request->module_name);
    const std::wstring wide_module_name(module_name.begin(), module_name.end());
    // Resolve only the requested image. Enumerating every loaded module and its
    // path per method made the first plugin binding block the Game thread.
    const auto module_handle = GetModuleHandleW(wide_module_name.c_str());
    MODULEINFO module{};
    if (!module_handle || !K32GetModuleInformation(GetCurrentProcess(), module_handle, &module, sizeof(module)))
        return Status(CABBIRD_STATUS_V1_UNAVAILABLE, "method native module is not loaded");
    il2cpp::Il2CppClass* klass = nullptr;
    const auto lookup = FindClass(api, Copy(request->type_name), &klass);
    if (lookup.code != CABBIRD_STATUS_V1_OK) return lookup;
    const auto method_name = Copy(request->method_name);
    const auto return_type = Copy(request->return_type);
    const il2cpp::MethodInfo* selected = nullptr;
    std::size_t named_methods = 0;
    void* iterator = nullptr;
    while (const auto* method = api.il2cpp_class_get_methods(klass, &iterator)) {
        const auto* name = api.il2cpp_method_get_name(method);
        if (name == nullptr || method_name != name) continue;
        ++named_methods;
        if (api.il2cpp_method_get_param_count(method) != parameters.size()) continue;
        std::uint32_t implementation_flags = 0;
        constexpr std::uint32_t kStaticMethodFlag = 0x10;
        const bool is_static = (api.il2cpp_method_get_flags(method, &implementation_flags) & kStaticMethodFlag) != 0;
        if (is_static != (request->method_kind == CABBIRD_UNITY_METHOD_V1_STATIC) ||
            TypeName(api, api.il2cpp_method_get_return_type(method)) != return_type) continue;
        bool matches = true;
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (TypeName(api, api.il2cpp_method_get_param(method, static_cast<std::uint32_t>(index))) != parameters[index]) {
                matches = false;
                break;
            }
        }
        if (!matches) continue;
        if (selected != nullptr && selected != method)
            return Status(CABBIRD_STATUS_V1_CONFLICT, "method shape matches multiple entries");
        selected = method;
    }
    if (selected == nullptr)
        return Status(CABBIRD_STATUS_V1_NOT_FOUND,
            Copy(request->type_name) + "." + method_name + ": " +
            std::to_string(named_methods) + " named entries; no complete match for " +
            (request->method_kind == CABBIRD_UNITY_METHOD_V1_STATIC ? "static " : "instance ") +
            return_type + " with " + std::to_string(parameters.size()) + " parameters");
    // The supported IL2CPP MethodInfo starts with methodPointer. This host-owned
    // layout rule is also used by the Unity build resolver and type-system dumper;
    // plugins never read or reproduce it.
    std::uintptr_t address = 0;
    if (!mem::ReadMemoryInto(reinterpret_cast<std::uintptr_t>(selected), &address, sizeof(address)) ||
        !ExecutableInModule(address, module))
        return Status(CABBIRD_STATUS_V1_UNAVAILABLE, "method native entry is not executable inside its declared module");
    result->address = address;
    result->rva = address - reinterpret_cast<std::uintptr_t>(module.lpBaseOfDll);
    return Status(CABBIRD_STATUS_V1_OK);
}

CabbirdStatusV1 ResolveUnityMetadataInstanceField(
    const CabbirdUnityFieldRequestV1* request, CabbirdUnityFieldResultV1* result) {
    if (request == nullptr || result == nullptr || request->struct_size < sizeof(*request) ||
        result->struct_size < sizeof(*result) || request->storage_size == 0 ||
        !ValidText(request->type_name) || !ValidText(request->field_name) || !ValidText(request->field_type))
        return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT, "invalid instance-field metadata request");
    *result = {sizeof(*result), 0, 0, 0};
    const auto& api = il2cpp::Functions();
    if (!LookupAvailable(api) || !api.il2cpp_field_get_type)
        return Status(CABBIRD_STATUS_V1_UNAVAILABLE, "IL2CPP field metadata APIs are unavailable");
    il2cpp::Il2CppClass* klass = nullptr;
    const auto lookup = FindClass(api, Copy(request->type_name), &klass);
    if (lookup.code != CABBIRD_STATUS_V1_OK) return lookup;
    const auto field_name = Copy(request->field_name);
    const auto field = il2cpp::ResolveInstanceField(klass, field_name, request->storage_size);
    if (!field.metadata_available)
        return Status(CABBIRD_STATUS_V1_UNAVAILABLE, "IL2CPP instance-field layout APIs are unavailable");
    if (!field.offset || *field.offset > (std::numeric_limits<std::uint32_t>::max)())
        return Status(CABBIRD_STATUS_V1_NOT_FOUND, Copy(request->type_name) + "." + field_name +
            ": no instance field of the declared size inside the class (property names are not field names)");
    if (TypeName(api, field.type) != Copy(request->field_type))
        return Status(CABBIRD_STATUS_V1_NOT_FOUND, "instance-field type did not match");
    result->offset = static_cast<std::uint32_t>(*field.offset);
    result->storage_size = request->storage_size;
    return Status(CABBIRD_STATUS_V1_OK);
}

}  // namespace cabbird

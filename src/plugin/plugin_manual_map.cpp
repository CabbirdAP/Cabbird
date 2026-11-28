#include "cabbird/plugin_manual_map.hpp"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cabbird {
namespace {

constexpr std::size_t kMaximumImageBytes = 256ULL * 1024ULL * 1024ULL;
constexpr WORD kMaximumSections = 96U;
constexpr std::size_t kMaximumTlsCallbacks = 4096U;
constexpr std::size_t kMaximumForwardDepth = 8U;

using DllMainFn = BOOL(WINAPI*)(HMODULE, DWORD, LPVOID);

// Process-wide table of manually mapped plugin images so _CxxThrowException
// can supply ExceptionInformation[3] = ImageBase without PEB->Ldr lookup.
// Kept as plain zero-initialized POD globals with no static constructor or
// destructor: this translation unit is linked into manually mapped Core
// (TLS-free), and quarantined images may still throw late during shutdown.
struct MappedImageRange final {
    std::uintptr_t begin{};
    std::uintptr_t end{};
};

SRWLOCK g_mapped_ranges_lock = SRWLOCK_INIT;
MappedImageRange* g_mapped_ranges = nullptr;
std::size_t g_mapped_range_count = 0;
std::size_t g_mapped_range_capacity = 0;

bool RegisterMappedImageRange(std::uintptr_t base, std::size_t size) noexcept {
    if (base == 0 || size == 0 || base > (std::numeric_limits<std::uintptr_t>::max)() - size) {
        return false;
    }
    AcquireSRWLockExclusive(&g_mapped_ranges_lock);
    bool registered = false;
    if (g_mapped_range_count == g_mapped_range_capacity) {
        const std::size_t next_capacity =
            g_mapped_range_capacity == 0 ? 16U : g_mapped_range_capacity * 2U;
        if (next_capacity <= (std::numeric_limits<std::size_t>::max)() / sizeof(MappedImageRange)) {
            HANDLE heap = GetProcessHeap();
            void* grown = g_mapped_ranges == nullptr
                ? HeapAlloc(heap, 0, next_capacity * sizeof(MappedImageRange))
                : HeapReAlloc(heap, 0, g_mapped_ranges, next_capacity * sizeof(MappedImageRange));
            if (grown != nullptr) {
                g_mapped_ranges = static_cast<MappedImageRange*>(grown);
                g_mapped_range_capacity = next_capacity;
            }
        }
    }
    if (g_mapped_range_count < g_mapped_range_capacity) {
        g_mapped_ranges[g_mapped_range_count++] = MappedImageRange{base, base + size};
        registered = true;
    }
    ReleaseSRWLockExclusive(&g_mapped_ranges_lock);
    return registered;
}

void UnregisterMappedImageRange(std::uintptr_t base) noexcept {
    if (base == 0) return;
    AcquireSRWLockExclusive(&g_mapped_ranges_lock);
    for (std::size_t index = 0; index < g_mapped_range_count; ++index) {
        if (g_mapped_ranges[index].begin == base) {
            g_mapped_ranges[index] = g_mapped_ranges[--g_mapped_range_count];
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_mapped_ranges_lock);
}

std::uintptr_t LookupMappedImageBase(const void* address) noexcept {
    if (address == nullptr) return 0;
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    std::uintptr_t found{};
    AcquireSRWLockShared(&g_mapped_ranges_lock);
    for (std::size_t index = 0; index < g_mapped_range_count; ++index) {
        const MappedImageRange& range = g_mapped_ranges[index];
        if (target >= range.begin && target < range.end) {
            found = range.begin;
            break;
        }
    }
    ReleaseSRWLockShared(&g_mapped_ranges_lock);
    return found;
}

extern "C" __declspec(noreturn) void WINAPI CabbirdMappedPluginCxxThrow(
    void* exception_object, const void* throw_info) {
    constexpr DWORD kMsvcCxxException = 0xE06D7363UL;
    constexpr ULONG_PTR kMsvcEhMagicNumber1 = 0x19930520UL;
    std::uintptr_t image_base{};
    if (throw_info != nullptr) {
        image_base = LookupMappedImageBase(throw_info);
        if (image_base == 0) {
            PVOID loader_base{};
            if (RtlPcToFileHeader(const_cast<void*>(throw_info), &loader_base) != nullptr) {
                image_base = reinterpret_cast<std::uintptr_t>(loader_base);
            }
        }
    }
    const ULONG_PTR parameters[]{
        kMsvcEhMagicNumber1,
        reinterpret_cast<ULONG_PTR>(exception_object),
        reinterpret_cast<ULONG_PTR>(throw_info),
        static_cast<ULONG_PTR>(image_base),
    };
    RaiseException(
        kMsvcCxxException, EXCEPTION_NONCONTINUABLE,
        static_cast<DWORD>(std::size(parameters)), parameters);
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

class FileHandle final {
public:
    explicit FileHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~FileHandle() {
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

[[nodiscard]] bool EqualInsensitive(
    std::wstring_view left, std::wstring_view right) noexcept {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(), [](wchar_t a, wchar_t b) {
            return std::towlower(a) == std::towlower(b);
        });
}

[[nodiscard]] bool HasPrefix(std::wstring_view value, std::wstring_view prefix) noexcept {
    return value.size() >= prefix.size() &&
        EqualInsensitive(value.substr(0, prefix.size()), prefix);
}

[[nodiscard]] bool IsApiSetModule(std::wstring_view module_name) noexcept {
    return HasPrefix(module_name, L"api-ms-win-") ||
        HasPrefix(module_name, L"ext-ms-win-");
}

[[nodiscard]] bool IsCrtModule(std::wstring_view module_name) noexcept {
    return HasPrefix(module_name, L"vcruntime") || HasPrefix(module_name, L"msvcp") ||
        HasPrefix(module_name, L"ucrtbase") || HasPrefix(module_name, L"concrt") ||
        HasPrefix(module_name, L"vcomp") || HasPrefix(module_name, L"mfc") ||
        HasPrefix(module_name, L"mfcm");
}

[[nodiscard]] bool IsDynamicCrtImport(std::wstring_view module_name) noexcept {
    return IsCrtModule(module_name) || HasPrefix(module_name, L"api-ms-win-crt-");
}

#if defined(CABBIRD_ENABLE_ASAN)
[[nodiscard]] bool IsHostSanitizerRuntime(std::wstring_view module_name) noexcept {
    return module_name == L"clang_rt.asan_dynamic-x86_64.dll";
}
#endif

[[nodiscard]] std::filesystem::path SystemDirectory() {
    std::array<wchar_t, 32768> buffer{};
    const UINT length = GetSystemDirectoryW(
        buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

[[nodiscard]] bool IsSystemModule(
    const std::filesystem::path& system_directory,
    std::wstring_view module_name) {
    if (system_directory.empty()) return false;
    const std::filesystem::path candidate =
        system_directory / std::wstring(module_name);
    const DWORD attributes = GetFileAttributesW(candidate.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

[[nodiscard]] bool IsRegularFile(const std::filesystem::path& path) noexcept {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

[[nodiscard]] std::wstring NormalizedPathKey(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path normalized = std::filesystem::absolute(path, error);
    if (error) normalized = path.lexically_normal();
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(normalized, error);
    return Lowercase((error ? normalized.lexically_normal() : canonical).wstring());
}

[[nodiscard]] std::optional<std::wstring> NormalizeImportName(std::string_view value) {
    if (value.empty() || value.size() > MAX_PATH) return std::nullopt;
    for (const unsigned char character : value) {
        if (character < 0x21U || character > 0x7eU || character == '/' ||
            character == '\\' || character == ':') {
            return std::nullopt;
        }
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (length <= 0) return std::nullopt;
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), length) != length ||
        result == L"." || result == L"..") {
        return std::nullopt;
    }
    return Lowercase(std::move(result));
}

template <typename T>
[[nodiscard]] T* ImageRva(
    std::byte* base, std::size_t image_size, std::uint32_t rva,
    std::size_t count = 1) noexcept {
    if (base == nullptr || count > (std::numeric_limits<std::size_t>::max)() / sizeof(T)) {
        return nullptr;
    }
    const std::size_t bytes = count * sizeof(T);
    if (rva > image_size || bytes > image_size - rva) return nullptr;
    return reinterpret_cast<T*>(base + rva);
}

template <typename T>
[[nodiscard]] const T* ImageRva(
    const std::byte* base, std::size_t image_size, std::uint32_t rva,
    std::size_t count = 1) noexcept {
    return ImageRva<T>(const_cast<std::byte*>(base), image_size, rva, count);
}

[[nodiscard]] IMAGE_NT_HEADERS64* NtHeaders(
    std::byte* base, std::size_t size) noexcept {
    if (base == nullptr || size < sizeof(IMAGE_DOS_HEADER)) return nullptr;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<std::size_t>(dos->e_lfanew) > size - sizeof(IMAGE_NT_HEADERS64)) {
        return nullptr;
    }
    auto* headers = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    return headers->Signature == IMAGE_NT_SIGNATURE ? headers : nullptr;
}

[[nodiscard]] const IMAGE_NT_HEADERS64* NtHeaders(
    const std::byte* base, std::size_t size) noexcept {
    return NtHeaders(const_cast<std::byte*>(base), size);
}

[[nodiscard]] DWORD SectionProtection(DWORD characteristics) noexcept {
    const bool execute = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    const bool read = (characteristics & IMAGE_SCN_MEM_READ) != 0;
    const bool write = (characteristics & IMAGE_SCN_MEM_WRITE) != 0;
    if (execute) {
        if (write) return PAGE_EXECUTE_READWRITE;
        return read ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    }
    if (write) return PAGE_READWRITE;
    return read ? PAGE_READONLY : PAGE_NOACCESS;
}

[[nodiscard]] bool ReadFileBytes(
    const std::filesystem::path& path,
    std::vector<std::byte>& output,
    std::string& failure) {
    FileHandle file(CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        failure = "plugin image could not be opened (err=" +
            std::to_string(GetLastError()) + ")";
        return false;
    }
    LARGE_INTEGER file_size{};
    if (GetFileSizeEx(file.Get(), &file_size) == FALSE ||
        file_size.QuadPart < static_cast<LONGLONG>(sizeof(IMAGE_DOS_HEADER)) ||
        static_cast<std::uint64_t>(file_size.QuadPart) > kMaximumImageBytes) {
        failure = "plugin image has an invalid size";
        return false;
    }
    output.resize(static_cast<std::size_t>(file_size.QuadPart));
    std::size_t offset{};
    while (offset < output.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(
            output.size() - offset,
            static_cast<std::size_t>(64U * 1024U * 1024U)));
        DWORD bytes_read{};
        if (ReadFile(file.Get(), output.data() + offset, chunk, &bytes_read, nullptr) == FALSE ||
            bytes_read == 0) {
            failure = "plugin image read failed (err=" +
                std::to_string(GetLastError()) + ")";
            return false;
        }
        offset += bytes_read;
    }
    return true;
}

struct MappedNode final {
    std::byte* base{};
    std::size_t image_size{};
    PRUNTIME_FUNCTION exception_table{};
    DWORD exception_count{};
    bool unwind_registered{};
    bool exception_range_registered{};
    std::vector<PIMAGE_TLS_CALLBACK> tls_callbacks;
    std::size_t attached_tls_count{};
    DllMainFn entry_point{};
    bool attached_entry{};
    std::vector<HMODULE> system_dependencies;
    std::filesystem::path source_path;

    ~MappedNode() { Teardown(); }

    MappedNode() = default;
    MappedNode(const MappedNode&) = delete;
    MappedNode& operator=(const MappedNode&) = delete;

    void Teardown() noexcept {
        const auto base_addr = reinterpret_cast<std::uintptr_t>(base);
        if (attached_entry && entry_point != nullptr) {
            attached_entry = false;
            try {
                static_cast<void>(entry_point(
                    reinterpret_cast<HMODULE>(base_addr), DLL_PROCESS_DETACH, nullptr));
            } catch (...) {
            }
        }
        while (attached_tls_count > 0) {
            --attached_tls_count;
            const PIMAGE_TLS_CALLBACK callback = tls_callbacks[attached_tls_count];
            if (callback != nullptr) {
                try {
                    callback(reinterpret_cast<PVOID>(base_addr), DLL_PROCESS_DETACH, nullptr);
                } catch (...) {
                }
            }
        }
        if (unwind_registered && exception_table != nullptr) {
            unwind_registered = false;
            static_cast<void>(RtlDeleteFunctionTable(exception_table));
            exception_table = nullptr;
            exception_count = 0;
        }
        if (exception_range_registered) {
            exception_range_registered = false;
            UnregisterMappedImageRange(base_addr);
        }
        for (auto it = system_dependencies.rbegin(); it != system_dependencies.rend(); ++it) {
            if (*it != nullptr) static_cast<void>(FreeLibrary(*it));
        }
        system_dependencies.clear();
        if (base != nullptr) {
            static_cast<void>(VirtualFree(base, 0, MEM_RELEASE));
            base = nullptr;
            image_size = 0;
        }
    }
};

struct LoadGraph final {
    std::filesystem::path system_directory;
    std::unordered_map<std::wstring, std::shared_ptr<MappedNode>> mapped_by_path;
    std::unordered_set<std::wstring> in_progress_paths;
    // Initialization order: dependencies precede dependents, so reverse teardown
    // always detaches dependents before their private dependencies.
    std::vector<std::shared_ptr<MappedNode>> initialized_nodes;

    void TeardownAll() noexcept {
        for (auto it = initialized_nodes.rbegin(); it != initialized_nodes.rend(); ++it) {
            if (*it) (*it)->Teardown();
        }
        initialized_nodes.clear();
        mapped_by_path.clear();
    }
};

[[nodiscard]] bool RelocateNode(MappedNode& node, std::string& failure) {
    auto* headers = NtHeaders(node.base, node.image_size);
    if (headers == nullptr) {
        failure = "mapped plugin headers are invalid";
        return false;
    }
    const auto base_addr = reinterpret_cast<std::uintptr_t>(node.base);
    const std::int64_t delta = static_cast<std::int64_t>(
        base_addr - static_cast<std::uintptr_t>(headers->OptionalHeader.ImageBase));
    if (delta == 0) return true;

    const auto& directory =
        headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_BASE_RELOCATION)) {
        failure = "plugin image cannot be relocated";
        return false;
    }
    std::size_t consumed{};
    while (consumed < directory.Size) {
        auto* block = ImageRva<IMAGE_BASE_RELOCATION>(
            node.base, node.image_size,
            directory.VirtualAddress + static_cast<std::uint32_t>(consumed));
        if (block == nullptr || block->SizeOfBlock < sizeof(*block) ||
            block->SizeOfBlock > directory.Size - consumed) {
            failure = "plugin relocation directory is invalid";
            return false;
        }
        const std::size_t count =
            (block->SizeOfBlock - sizeof(*block)) / sizeof(WORD);
        auto* entries = reinterpret_cast<WORD*>(block + 1);
        for (std::size_t index = 0; index < count; ++index) {
            const WORD type = entries[index] >> 12U;
            const WORD offset = entries[index] & 0x0fffU;
            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
            if (type != IMAGE_REL_BASED_DIR64) {
                failure = "plugin image uses an unsupported relocation type";
                return false;
            }
            auto* target = ImageRva<std::uint64_t>(
                node.base, node.image_size, block->VirtualAddress + offset);
            if (target == nullptr) {
                failure = "plugin relocation target is outside the image";
                return false;
            }
            *target = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(*target) + delta);
        }
        consumed += block->SizeOfBlock;
    }
    headers->OptionalHeader.ImageBase = base_addr;
    return true;
}

[[nodiscard]] bool ProtectNode(MappedNode& node, std::string& failure) {
    auto* headers = NtHeaders(node.base, node.image_size);
    if (headers == nullptr) {
        failure = "mapped plugin headers are invalid";
        return false;
    }
    const DWORD size_of_headers = headers->OptionalHeader.SizeOfHeaders;
    const auto* first_section = IMAGE_FIRST_SECTION(headers);
    std::vector<IMAGE_SECTION_HEADER> sections(
        first_section, first_section + headers->FileHeader.NumberOfSections);

    DWORD previous{};
    if (VirtualProtect(node.base, node.image_size, PAGE_NOACCESS, &previous) == FALSE) {
        failure = "plugin image memory protection reset failed (err=" +
            std::to_string(GetLastError()) + ")";
        return false;
    }
    if (VirtualProtect(node.base, size_of_headers, PAGE_READONLY, &previous) == FALSE) {
        failure = "plugin header protection failed (err=" +
            std::to_string(GetLastError()) + ")";
        return false;
    }
    for (const auto& section : sections) {
        const SIZE_T size = (std::max)(
            static_cast<SIZE_T>(section.Misc.VirtualSize),
            static_cast<SIZE_T>(section.SizeOfRawData));
        if (size == 0) continue;
        if (VirtualProtect(
                node.base + section.VirtualAddress, size,
                SectionProtection(section.Characteristics), &previous) == FALSE) {
            failure = "plugin section protection failed (err=" +
                std::to_string(GetLastError()) + ")";
            return false;
        }
    }
    if (FlushInstructionCache(GetCurrentProcess(), node.base, node.image_size) == FALSE) {
        failure = "plugin instruction cache flush failed (err=" +
            std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

[[nodiscard]] void* FindDirectExportByName(
    const std::byte* base, std::size_t image_size, std::string_view requested,
    std::string* forwarded_out = nullptr) noexcept {
    const auto* headers = NtHeaders(base, image_size);
    if (headers == nullptr) return nullptr;
    const auto& data = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (data.VirtualAddress == 0 || data.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
        return nullptr;
    }
    const auto* exports = ImageRva<IMAGE_EXPORT_DIRECTORY>(
        base, image_size, data.VirtualAddress);
    if (exports == nullptr) return nullptr;
    const auto* names = ImageRva<DWORD>(
        base, image_size, exports->AddressOfNames, exports->NumberOfNames);
    const auto* ordinals = ImageRva<WORD>(
        base, image_size, exports->AddressOfNameOrdinals, exports->NumberOfNames);
    const auto* functions = ImageRva<DWORD>(
        base, image_size, exports->AddressOfFunctions, exports->NumberOfFunctions);
    if (names == nullptr || ordinals == nullptr || functions == nullptr) return nullptr;

    for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
        const char* name = ImageRva<char>(base, image_size, names[index]);
        if (name == nullptr ||
            std::memchr(name, '\0', image_size - names[index]) == nullptr) {
            return nullptr;
        }
        if (requested != name) continue;
        const WORD ordinal = ordinals[index];
        if (ordinal >= exports->NumberOfFunctions) return nullptr;
        const DWORD rva = functions[ordinal];
        if (rva == 0 || rva >= image_size) return nullptr;
        if (rva >= data.VirtualAddress && rva < data.VirtualAddress + data.Size) {
            if (forwarded_out != nullptr) {
                const char* forwarder = ImageRva<char>(base, image_size, rva);
                if (forwarder != nullptr &&
                    std::memchr(forwarder, '\0', image_size - rva) != nullptr) {
                    forwarded_out->assign(forwarder);
                }
            }
            return nullptr;
        }
        return const_cast<std::byte*>(base + rva);
    }
    return nullptr;
}

[[nodiscard]] void* FindDirectExportByOrdinal(
    const std::byte* base, std::size_t image_size, WORD ordinal,
    std::string* forwarded_out = nullptr) noexcept {
    const auto* headers = NtHeaders(base, image_size);
    if (headers == nullptr) return nullptr;
    const auto& data = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (data.VirtualAddress == 0 || data.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
        return nullptr;
    }
    const auto* exports = ImageRva<IMAGE_EXPORT_DIRECTORY>(
        base, image_size, data.VirtualAddress);
    if (exports == nullptr || ordinal < exports->Base) return nullptr;
    const DWORD index = static_cast<DWORD>(ordinal) - exports->Base;
    if (index >= exports->NumberOfFunctions) return nullptr;
    const auto* functions = ImageRva<DWORD>(
        base, image_size, exports->AddressOfFunctions, exports->NumberOfFunctions);
    if (functions == nullptr) return nullptr;
    const DWORD rva = functions[index];
    if (rva == 0 || rva >= image_size) return nullptr;
    if (rva >= data.VirtualAddress && rva < data.VirtualAddress + data.Size) {
        if (forwarded_out != nullptr) {
            const char* forwarder = ImageRva<char>(base, image_size, rva);
            if (forwarder != nullptr &&
                std::memchr(forwarder, '\0', image_size - rva) != nullptr) {
                forwarded_out->assign(forwarder);
            }
        }
        return nullptr;
    }
    return const_cast<std::byte*>(base + rva);
}

[[nodiscard]] std::shared_ptr<MappedNode> LoadNode(
    LoadGraph& graph, const std::filesystem::path& path, std::string& failure);

[[nodiscard]] HMODULE AcquireSystemModule(
    MappedNode& requester,
    std::unordered_map<std::wstring, HMODULE>& cache,
    const std::wstring& module_name,
    std::string& failure) {
    if (const auto found = cache.find(module_name); found != cache.end()) {
        return found->second;
    }
    HMODULE module{};
#if defined(CABBIRD_ENABLE_ASAN)
    if (IsHostSanitizerRuntime(module_name)) {
        if (GetModuleHandleExW(0, module_name.c_str(), &module) == FALSE ||
            module == nullptr) {
            failure = "host AddressSanitizer runtime is not loaded";
            return nullptr;
        }
    } else
#endif
    {
        module = LoadLibraryExW(
            module_name.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) {
            failure = "system dependency could not be loaded (err=" +
                std::to_string(GetLastError()) + ")";
            return nullptr;
        }
    }
    requester.system_dependencies.push_back(module);
    cache.emplace(module_name, module);
    return module;
}

[[nodiscard]] void* ResolveForwardedSymbol(
    LoadGraph& graph,
    MappedNode& requester,
    std::unordered_map<std::wstring, HMODULE>& system_cache,
    std::string_view forwarder,
    std::size_t depth,
    std::string& failure) {
    if (depth >= kMaximumForwardDepth) {
        failure = "forwarded export exceeded maximum indirection depth";
        return nullptr;
    }
    const std::size_t dot = forwarder.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= forwarder.size()) {
        failure = "forwarded export target is malformed";
        return nullptr;
    }
    std::string dll_name(forwarder.substr(0, dot));
    if (dll_name.find('.') == std::string::npos) dll_name += ".dll";
    const auto normalized = NormalizeImportName(dll_name);
    if (!normalized) {
        failure = "forwarded export module name is invalid";
        return nullptr;
    }
    const std::string_view symbol = forwarder.substr(dot + 1);
    const bool by_ordinal = !symbol.empty() && symbol.front() == '#';
    WORD ordinal{};
    if (by_ordinal) {
        std::uint32_t parsed{};
        if (symbol.size() == 1) {
            failure = "forwarded ordinal is empty";
            return nullptr;
        }
        for (std::size_t i = 1; i < symbol.size(); ++i) {
            if (symbol[i] < '0' || symbol[i] > '9') {
                failure = "forwarded ordinal is invalid";
                return nullptr;
            }
            parsed = parsed * 10U + static_cast<std::uint32_t>(symbol[i] - '0');
            if (parsed > 0xffffU) {
                failure = "forwarded ordinal overflows";
                return nullptr;
            }
        }
        ordinal = static_cast<WORD>(parsed);
    }

    if (!by_ordinal && symbol == "_CxxThrowException") {
        return reinterpret_cast<void*>(&CabbirdMappedPluginCxxThrow);
    }

    const std::filesystem::path private_candidate =
        requester.source_path.parent_path() / *normalized;
    const bool has_private = IsRegularFile(private_candidate) &&
        !IsSystemModule(graph.system_directory, *normalized) &&
        !IsCrtModule(*normalized) && !IsApiSetModule(*normalized);
    if (has_private) {
        const auto dep = LoadNode(graph, private_candidate, failure);
        if (!dep) return nullptr;
        std::string nested;
        void* resolved = by_ordinal
            ? FindDirectExportByOrdinal(dep->base, dep->image_size, ordinal, &nested)
            : FindDirectExportByName(dep->base, dep->image_size, symbol, &nested);
        if (resolved != nullptr) return resolved;
        if (!nested.empty()) {
            return ResolveForwardedSymbol(
                graph, *dep, system_cache, nested, depth + 1, failure);
        }
        failure = "forwarded export could not be resolved in private dependency";
        return nullptr;
    }

    HMODULE dep = AcquireSystemModule(requester, system_cache, *normalized, failure);
    if (dep == nullptr) return nullptr;
    FARPROC proc = by_ordinal
        ? GetProcAddress(dep, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(ordinal)))
        : GetProcAddress(dep, std::string(symbol).c_str());
    if (proc == nullptr) {
        failure = "forwarded system export could not be resolved";
        return nullptr;
    }
    return reinterpret_cast<void*>(proc);
}

[[nodiscard]] bool ResolveThunkTableForSystem(
    MappedNode& node,
    HMODULE dep_module,
    DWORD source_rva,
    DWORD destination_rva,
    std::string& failure) {
    const std::size_t maximum = node.image_size / sizeof(IMAGE_THUNK_DATA64);
    for (std::size_t index = 0; index < maximum; ++index) {
        const auto* source = ImageRva<IMAGE_THUNK_DATA64>(
            node.base, node.image_size,
            source_rva + static_cast<DWORD>(index * sizeof(IMAGE_THUNK_DATA64)));
        auto* destination = ImageRva<IMAGE_THUNK_DATA64>(
            node.base, node.image_size,
            destination_rva + static_cast<DWORD>(index * sizeof(IMAGE_THUNK_DATA64)));
        if (source == nullptr || destination == nullptr) {
            failure = "plugin import thunk table is outside the image";
            return false;
        }
        if (source->u1.AddressOfData == 0) return true;

        FARPROC proc{};
        if (IMAGE_SNAP_BY_ORDINAL64(source->u1.Ordinal)) {
            proc = GetProcAddress(
                dep_module,
                reinterpret_cast<LPCSTR>(IMAGE_ORDINAL64(source->u1.Ordinal)));
        } else {
            const auto* import = ImageRva<IMAGE_IMPORT_BY_NAME>(
                node.base, node.image_size,
                static_cast<std::uint32_t>(source->u1.AddressOfData));
            if (import == nullptr) {
                failure = "plugin import name is outside the image";
                return false;
            }
            const std::size_t name_offset = static_cast<std::size_t>(
                reinterpret_cast<const std::byte*>(import->Name) - node.base);
            if (name_offset >= node.image_size ||
                std::memchr(import->Name, '\0', node.image_size - name_offset) == nullptr) {
                failure = "plugin import name is not terminated";
                return false;
            }
            if (std::string_view(import->Name) == "_CxxThrowException") {
                destination->u1.Function = reinterpret_cast<std::uintptr_t>(
                    &CabbirdMappedPluginCxxThrow);
                continue;
            }
            proc = GetProcAddress(dep_module, reinterpret_cast<LPCSTR>(import->Name));
        }
        if (proc == nullptr) {
            failure = "imported system symbol could not be resolved";
            return false;
        }
        destination->u1.Function = reinterpret_cast<std::uintptr_t>(proc);
    }
    failure = "plugin import thunk table is not terminated";
    return false;
}

[[nodiscard]] bool ResolveThunkTableForPrivate(
    LoadGraph& graph,
    MappedNode& node,
    MappedNode& dependency,
    std::unordered_map<std::wstring, HMODULE>& system_cache,
    DWORD source_rva,
    DWORD destination_rva,
    std::string& failure) {
    const std::size_t maximum = node.image_size / sizeof(IMAGE_THUNK_DATA64);
    for (std::size_t index = 0; index < maximum; ++index) {
        const auto* source = ImageRva<IMAGE_THUNK_DATA64>(
            node.base, node.image_size,
            source_rva + static_cast<DWORD>(index * sizeof(IMAGE_THUNK_DATA64)));
        auto* destination = ImageRva<IMAGE_THUNK_DATA64>(
            node.base, node.image_size,
            destination_rva + static_cast<DWORD>(index * sizeof(IMAGE_THUNK_DATA64)));
        if (source == nullptr || destination == nullptr) {
            failure = "plugin private import thunk table is outside the image";
            return false;
        }
        if (source->u1.AddressOfData == 0) return true;

        void* resolved{};
        std::string forwarded;
        if (IMAGE_SNAP_BY_ORDINAL64(source->u1.Ordinal)) {
            resolved = FindDirectExportByOrdinal(
                dependency.base, dependency.image_size,
                static_cast<WORD>(IMAGE_ORDINAL64(source->u1.Ordinal)), &forwarded);
        } else {
            const auto* import = ImageRva<IMAGE_IMPORT_BY_NAME>(
                node.base, node.image_size,
                static_cast<std::uint32_t>(source->u1.AddressOfData));
            if (import == nullptr) {
                failure = "plugin private import name is outside the image";
                return false;
            }
            const std::size_t name_offset = static_cast<std::size_t>(
                reinterpret_cast<const std::byte*>(import->Name) - node.base);
            if (name_offset >= node.image_size ||
                std::memchr(import->Name, '\0', node.image_size - name_offset) == nullptr) {
                failure = "plugin private import name is not terminated";
                return false;
            }
            resolved = FindDirectExportByName(
                dependency.base, dependency.image_size, import->Name, &forwarded);
        }
        if (resolved == nullptr && !forwarded.empty()) {
            resolved = ResolveForwardedSymbol(
                graph, dependency, system_cache, forwarded, 0, failure);
            if (resolved == nullptr) return false;
        }
        if (resolved == nullptr) {
            failure = "imported private dependency symbol could not be resolved";
            return false;
        }
        destination->u1.Function = reinterpret_cast<std::uintptr_t>(resolved);
    }
    failure = "plugin private import thunk table is not terminated";
    return false;
}

[[nodiscard]] bool ResolveDependencyModule(
    LoadGraph& graph,
    MappedNode& node,
    std::unordered_map<std::wstring, HMODULE>& system_cache,
    std::string_view raw_name,
    DWORD source_rva,
    DWORD destination_rva,
    DWORD delay_module_handle_rva,
    bool& has_dynamic_crt_import,
    std::string& failure) {
    const auto module_name = NormalizeImportName(raw_name);
    if (!module_name) {
        failure = "plugin import module name is invalid";
        return false;
    }

    const std::filesystem::path private_candidate =
        node.source_path.parent_path() / *module_name;
    const bool has_private = IsRegularFile(private_candidate);
    const bool is_system = IsSystemModule(graph.system_directory, *module_name);
    const bool is_crt = IsCrtModule(*module_name);
    const bool is_api_set = IsApiSetModule(*module_name);

    if (has_private && is_system) {
        failure = "plugin packages must not override a Windows system DLL";
        return false;
    }
    if (is_crt && has_private) {
        failure = "plugin packages must not provide a private CRT DLL";
        return false;
    }
#if defined(CABBIRD_ENABLE_ASAN)
    if (IsHostSanitizerRuntime(*module_name) && has_private) {
        failure = "plugin packages must not provide a private AddressSanitizer runtime";
        return false;
    }
#endif
    if (IsDynamicCrtImport(*module_name)) {
        has_dynamic_crt_import = true;
    }

    if (has_private && !is_api_set) {
        const auto dep = LoadNode(graph, private_candidate, failure);
        if (!dep) return false;
        return ResolveThunkTableForPrivate(
            graph, node, *dep, system_cache, source_rva, destination_rva, failure);
    }

    if (!is_system && !is_api_set
#if defined(CABBIRD_ENABLE_ASAN)
        && !IsHostSanitizerRuntime(*module_name)
#endif
    ) {
        failure = "plugin import is neither a Windows system module nor a sibling private DLL: " +
            std::string(raw_name);
        return false;
    }

    HMODULE dep = AcquireSystemModule(node, system_cache, *module_name, failure);
    if (dep == nullptr) return false;
    if (delay_module_handle_rva != 0) {
        auto* slot = ImageRva<HMODULE>(node.base, node.image_size, delay_module_handle_rva);
        if (slot == nullptr) {
            failure = "plugin delay-import module handle slot is outside the image";
            return false;
        }
        *slot = dep;
    }
    return ResolveThunkTableForSystem(node, dep, source_rva, destination_rva, failure);
}

[[nodiscard]] bool ResolveNodeImports(
    LoadGraph& graph, MappedNode& node, std::string& failure) {
    auto* headers = NtHeaders(node.base, node.image_size);
    if (headers == nullptr) {
        failure = "mapped plugin headers are invalid";
        return false;
    }
    std::unordered_map<std::wstring, HMODULE> system_cache;
    bool has_dynamic_crt_import{};

    const auto& imports =
        headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress != 0) {
        const std::size_t maximum = imports.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
        bool terminated{};
        for (std::size_t index = 0; index < maximum; ++index) {
            const auto* descriptor = ImageRva<IMAGE_IMPORT_DESCRIPTOR>(
                node.base, node.image_size,
                imports.VirtualAddress + static_cast<DWORD>(
                    index * sizeof(IMAGE_IMPORT_DESCRIPTOR)));
            if (descriptor == nullptr) {
                failure = "plugin import directory is outside the image";
                return false;
            }
            if (descriptor->Name == 0) {
                terminated = true;
                break;
            }
            const char* name = ImageRva<char>(node.base, node.image_size, descriptor->Name);
            if (name == nullptr ||
                std::memchr(name, '\0', node.image_size - descriptor->Name) == nullptr) {
                failure = "plugin import module name is invalid";
                return false;
            }
            const DWORD source = descriptor->OriginalFirstThunk != 0
                ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
            if (!ResolveDependencyModule(
                    graph, node, system_cache, name, source, descriptor->FirstThunk, 0,
                    has_dynamic_crt_import, failure)) {
                return false;
            }
        }
        if (!terminated) {
            failure = "plugin import directory is not terminated";
            return false;
        }
    }

    const auto& delay =
        headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delay.VirtualAddress != 0) {
        const std::size_t maximum = delay.Size / sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
        bool terminated{};
        for (std::size_t index = 0; index < maximum; ++index) {
            const auto* descriptor = ImageRva<IMAGE_DELAYLOAD_DESCRIPTOR>(
                node.base, node.image_size,
                delay.VirtualAddress + static_cast<DWORD>(
                    index * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)));
            if (descriptor == nullptr) {
                failure = "plugin delay-import directory is outside the image";
                return false;
            }
            if (descriptor->DllNameRVA == 0) {
                terminated = true;
                break;
            }
            if ((descriptor->Attributes.AllAttributes & 1U) == 0U) {
                failure = "VA-based delay imports are not supported for x64 plugin images";
                return false;
            }
            const char* name = ImageRva<char>(
                node.base, node.image_size, descriptor->DllNameRVA);
            if (name == nullptr ||
                std::memchr(name, '\0', node.image_size - descriptor->DllNameRVA) == nullptr) {
                failure = "plugin delay-import module name is invalid";
                return false;
            }
            if (!ResolveDependencyModule(
                    graph, node, system_cache, name,
                    descriptor->ImportNameTableRVA,
                    descriptor->ImportAddressTableRVA,
                    descriptor->ModuleHandleRVA,
                    has_dynamic_crt_import, failure)) {
                return false;
            }
        }
        if (!terminated) {
            failure = "plugin delay-import directory is not terminated";
            return false;
        }
    }

    if (headers->OptionalHeader.AddressOfEntryPoint != 0 && !has_dynamic_crt_import) {
        failure = "plugin image requires dynamic CRT (/MD); static CRT (/MT) is unsupported";
        return false;
    }
    return true;
}

[[nodiscard]] bool CollectTlsCallbacks(
    MappedNode& node, std::string& failure) {
    const auto* headers = NtHeaders(node.base, node.image_size);
    if (headers == nullptr) return false;
    const auto& tls = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tls.VirtualAddress == 0) return true;
    const auto* directory = ImageRva<IMAGE_TLS_DIRECTORY64>(
        node.base, node.image_size, tls.VirtualAddress);
    if (tls.Size < sizeof(IMAGE_TLS_DIRECTORY64) || directory == nullptr) {
        failure = "plugin TLS directory is outside the image";
        return false;
    }
    if (directory->AddressOfCallBacks == 0) return true;

    const auto base_addr = reinterpret_cast<std::uintptr_t>(node.base);
    const std::uintptr_t table_addr = directory->AddressOfCallBacks;
    if (table_addr < base_addr || table_addr >= base_addr + node.image_size) {
        failure = "plugin TLS callback table is outside the image";
        return false;
    }
    const auto callback_rva = static_cast<DWORD>(table_addr - base_addr);
    const std::size_t max_entries = (std::min)(
        (node.image_size - callback_rva) / sizeof(std::uint64_t),
        kMaximumTlsCallbacks);
    bool terminated{};
    for (std::size_t index = 0; index < max_entries; ++index) {
        const auto* entry = ImageRva<std::uint64_t>(
            node.base, node.image_size,
            callback_rva + static_cast<DWORD>(index * sizeof(std::uint64_t)));
        if (entry == nullptr) break;
        if (*entry == 0) {
            terminated = true;
            break;
        }
        if (*entry < base_addr || *entry >= base_addr + node.image_size) {
            failure = "plugin TLS callback target is outside the image";
            return false;
        }
        node.tls_callbacks.push_back(reinterpret_cast<PIMAGE_TLS_CALLBACK>(*entry));
    }
    if (!terminated) {
        failure = "plugin TLS callback table is not terminated";
        return false;
    }
    return true;
}

[[nodiscard]] std::shared_ptr<MappedNode> LoadNode(
    LoadGraph& graph, const std::filesystem::path& path, std::string& failure) {
    const std::wstring key = NormalizedPathKey(path);
    if (const auto existing = graph.mapped_by_path.find(key);
        existing != graph.mapped_by_path.end()) {
        return existing->second;
    }
    if (!graph.in_progress_paths.insert(key).second) {
        failure = "cyclic package-private native DLL dependency detected";
        return nullptr;
    }
    struct InProgressGuard {
        std::unordered_set<std::wstring>& set;
        const std::wstring& key;
        ~InProgressGuard() { set.erase(key); }
    } guard{graph.in_progress_paths, key};

    std::vector<std::byte> file;
    if (!ReadFileBytes(path, file, failure)) return nullptr;

    const auto* source_headers = NtHeaders(file.data(), file.size());
    if (source_headers == nullptr ||
        source_headers->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        (source_headers->FileHeader.Characteristics & IMAGE_FILE_DLL) == 0 ||
        source_headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        source_headers->FileHeader.NumberOfSections == 0 ||
        source_headers->FileHeader.NumberOfSections > kMaximumSections ||
        source_headers->OptionalHeader.SizeOfImage < source_headers->OptionalHeader.SizeOfHeaders ||
        source_headers->OptionalHeader.SizeOfImage > kMaximumImageBytes ||
        source_headers->OptionalHeader.SizeOfHeaders > file.size()) {
        failure = "plugin image is not a supported x64 DLL";
        return nullptr;
    }
    const auto* sections = IMAGE_FIRST_SECTION(source_headers);
    const auto* section_table_end = reinterpret_cast<const std::byte*>(
        sections + source_headers->FileHeader.NumberOfSections);
    if (section_table_end > file.data() + file.size()) {
        failure = "plugin section table is truncated";
        return nullptr;
    }

    const std::size_t image_size = source_headers->OptionalHeader.SizeOfImage;
    auto node = std::make_shared<MappedNode>();
    node->source_path = std::filesystem::absolute(path);

    void* allocated = VirtualAlloc(
        reinterpret_cast<void*>(source_headers->OptionalHeader.ImageBase),
        image_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (allocated == nullptr) {
        allocated = VirtualAlloc(
            nullptr, image_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    if (allocated == nullptr) {
        failure = "plugin virtual image allocation failed (err=" +
            std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    node->base = static_cast<std::byte*>(allocated);
    node->image_size = image_size;

    std::memcpy(node->base, file.data(), source_headers->OptionalHeader.SizeOfHeaders);
    for (WORD index = 0; index < source_headers->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        const std::size_t virtual_size = (std::max)(
            static_cast<std::size_t>(section.Misc.VirtualSize),
            static_cast<std::size_t>(section.SizeOfRawData));
        if (section.VirtualAddress > image_size ||
            virtual_size > image_size - section.VirtualAddress ||
            section.PointerToRawData > file.size() ||
            section.SizeOfRawData > file.size() - section.PointerToRawData) {
            failure = "plugin section is outside the file or virtual image";
            return nullptr;
        }
        if (section.SizeOfRawData != 0) {
            std::memcpy(
                node->base + section.VirtualAddress,
                file.data() + section.PointerToRawData,
                section.SizeOfRawData);
        }
    }

    auto* headers = NtHeaders(node->base, node->image_size);
    if (headers == nullptr) {
        failure = "mapped plugin headers are invalid";
        return nullptr;
    }

    // Validate static TLS before relocation so Start/End addresses are compared verbatim.
    const auto& source_tls =
        headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (source_tls.VirtualAddress != 0) {
        const auto* tls_directory = ImageRva<IMAGE_TLS_DIRECTORY64>(
            node->base, node->image_size, source_tls.VirtualAddress);
        if (source_tls.Size < sizeof(IMAGE_TLS_DIRECTORY64) || tls_directory == nullptr) {
            failure = "plugin TLS directory is outside the image";
            return nullptr;
        }
        if (tls_directory->StartAddressOfRawData != tls_directory->EndAddressOfRawData ||
            tls_directory->SizeOfZeroFill != 0) {
            failure = "plugin image requires loader-managed static TLS";
            return nullptr;
        }
    }

    const auto& exception =
        headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (headers->OptionalHeader.AddressOfEntryPoint >= node->image_size ||
        (exception.VirtualAddress != 0 &&
         (exception.Size == 0 ||
          exception.Size % sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY) != 0 ||
          ImageRva<std::byte>(
              node->base, node->image_size, exception.VirtualAddress, exception.Size) == nullptr))) {
        failure = "plugin entry point or exception directory is invalid";
        return nullptr;
    }

    if (!RelocateNode(*node, failure)) return nullptr;
    if (!ResolveNodeImports(graph, *node, failure)) return nullptr;
    if (!CollectTlsCallbacks(*node, failure)) return nullptr;
    if (!ProtectNode(*node, failure)) return nullptr;

    const auto base_addr = reinterpret_cast<std::uintptr_t>(node->base);
    if (exception.VirtualAddress != 0) {
        node->exception_table = reinterpret_cast<PRUNTIME_FUNCTION>(
            node->base + exception.VirtualAddress);
        node->exception_count = exception.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
        if (RtlAddFunctionTable(
                node->exception_table, node->exception_count, base_addr) == FALSE) {
            failure = "plugin unwind function table registration failed";
            return nullptr;
        }
        node->unwind_registered = true;
    }

    if (!RegisterMappedImageRange(base_addr, node->image_size)) {
        failure = "plugin exception range registration failed";
        return nullptr;
    }
    node->exception_range_registered = true;

    for (const PIMAGE_TLS_CALLBACK callback : node->tls_callbacks) {
        try {
            callback(reinterpret_cast<PVOID>(base_addr), DLL_PROCESS_ATTACH, nullptr);
            ++node->attached_tls_count;
        } catch (...) {
            failure = "plugin TLS callback raised an exception during attach";
            return nullptr;
        }
    }

    if (headers->OptionalHeader.AddressOfEntryPoint != 0) {
        node->entry_point = reinterpret_cast<DllMainFn>(
            node->base + headers->OptionalHeader.AddressOfEntryPoint);
        BOOL attached = FALSE;
        try {
            attached = node->entry_point(
                reinterpret_cast<HMODULE>(base_addr), DLL_PROCESS_ATTACH, nullptr);
        } catch (...) {
            failure = "plugin DllMain raised an exception during attach";
            return nullptr;
        }
        if (attached == FALSE) {
            failure = "plugin DllMain returned FALSE during attach";
            return nullptr;
        }
        node->attached_entry = true;
    }

    graph.mapped_by_path.emplace(key, node);
    graph.initialized_nodes.push_back(node);
    return node;
}

}  // namespace

struct MappedPluginImage::Impl final {
    std::shared_ptr<MappedNode> root;
    std::vector<std::shared_ptr<MappedNode>> initialized_nodes;

    void Unload() noexcept {
        for (auto it = initialized_nodes.rbegin(); it != initialized_nodes.rend(); ++it) {
            if (*it) (*it)->Teardown();
        }
        initialized_nodes.clear();
        root.reset();
    }

    ~Impl() { Unload(); }
};

MappedPluginImage::MappedPluginImage(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

MappedPluginImage::~MappedPluginImage() { Unload(); }

MappedPluginImage::LoadResult MappedPluginImage::Load(
    const std::filesystem::path& entry_path) noexcept {
    LoadResult result;
    try {
        if (entry_path.empty()) {
            result.error = "plugin image path is empty";
            return result;
        }
        LoadGraph graph;
        graph.system_directory = SystemDirectory();
        if (graph.system_directory.empty()) {
            result.error = "Windows system directory is unavailable";
            return result;
        }
        std::string failure;
        const auto root = LoadNode(graph, entry_path, failure);
        if (!root) {
            graph.TeardownAll();
            result.error = failure.empty() ? "plugin manual mapping failed" : std::move(failure);
            return result;
        }
        auto impl = std::make_unique<Impl>();
        impl->root = root;
        impl->initialized_nodes = std::move(graph.initialized_nodes);
        result.image = std::unique_ptr<MappedPluginImage>(
            new MappedPluginImage(std::move(impl)));
        return result;
    } catch (const std::exception& error) {
        result.error = error.what();
        return result;
    } catch (...) {
        result.error = "plugin manual mapping raised an unexpected exception";
        return result;
    }
}

void* MappedPluginImage::ExportAddress(std::string_view name) const noexcept {
    if (!impl_ || !impl_->root || impl_->root->base == nullptr || name.empty()) {
        return nullptr;
    }
    return FindDirectExportByName(impl_->root->base, impl_->root->image_size, name);
}

std::uintptr_t MappedPluginImage::BaseAddress() const noexcept {
    if (!impl_ || !impl_->root) return 0;
    return reinterpret_cast<std::uintptr_t>(impl_->root->base);
}

std::size_t MappedPluginImage::ImageSize() const noexcept {
    if (!impl_ || !impl_->root) return 0;
    return impl_->root->image_size;
}

void MappedPluginImage::Unload() noexcept {
    if (impl_) {
        impl_->Unload();
        impl_.reset();
    }
}

}  // namespace cabbird

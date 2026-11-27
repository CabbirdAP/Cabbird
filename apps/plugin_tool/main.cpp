#include "cabbird/plugin_manifest.hpp"
#include "cabbird/plugin_capability_policy.hpp"
#include "cabbird/plugin_package.hpp"
#include "cabbird/artifact_bundle.hpp"
#include "cabbird/artifact_crypto.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
std::string Utf8(const fs::path& path) {
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}
[[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
bool SameName(const fs::path& value, const wchar_t* expected) {
    return CompareStringOrdinal(value.c_str(), -1, expected, -1, TRUE) == CSTR_EQUAL;
}

// Reject reparse components, then resolve the existing prefix by handle so DOS,
// extended-length and short-name aliases compare as the same physical directory.
fs::path CheckedPath(const fs::path& input) {
    if (input.empty()) Fail("path is empty");
    const auto absolute = fs::absolute(input).lexically_normal();
    auto current = absolute.root_path();
    auto existing = current;
    fs::path tail;
    bool missing{};
    for (const auto& part : absolute.relative_path()) {
        if (part.empty()) continue;
        if (missing) { tail /= part; continue; }
        current /= part;
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                missing = true;
                tail /= part;
                continue;
            }
            Fail("path attributes unavailable: " + Utf8(current));
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            Fail("reparse points are not package paths: " + Utf8(current));
        existing = current;
    }
    Handle handle;
    handle.value = CreateFileW(existing.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle.value == INVALID_HANDLE_VALUE) Fail("cannot open path identity: " + Utf8(existing));
    const DWORD required = GetFinalPathNameByHandleW(handle.value, nullptr, 0, FILE_NAME_NORMALIZED);
    if (required == 0) Fail("path identity is unavailable: " + Utf8(existing));
    std::wstring name(required, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(handle.value, name.data(), required, FILE_NAME_NORMALIZED);
    if (written == 0 || written >= required) Fail("path identity changed while resolving");
    name.resize(written);
    return tail.empty() ? fs::path(name) : fs::path(name) / tail;
}
bool Within(const fs::path& root, const fs::path& candidate) {
    auto prefix = root.native();
    const auto value = candidate.native();
    while (prefix.size() > root.root_path().native().size() &&
           (prefix.back() == L'\\' || prefix.back() == L'/')) prefix.pop_back();
    if (value.size() < prefix.size() || CompareStringOrdinal(prefix.c_str(),
            static_cast<int>(prefix.size()), value.c_str(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL)
        return false;
    return value.size() == prefix.size() || prefix.back() == L'\\' || prefix.back() == L'/' ||
        value[prefix.size()] == L'\\' || value[prefix.size()] == L'/';
}

std::vector<fs::path> PackageFiles(const fs::path& root) {
    std::vector<fs::path> files;
    std::uint64_t total{};
    for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
        const auto relative = it->path().lexically_relative(root);
        CheckedPath(it->path());
        if (!relative.empty() && SameName(*relative.begin(), L".cache")) {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory()) continue;
        if (!it->is_regular_file()) Fail("package contains a non-file entry");
        const auto confined = cabbird::OpenConfinedPluginPackageFile(root, Utf8(relative));
        if (!confined.Ok()) Fail("package entry: " + confined.message);
        const auto size = it->file_size();
        if (size > cabbird::kMaximumArtifactBundleBytes || total > cabbird::kMaximumArtifactBundleBytes - size ||
            files.size() >= cabbird::kMaximumArtifactBundleEntries)
            Fail("package exceeds artifact size or entry limits");
        total += size;
        files.push_back(relative);
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string ReadManifest(const fs::path& root) {
    Handle handle;
    const auto opened = cabbird::OpenConfinedPluginPackageFile(root, "manifest.json", false, &handle.value);
    if (!opened.Ok()) Fail("manifest.json: " + opened.message);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.value, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > cabbird::kMaximumPluginManifestBytes)
        Fail("manifest.json is unreadable or too large");
    std::string document(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read{};
    if (!ReadFile(handle.value, document.data(), static_cast<DWORD>(document.size()), &read, nullptr) ||
        read != document.size()) Fail("manifest.json read failed");
    return document;
}
struct Package {
    cabbird::PluginManifest manifest;
    fs::path root;
    std::vector<fs::path> files;
};
Package Validate(const fs::path& input, bool print_success) {
    const auto root = CheckedPath(input);
    if (!fs::is_directory(root)) Fail("package directory is unavailable: " + Utf8(root));
    auto parsed = cabbird::ParsePluginManifest(ReadManifest(root));
    for (const auto& diagnostic : parsed.diagnostics)
        std::cerr << "manifest[" << static_cast<unsigned>(diagnostic.code) << "] "
                  << diagnostic.path << ": " << diagnostic.message << '\n';
    if (!parsed.Ok()) Fail("manifest validation failed");
    const auto grant = cabbird::ResolvePluginCapabilityGrant(&*parsed.manifest);
    if (!grant.IsEnforceable()) {
        for (const auto& audit : grant.Audits()) {
            std::cerr << "manifest capability[" << static_cast<unsigned>(audit.code)
                      << "] service=" << audit.service
                      << " capability=" << audit.capability << '\n';
        }
        Fail("manifest service capability validation failed");
    }
    const auto entry = cabbird::OpenConfinedPluginPackageFile(root, parsed.manifest->entry, true);
    if (!entry.Ok()) Fail("entry: " + entry.message);
    auto files = PackageFiles(root);
    if (print_success)
        std::cout << "valid id=" << parsed.manifest->id << " version="
                  << parsed.manifest->version.ToString() << " entry=" << Utf8(entry.path.filename()) << '\n';
    return {std::move(*parsed.manifest), root, std::move(files)};
}

// Only this process's newly created staging tree is cleaned; published packages
// are never recursively deleted or replaced by the tool.
class StagingDirectory {
public:
    explicit StagingDirectory(fs::path path) : path_(CheckedPath(path)) {
        if (!fs::create_directory(path_)) Fail("staging directory already exists");
    }
    ~StagingDirectory() {
        if (!active_) return;
        try {
            CheckedPath(path_);
            std::vector<fs::path> entries;
            for (fs::recursive_directory_iterator it(path_), end; it != end; ++it) {
                const auto entry = CheckedPath(it->path());
                if (!Within(path_, entry) || entry == path_) return;
                entries.push_back(entry);
            }
            std::sort(entries.begin(), entries.end(), std::greater<fs::path>());
            for (const auto& entry : entries) {
                CheckedPath(entry);
                fs::remove(entry);
            }
            fs::remove(path_);
        } catch (...) {}
    }
    const fs::path& Path() const { return path_; }
    void Published() { active_ = false; }
private:
    fs::path path_;
    bool active_{true};
};

void CopyEntry(const Package& package, const fs::path& relative, const fs::path& destination) {
    Handle source;
    const auto opened = cabbird::OpenConfinedPluginPackageFile(package.root, Utf8(relative), false, &source.value);
    if (!opened.Ok()) Fail("source entry: " + opened.message);
    const auto target = CheckedPath(destination / relative);
    if (!Within(destination, target)) Fail("copy target escaped staging");
    fs::create_directories(target.parent_path());
    CheckedPath(target.parent_path());
    Handle output;
    output.value = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (output.value == INVALID_HANDLE_VALUE) Fail("cannot create package entry: " + Utf8(relative));
    std::array<std::byte, 64 * 1024> buffer{};
    for (;;) {
        DWORD read{};
        if (!ReadFile(source.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            Fail("package entry read failed");
        if (read == 0) break;
        DWORD written{};
        if (!WriteFile(output.value, buffer.data(), read, &written, nullptr) || written != read)
            Fail("package entry write failed");
    }
}

void Pack(const fs::path& input, const fs::path& output_root) {
    const auto package = Validate(input, false);
    const auto output = CheckedPath(output_root);
    if (Within(package.root, output) || Within(output, package.root))
        Fail("input and output directory trees must not overlap");
    const auto destination = CheckedPath(output / package.manifest.id);
    if (fs::exists(destination)) Fail("output package already exists; choose a fresh output directory");
    fs::create_directories(output);
    CheckedPath(output);
    StagingDirectory staging(output / (package.manifest.id + ".staging-" +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    std::vector<fs::path> copied;
    for (const auto& relative : package.files) {
        if (SameName(relative, L"package.sha256")) continue;
        CopyEntry(package, relative, staging.Path());
        copied.push_back(relative);
    }
    const auto staged = Validate(staging.Path(), false);
    if (staged.manifest.id != package.manifest.id) Fail("manifest changed while packing");
    std::ofstream hashes(staging.Path() / L"package.sha256", std::ios::binary | std::ios::trunc);
    for (const auto& relative : copied) {
        const auto digest = cabbird::Sha256FileHex(staging.Path() / relative, cabbird::kMaximumArtifactBundleBytes);
        if (digest.empty()) Fail("package hash failed: " + Utf8(relative));
        hashes << digest << "  " << Utf8(relative) << '\n';
    }
    hashes.close();
    if (!hashes) Fail("package hash manifest write failed");
    CheckedPath(destination);
    if (fs::exists(destination)) Fail("output package appeared before publication");
    fs::rename(staging.Path(), destination);
    staging.Published();
    std::cout << "packed id=" << package.manifest.id << " output=" << Utf8(destination)
              << " files=" << copied.size() + 1 << '\n';
}

void Bundle(const fs::path& input, const fs::path& output_file) {
    const auto package = Validate(input, false);
    const auto output = CheckedPath(output_file);
    if (Within(package.root, output)) Fail("bundle output must be outside the input package");
    if (fs::exists(output) && !fs::is_regular_file(output)) Fail("bundle output is not a regular file");
    const auto leaf = cabbird::ValidatePluginPackageRelativePath(Utf8(output.filename()));
    if (!leaf.Ok()) Fail("bundle output filename: " + leaf.message);
    const auto temporary = CheckedPath(output.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()));
    if (fs::exists(temporary)) Fail("bundle temporary path already exists");
    const auto result = cabbird::CreateArtifactBundle(package.root, output);
    if (!result.Ok()) Fail("bundle failed: " + result.message);
    std::cout << "bundled output=" << Utf8(output) << " files=" << result.entries
              << " bytes=" << result.bytes << '\n';
}
void Usage() {
    std::cerr << "usage:\n  cabbird-plugin validate <package-dir>\n"
                 "  cabbird-plugin pack <package-dir> --output <directory>\n"
                 "  cabbird-plugin bundle <packed-package-dir> --output <file.cabbird-package>\n";
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"validate") {
            static_cast<void>(Validate(argv[2], true));
            return 0;
        }
        if (argc == 5 && std::wstring_view(argv[3]) == L"--output") {
            const std::wstring_view command(argv[1]);
            if (command == L"pack") { Pack(argv[2], argv[4]); return 0; }
            if (command == L"bundle") { Bundle(argv[2], argv[4]); return 0; }
        }
        Usage();
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "cabbird-plugin: " << error.what() << '\n';
        return 2;
    }
}

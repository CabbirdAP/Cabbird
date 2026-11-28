#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace cabbird {

// Manually maps an x64 plugin DLL (and any sibling package-private DLLs) into
// the current process without registering them in the Windows PEB loader list.
//
// Supported images must match the Core manual-map invariants:
//   - x64 PE32+ DLL with dynamic CRT (/MD or /MDd);
//   - no loader-managed static TLS (.tls raw/zero-fill data must be empty);
//   - standard DIR64 relocations and RVA-based delay imports (if any).
//
// C++ exceptions inside mapped plugins work via RtlAddFunctionTable plus an
// IAT bridge on _CxxThrowException backed by a process-wide mapped-range table.
class MappedPluginImage final {
public:
    struct LoadResult {
        std::unique_ptr<MappedPluginImage> image;
        std::string error;

        [[nodiscard]] bool Ok() const noexcept { return image != nullptr; }
    };

    ~MappedPluginImage();

    MappedPluginImage(const MappedPluginImage&) = delete;
    MappedPluginImage& operator=(const MappedPluginImage&) = delete;
    MappedPluginImage(MappedPluginImage&&) = delete;
    MappedPluginImage& operator=(MappedPluginImage&&) = delete;

    [[nodiscard]] static LoadResult Load(const std::filesystem::path& entry_path) noexcept;

    [[nodiscard]] void* ExportAddress(std::string_view name) const noexcept;
    [[nodiscard]] std::uintptr_t BaseAddress() const noexcept;
    [[nodiscard]] std::size_t ImageSize() const noexcept;

    // Tears down the mapped image and any mapped package-private dependencies
    // in reverse initialization order. Safe to call repeatedly.
    void Unload() noexcept;

private:
    struct Impl;
    explicit MappedPluginImage(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace cabbird

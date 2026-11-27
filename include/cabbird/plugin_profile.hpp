#pragma once

#include "cabbird/sdk/base.h"
#include <nlohmann/json_fwd.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace cabbird {

inline constexpr std::size_t kMaximumPluginProfileBytes = 1024U * 1024U;

struct PluginProfileResult final {
    std::uint32_t code{CABBIRD_STATUS_V1_INVALID_ARGUMENT};
    std::string error;
    std::shared_ptr<nlohmann::json> document;
    [[nodiscard]] explicit operator bool() const noexcept { return document != nullptr; }
};

[[nodiscard]] PluginProfileResult ParsePluginProfile(std::string_view text);
[[nodiscard]] PluginProfileResult LoadPluginProfile(
    const std::filesystem::path& package_directory, std::string_view relative_path);

}  // namespace cabbird

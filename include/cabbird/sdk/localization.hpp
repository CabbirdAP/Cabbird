#pragma once

#include "cabbird/sdk/cpp.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace cabbird::sdk {

// Plugin-side convenience wrapper. The host owns the table and loads the
// plugin's locales/<locale>.json; no catalog or host implementation is linked.
class Localizer final {
public:
    Localizer() noexcept = default;

    explicit Localizer(const CabbirdHostApiV1* host) noexcept {
        const auto queried = Host(host).Query<CabbirdLocalizationServiceV1>(
            CABBIRD_LOCALIZATION_SERVICE_V1_ID, CABBIRD_LOCALIZATION_SERVICE_V1_VERSION);
        const auto* service = queried.get();
        constexpr std::size_t required_size = offsetof(CabbirdLocalizationServiceV1, translate) +
            sizeof(decltype(CabbirdLocalizationServiceV1::translate));
        if (service != nullptr && service->struct_size >= required_size &&
            service->translate != nullptr) service_ = service;
    }

    [[nodiscard]] std::string Text(
        std::string_view key, std::string_view english_fallback) const {
        return Format(key, english_fallback, {});
    }

    [[nodiscard]] std::string Locale() const {
        std::array<char, kInlineBytes> buffer{};
        std::size_t size = buffer.size();
        if (service_ != nullptr && service_->locale != nullptr &&
            Succeeded(service_->locale(service_->user, buffer.data(), &size)) &&
            size > 1U && size <= buffer.size() && buffer[size - 1U] == '\0') {
            return {buffer.data(), size - 1U};
        }
        return "en-US";
    }

    [[nodiscard]] std::string Format(
        std::string_view key, std::string_view english_fallback,
        std::span<const std::string_view> arguments) const {
        if (service_ == nullptr || arguments.size() > kMaximumArguments) {
            return FormatFallback(english_fallback, arguments);
        }
        std::array<CabbirdStringViewV1, kMaximumArguments> views{};
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            views[index] = StringView(arguments[index]);
        }
        const auto translate = [&](char* destination, std::size_t* size) {
            return service_->translate(service_->user, StringView(key),
                StringView(english_fallback), views.data(), arguments.size(), destination, size);
        };
        std::array<char, kInlineBytes> buffer{};
        std::size_t size = buffer.size();
        const auto status = translate(buffer.data(), &size);
        if (Succeeded(status) && size >= 1U && size <= buffer.size() &&
            buffer[size - 1U] == '\0') return {buffer.data(), size - 1U};
        if (status.code == CABBIRD_STATUS_V1_BUFFER_TOO_SMALL &&
            size > buffer.size() && size <= kMaximumBytes) {
            std::string sized(size, '\0');
            std::size_t required = sized.size();
            if (Succeeded(translate(sized.data(), &required)) && required >= 1U &&
                required <= sized.size() && sized[required - 1U] == '\0') {
                sized.resize(required - 1U);
                return sized;
            }
        }
        return FormatFallback(english_fallback, arguments);
    }

    // A translated widget title keeps its identity across text changes.
    [[nodiscard]] std::string Label(std::string_view key, std::string_view english_fallback,
                                    std::string_view stable_id) const {
        std::string result = Text(key, english_fallback);
        result.append("###").append(stable_id);
        return result;
    }

private:
    static constexpr std::size_t kMaximumArguments = 8;
    static constexpr std::size_t kInlineBytes = 512;
    static constexpr std::size_t kMaximumBytes = 64U * 1024U;

    [[nodiscard]] static std::string FormatFallback(
        std::string_view pattern, std::span<const std::string_view> arguments) {
        std::string result;
        result.reserve(pattern.size());
        for (std::size_t index = 0; index < pattern.size();) {
            if (index + 1U < pattern.size() &&
                ((pattern[index] == '{' && pattern[index + 1U] == '{') ||
                 (pattern[index] == '}' && pattern[index + 1U] == '}'))) {
                result.push_back(pattern[index]);
                index += 2U;
            } else if (index + 2U < pattern.size() && pattern[index] == '{' &&
                       pattern[index + 2U] == '}' && pattern[index + 1U] >= '0' &&
                       pattern[index + 1U] <= '7') {
                const auto argument = static_cast<std::size_t>(pattern[index + 1U] - '0');
                if (argument < arguments.size()) result.append(arguments[argument]);
                else result.append(pattern.substr(index, 3U));
                index += 3U;
            } else {
                result.push_back(pattern[index++]);
            }
        }
        return result;
    }

    const CabbirdLocalizationServiceV1* service_{};
};

}  // namespace cabbird::sdk

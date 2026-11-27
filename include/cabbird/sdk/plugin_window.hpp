#pragma once

#include "cabbird/sdk/cpp.hpp"

#include <cmath>
#include <string>

namespace cabbird::sdk {

class PluginWindow final {
   public:
    PluginWindow(const CabbirdUiServiceV1* ui, const std::string& title, float& width, float& height,
                 bool& initialized)
        : ui_(ui), width_(width), height_(height) {
        if (!initialized) {
            ui_->set_next_window_size(ui_->user, width, height, 0);
            initialized = true;
        }
        // The host captures close requests and owns persisted visibility.
        int open = 1;
        active_ = ui_->begin_window(ui_->user, StringView(title), &open, 0) != 0;
    }
    ~PluginWindow() {
        float width{}, height{};
        ui_->get_window_size(ui_->user, &width, &height);
        if (std::isfinite(width) && width >= 100 && width <= 8192 &&
            std::isfinite(height) && height >= 100 && height <= 8192) {
            width_ = width;
            height_ = height;
        }
        ui_->end_window(ui_->user);
    }
    explicit operator bool() const noexcept {
        return active_;
    }

   private:
    const CabbirdUiServiceV1* ui_;
    float& width_;
    float& height_;
    bool active_{};
};

} // namespace cabbird::sdk

#pragma once

#include "cabbird/thread_local_value.hpp"

namespace cabbird::embedded {

// One scope covers both Present entry points and the call to the next hook.
// A proxy may forward Present -> Present1 -> native Present; none of those
// nested calls may render a second overlay or tear down the outer frame.
class EmbeddedPresentScope final {
public:
    explicit EmbeddedPresentScope(cabbird::ThreadLocalScalar<bool>& active) noexcept
        : active_(active), outermost_(!active.Get()) {
        if (outermost_) active_.Set(true);
    }
    ~EmbeddedPresentScope() {
        if (outermost_) active_.Set(false);
    }
    EmbeddedPresentScope(const EmbeddedPresentScope&) = delete;
    EmbeddedPresentScope& operator=(const EmbeddedPresentScope&) = delete;
    [[nodiscard]] bool Outermost() const noexcept { return outermost_; }

private:
    cabbird::ThreadLocalScalar<bool>& active_;
    bool outermost_{};
};

} // namespace cabbird::embedded

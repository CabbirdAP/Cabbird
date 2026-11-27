/* DispatchTickHook -- see the header for why this is not `GameTickHook`.
 *
 * The short version: `GameTickHook`'s thunk carries Unreal's `(self, delta, idle)` signature,
 * and the game's `UPlayerLoop.Dispatch` takes no arguments.  Pointing one at the other would
 * compile, install, and hand plugins a delta read from an XMM register nobody wrote.  This
 * file installs a thunk that changes nothing about the ABI.
 */

#include "cabbird/dispatch_tick_hook.hpp"

#include "cabbird/unity_build_profile.hpp"
#include "cabbird/hook_manager.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cabbird {
namespace {

constexpr std::string_view kOwner = "cabbird.unity.framework";
constexpr std::uint64_t kGeneration = 1;
constexpr std::string_view kLabel = "unity-playerloop-dispatch";

// The profile key.  The name of the game-side function lives in the data file; this constant
// is only the handle used to fetch it.
constexpr const char* kMethodKey = "unity.playerloop.dispatch";

// Static IL2CPP methods carry a hidden MethodInfo pointer even with zero C# parameters.
using DispatchFunction = void(__fastcall*)(const void* method_info);
std::atomic<DispatchFunction> g_original_dispatch;

// Set only between install and removal.  The thunk reads it; `Start`/`Stop` write it.
std::atomic<std::shared_ptr<const DispatchTickHook::Callback>> g_callback;

std::atomic<std::uint64_t> g_ticks{0};

// Frame clock for the delta handed to plugins.
//
// The dispatcher is called once per frame but tells us nothing about time, so the delta is
// measured here.  Steady clock, not wall clock: a wall-clock delta jumps when NTP steps the
// system time and every plugin that integrates it would jump too.
std::atomic<std::int64_t> g_last_tick_ns{0};

double MeasureDeltaSeconds() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    const auto previous = g_last_tick_ns.exchange(now_ns, std::memory_order_relaxed);
    if (previous == 0 || now_ns <= previous) return 0.0;
    // Clamped: a debugger breakpoint or a long stall must not deliver a ten-second delta to
    // every plugin at once.  A quarter second is already far past any real frame.
    constexpr double kMaxDeltaSeconds = 0.25;
    const auto seconds = static_cast<double>(now_ns - previous) / 1.0e9;
    return seconds > kMaxDeltaSeconds ? kMaxDeltaSeconds : seconds;
}

// Forward the complete native ABI explicitly. Register preservation is not a promise
// a zero-argument C++ thunk can make, especially in Debug builds.
void __fastcall DispatchThunk(const void* method_info) {
    const auto original = g_original_dispatch.load(std::memory_order_acquire);
    if (original == nullptr) return;
    const auto callback = g_callback.load(std::memory_order_acquire);

    // The original runs FIRST.  It is what advances the game's own frame; making the game wait
    // on plugin work before its frame has run is how a plugin stall becomes a game stall.
    // Plugins observe a frame that has already happened.
    original(method_info);

    // A frame admitted before Stop/Start must not observe the next generation's callback.
    if (callback == nullptr || g_callback.load(std::memory_order_acquire) != callback) return;

    g_ticks.fetch_add(1, std::memory_order_relaxed);

    try {
        (*callback)(MeasureDeltaSeconds());
    } catch (...) {
        // Swallowed deliberately.  This runs inside the game's frame loop: an exception that
        // escaped here would unwind into IL2CPP and take the process with it.  The plugin layer
        // records the offending plugin as faulted on its own side.
    }
}

}  // namespace

class DispatchTickHook::Impl final {
public:
    Impl(std::filesystem::path profile_path, Callback callback)
        : profile_path_(std::move(profile_path)),
          callback_(std::make_shared<const Callback>(std::move(callback))) {
        if (!*callback_) throw std::invalid_argument("DispatchTickHook requires callback");
    }

    ~Impl() { Stop(); }

    bool Start() noexcept {
        if (started_.load(std::memory_order_acquire)) return true;
        if (!Resolve()) return false;
        if (!Install()) return false;
        g_last_tick_ns.store(0, std::memory_order_relaxed);
        g_ticks.store(0, std::memory_order_relaxed);
        g_callback.store(callback_, std::memory_order_release);
        started_.store(true, std::memory_order_release);
        return true;
    }

    void Stop() noexcept {
        if (!started_.exchange(false, std::memory_order_acq_rel)) return;
        // Retire the callback BEFORE the detour comes out, so a tick already inside the thunk
        // finishes and every later one finds nothing to call.
        g_callback.store(nullptr, std::memory_order_release);
        if (hooks_ != nullptr) {
            static_cast<void>(hooks_->RemoveOwner(kOwner, kGeneration));
        }
        g_original_dispatch.store(nullptr, std::memory_order_release);
    }

    [[nodiscard]] bool Started() const noexcept {
        return started_.load(std::memory_order_acquire);
    }
    [[nodiscard]] const Callback& InstalledCallback() const noexcept { return *callback_; }
    [[nodiscard]] const std::string& LastError() const noexcept { return error_; }
    [[nodiscard]] std::uint64_t TickCount() const noexcept {
        return g_ticks.load(std::memory_order_relaxed);
    }
    [[nodiscard]] const std::string& ResolutionHow() const noexcept { return how_; }
    [[nodiscard]] std::uintptr_t ResolutionAddress() const noexcept { return address_; }
    [[nodiscard]] const std::string& BuildId() const noexcept { return build_id_; }

private:
    // Profile + metadata -> one verified address.  Both routes report why they failed and the
    // combined reason is kept, so a failure names every reason it had rather than the last one
    // tried.
    bool Resolve() noexcept {
        std::string error;
        profile_ = UnityBuildProfile::LoadFromFile(profile_path_, &error);
        if (!profile_.Valid()) {
            error_ = "build profile unusable: " + error;
            return false;
        }
        build_id_ = profile_.BuildId();

        const auto method = profile_.Resolve(kMethodKey);
        if (!method.resolved) {
            error_ = "cannot resolve " + std::string(kMethodKey) + ": " + method.how;
            return false;
        }
        address_ = method.address;
        how_ = method.how;
        return true;
    }

    bool Install() noexcept {
        auto* const target = reinterpret_cast<void*>(address_);
        hooks_ = std::make_unique<HookManager>(CreateMinHookBackend());
        void* original = nullptr;
        if (!hooks_->Create(std::string(kOwner), kGeneration, std::string(kLabel), target,
                            reinterpret_cast<void*>(&DispatchThunk), &original)) {
            error_ = "HookManager::Create failed for " + std::string(kMethodKey) + " at 0x" +
                     Hex(address_);
            hooks_.reset();
            return false;
        }
        g_original_dispatch.store(reinterpret_cast<DispatchFunction>(original), std::memory_order_release);
        if (!hooks_->EnableOwner(kOwner, kGeneration)) {
            error_ = "HookManager::EnableOwner failed for " + std::string(kMethodKey);
            static_cast<void>(hooks_->RemoveOwner(kOwner, kGeneration));
            hooks_.reset();
            g_original_dispatch.store(nullptr, std::memory_order_release);
            return false;
        }
        return true;
    }

    static std::string Hex(std::uint64_t value) {
        constexpr char kDigits[] = "0123456789ABCDEF";
        std::string text;
        bool started = false;
        for (int shift = 60; shift >= 0; shift -= 4) {
            const auto nibble = static_cast<unsigned>((value >> shift) & 0xFULL);
            if (nibble != 0 || started || shift == 0) {
                started = true;
                text.push_back(kDigits[nibble]);
            }
        }
        return text;
    }

    std::filesystem::path profile_path_;
    std::shared_ptr<const Callback> callback_;
    UnityBuildProfile profile_;
    std::unique_ptr<HookManager> hooks_;
    std::string build_id_;
    std::string how_;
    std::uintptr_t address_{};
    std::string error_;
    std::atomic_bool started_{false};
};

DispatchTickHook::DispatchTickHook(std::filesystem::path profile_path, Callback callback)
    : impl_(std::make_unique<Impl>(std::move(profile_path), std::move(callback))) {}

DispatchTickHook::~DispatchTickHook() = default;

bool DispatchTickHook::Start() noexcept { return impl_->Start(); }
void DispatchTickHook::Stop() noexcept { impl_->Stop(); }
bool DispatchTickHook::Started() const noexcept { return impl_->Started(); }
const std::string& DispatchTickHook::LastError() const noexcept { return impl_->LastError(); }

const DispatchTickHook::Callback& DispatchTickHook::InstalledCallback() const noexcept {
    return impl_->InstalledCallback();
}std::uint64_t DispatchTickHook::TickCount() const noexcept { return impl_->TickCount(); }
const std::string& DispatchTickHook::ResolutionHow() const noexcept { return impl_->ResolutionHow(); }
std::uintptr_t DispatchTickHook::ResolutionAddress() const noexcept {
    return impl_->ResolutionAddress();
}
const std::string& DispatchTickHook::BuildId() const noexcept { return impl_->BuildId(); }
const char* DispatchTickHook::MethodKey() noexcept { return kMethodKey; }

}  // namespace cabbird

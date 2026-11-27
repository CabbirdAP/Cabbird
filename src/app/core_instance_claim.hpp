#pragma once

#include <Windows.h>

#include <string>

namespace cabbird::app {

// Each mapped Core has separate C++ globals. A named kernel object provides
// one process-wide owner, including across stopped and restarted sessions.
class CoreInstanceClaim final {
public:
    CoreInstanceClaim() = default;
    CoreInstanceClaim(const CoreInstanceClaim&) = delete;
    CoreInstanceClaim& operator=(const CoreInstanceClaim&) = delete;
    ~CoreInstanceClaim() { if (handle_ != nullptr) CloseHandle(handle_); }

    // Called under RuntimeControl::mutex, outside Loader Lock.
    [[nodiscard]] DWORD Acquire() {
        if (handle_ != nullptr) return ERROR_SUCCESS;
        const auto name = L"Local\\Cabbird.Core.Owner." +
            std::to_wstring(GetCurrentProcessId());
        const HANDLE candidate = CreateMutexW(nullptr, FALSE, name.c_str());
        const DWORD error = GetLastError();
        if (candidate == nullptr) return error;
        if (error == ERROR_ALREADY_EXISTS) {
            CloseHandle(candidate);
            return ERROR_ALREADY_INITIALIZED;
        }
        handle_ = candidate;
        return ERROR_SUCCESS;
    }

private:
    HANDLE handle_{};
};

}  // namespace cabbird::app

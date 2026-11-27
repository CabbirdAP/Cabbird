/* Four execution domains: Lifecycle and Worker own host threads; Game and Render
 * borrow engine/presenter threads through explicit pumps. Managed Unity objects may
 * only be accessed on Game. Invoke owns queued callbacks across bounded waits;
 * service start/stop uses WaitForRunning to preserve synchronous lifetime boundaries.
 */
#pragma once

#include "cabbird/dispatcher.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cabbird {

enum class ExecutionDomain : std::uint8_t {
    Lifecycle,
    Worker,
    Game,
    Render,
};

enum class InvocationTimeoutPolicy {
    ReturnWhileRunning,
    // Queued work can time out, but a running callback must finish before returning.
    // Service lifecycle callbacks use this so teardown cannot race a late start/stop.
    WaitForRunning,
};

const char* ExecutionDomainName(ExecutionDomain domain) noexcept;

/* True for domains that are pumped by an engine-owned thread rather than by a thread this
 * class created.  Callers use this to decide whether posting alone is enough to make work
 * happen -- it is not, for these two. */
[[nodiscard]] bool IsPumpedDomain(ExecutionDomain domain) noexcept;

/* A posted task's location: which domain, which worker lane, and the underlying handle.
 *
 * `lane` is part of the identity, not a detail: two Worker tasks can share a TaskHandle
 * value because they live in different Dispatcher instances, so cancelling by handle alone
 * would be ambiguous. */
struct DomainTaskHandle {
    ExecutionDomain domain{ExecutionDomain::Lifecycle};
    std::size_t lane{};
    TaskHandle task;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(task);
    }
    friend bool operator==(DomainTaskHandle, DomainTaskHandle) = default;
};

struct RuntimeDispatchersOptions {
    std::size_t worker_threads{1};
    std::size_t terminal_history_capacity{1024};
};

class RuntimeDispatchers final {
public:
    using Callback = Dispatcher::Callback;

    /* The owner must outlive callbacks.  Destroying this object from inside one of its own
     * callbacks is invalid -- the self-join guard in JoinWorkers covers the Worker case
     * (a worker joining itself), but destroying the object underneath a running callback
     * cannot be made safe and is not attempted. */
    explicit RuntimeDispatchers(RuntimeDispatchersOptions options = {});
    ~RuntimeDispatchers();

    RuntimeDispatchers(const RuntimeDispatchers&) = delete;
    RuntimeDispatchers& operator=(const RuntimeDispatchers&) = delete;
    RuntimeDispatchers(RuntimeDispatchers&&) = delete;
    RuntimeDispatchers& operator=(RuntimeDispatchers&&) = delete;

    [[nodiscard]] bool StartWorkers() noexcept;

    /* Stops accepting NEW external posts and cancels queued external work, while leaving
     * the domain workers alive.  The distinction matters during shutdown: lifecycle stop
     * callbacks still need thread affinity after external work has been refused, so
     * closing posts must not also kill the threads that will run them. */
    void CloseExternalPosts() noexcept;
    [[nodiscard]] bool DrainExternalWork(std::chrono::milliseconds timeout) noexcept;
    void RequestStop() noexcept;
    void JoinWorkers() noexcept;

    /* Binds the lifecycle dispatcher to the CURRENT thread.  Called before startup
     * callbacks begin, because runtime startup runs on this same thread before
     * RunLifecycle enters its pump -- and Invoke() must see that binding, or startup's own
     * affinity calls would deadlock waiting for a pump that has not started. */
    void BindLifecycleToCurrentThread() noexcept;

    [[nodiscard]] DomainTaskHandle Post(
        ExecutionDomain domain,
        std::string owner,
        std::uint64_t generation,
        Callback callback);

    /* Runs one bounded synchronous operation on a domain and waits up to `timeout`.
     *
     * IF THE CALLER IS ALREADY ON THE DOMAIN'S THREAD the callback runs inline -- not
     * queued.  Without that, an affinity call made from inside a domain callback would
     * wait for the very pump it is blocking, and time out every time.
     *
     * Returns ERROR_SUCCESS, or one of ERROR_TIMEOUT / ERROR_CANCELLED / ERROR_NOT_READY /
     * ERROR_INVALID_FUNCTION / ERROR_INVALID_PARAMETER / ERROR_UNHANDLED_EXCEPTION.
     *
     * With ReturnWhileRunning, ERROR_TIMEOUT does NOT mean "did not run"; see
     * DrainInvocations. WaitForRunning cancels queued work at the deadline, then
     * waits for completion if the callback has already entered its domain. */
    [[nodiscard]] DWORD Invoke(
        ExecutionDomain domain,
        Callback callback,
        std::chrono::milliseconds timeout = std::chrono::seconds(30),
        InvocationTimeoutPolicy timeout_policy = InvocationTimeoutPolicy::ReturnWhileRunning);

    /* Waits until no invocation is in flight.
     *
     * REQUIRED after ERROR_TIMEOUT before destroying anything the callback captured.
     * The callback's captures are released before its completion is published (the
     * ordering is enforced in InvocationCallbackState's destructor), so a drain that
     * returns true is a real lifetime boundary: after it, nothing is still holding them. */
    [[nodiscard]] bool DrainInvocations(
        std::chrono::milliseconds timeout = std::chrono::seconds(30));

    /* Cancels queued invocation callbacks but leaves the admission gate OPEN, so teardown
     * can still make the affinity calls it needs. */
    void CancelQueuedInvocations() noexcept;

    /* Closes the invocation admission gate and cancels queued invocation callbacks,
     * without touching unrelated work in the domains. */
    void CloseInvocations() noexcept;

    [[nodiscard]] bool Cancel(DomainTaskHandle handle);
    [[nodiscard]] std::size_t CancelOwnerGeneration(
        std::string_view owner, std::uint64_t generation);
    void SetGeneration(std::string owner, std::uint64_t generation);
    [[nodiscard]] bool Drain(
        std::string_view owner,
        std::uint64_t generation,
        std::chrono::milliseconds timeout);

    /* Lifecycle's thread body.  Blocks until stop is requested. */
    void RunLifecycle(std::stop_token stop_token) noexcept;

    /* Services can borrow the bound Lifecycle thread while startup or shutdown
     * waits for the graph coordinator. Call only from that thread. */
    [[nodiscard]] std::size_t PumpLifecycle(
        std::size_t max_callbacks = (std::numeric_limits<std::size_t>::max)()) noexcept;

    /* Pumped by the caller's thread.  See IsPumpedDomain. */
    [[nodiscard]] std::size_t PumpGame(
        std::size_t max_callbacks = (std::numeric_limits<std::size_t>::max)()) noexcept;
    [[nodiscard]] std::size_t PumpRender(
        std::size_t max_callbacks = (std::numeric_limits<std::size_t>::max)()) noexcept;

    [[nodiscard]] std::optional<TaskSnapshot> GetTask(DomainTaskHandle handle) const;
    [[nodiscard]] std::thread::id BoundThread(
        ExecutionDomain domain, std::size_t lane = 0) const noexcept;
    [[nodiscard]] std::size_t WorkerCount() const noexcept;
    // Anomaly's name.  A previous dispatch of this header renamed it to AcceptsNewWork(); the
    // rename carried no behaviour change and only made the two projects harder to diff, so the
    // upstream spelling is restored.
    [[nodiscard]] bool IsAccepting() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cabbird

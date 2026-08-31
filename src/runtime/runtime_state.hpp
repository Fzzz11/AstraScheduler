#ifndef ASTRA_SRC_RUNTIME_RUNTIME_STATE_HPP
#define ASTRA_SRC_RUNTIME_RUNTIME_STATE_HPP

#include "runtime/admission_controller.hpp"
#include "runtime/ready_queues.hpp"
#include "runtime/runtime_diagnostics.hpp"
#include "runtime/runtime_identity.hpp"
#include "runtime/runtime_metrics.hpp"
#include "runtime/timer_queue.hpp"

#include <astra/scheduler.hpp>
#include <astra/trace.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace astra::detail {

// Shutdown Completion 的同步对象必须能比 RuntimeState/Impl 活得更久：
// Reaper Waiter 可能在 Leader 已 delete Impl 之后仍位于 condition_variable 谓词中（R-020）。
struct ShutdownCompletion {
    std::mutex mutex;
    std::condition_variable cv;
    bool in_progress{false};
    std::atomic<bool> stopped{false};
};

// 单个 Runtime 的唯一组合状态所有者（R-130）。Scheduler::Impl 只负责
// shared ownership 与 facade/Graph port 适配，不再重复拥有运行时状态。
struct RuntimeState {
    RuntimeState(RuntimeId id, SchedulerOptions options, SchedulerCapabilities capabilities);

    RuntimeState(const RuntimeState&) = delete;
    RuntimeState& operator=(const RuntimeState&) = delete;

    static constexpr std::uint16_t pack(SchedulerState state, ShutdownMode mode) noexcept {
        return static_cast<std::uint16_t>((static_cast<std::uint8_t>(state) << 8) |
                                          static_cast<std::uint8_t>(mode));
    }

    static constexpr SchedulerStatus unpack(std::uint16_t value) noexcept {
        return SchedulerStatus{
            static_cast<SchedulerState>((value >> 8) & 0xFF),
            static_cast<ShutdownMode>(value & 0xFF)};
    }

    [[nodiscard]] SchedulerStatus status() const noexcept {
        return unpack(packed_status.load(std::memory_order_acquire));
    }

    [[nodiscard]] SchedulerStatus get_status() const noexcept { return status(); }
    void process_due_timers();
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> earliest_wake_time();
    [[nodiscard]] bool has_timers();
    void release_external_slot_after_claim(const ReadyQueues::QueuedTask& task) noexcept;

    RuntimeId runtime_id;
    RuntimeIdentityAllocator identities;
    SchedulerOptions options;
    SchedulerCapabilities capabilities;
    std::atomic<std::uint16_t> packed_status;

    RuntimeMetrics metrics;
    RuntimeDiagnostics diagnostics;
    AdmissionController admission;
    TimerQueue timers;
    ReadyQueues ready_queues;

    std::mutex lifecycle_mutex;
    std::condition_variable startup_cv;
    std::condition_variable work_cv;
    bool startup_done{false};
    bool startup_failed{false};
    bool stop_requested{false};
    std::atomic<bool> handoff_dispatched{false};

    std::size_t workers_ready{0};
    std::atomic<std::size_t> active_workers{0};
    std::atomic<std::size_t> parked_workers{0};
    std::atomic<std::uint64_t> work_epoch{0};
    std::vector<std::thread> worker_threads;

    std::shared_ptr<ShutdownCompletion> shutdown_completion{
        std::make_shared<ShutdownCompletion>()};
};

}  // namespace astra::detail

#endif  // ASTRA_SRC_RUNTIME_RUNTIME_STATE_HPP

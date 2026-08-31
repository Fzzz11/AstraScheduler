#ifndef ASTRA_SRC_RUNTIME_READY_QUEUES_HPP
#define ASTRA_SRC_RUNTIME_READY_QUEUES_HPP

#include "runtime/admission_controller.hpp"
#include "runtime/runtime_metrics.hpp"
#include "scheduling/chase_lev_deque.hpp"

#include <astra/capabilities.hpp>
#include <astra/task_handle.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace astra::detail {

// Ready work 从 publication 到唯一 claim/cancel 的所有者（R-129）。
// lifecycle、park epoch 与 shutdown mode 均由 RuntimeState 协调，本类型只拥有
// 队列、选择算法和已 claim 尚未完成的计数。
class ReadyQueues final {
public:
    struct QueuedTask {
        std::unique_ptr<TaskInvokerBase> invoker;
        bool is_external{false};
    };

    ReadyQueues(
        std::size_t worker_count,
        RuntimeMetrics& metrics,
        LocalDequeBackend local_backend);

    ReadyQueues(const ReadyQueues&) = delete;
    ReadyQueues& operator=(const ReadyQueues&) = delete;

    [[nodiscard]] static LocalDequeBackend preferred_local_backend() noexcept;

    void publish(
        std::unique_ptr<TaskInvokerBase> task,
        bool is_external,
        bool use_local_queue,
        std::size_t worker_index);

    bool claim_local(
        std::size_t worker_index,
        std::size_t& calendar_index,
        QueuedTask& out);
    bool claim_global(
        std::size_t& calendar_index,
        std::array<std::size_t, 4>& deadline_bursts,
        QueuedTask& out);
    bool steal(
        std::size_t victim_index,
        std::size_t& calendar_index,
        QueuedTask& out);

    void complete_claim() noexcept;
    [[nodiscard]] std::size_t claimed_count() const noexcept;

    void cancel_unstarted(AdmissionController& admission) noexcept;

    [[nodiscard]] bool global_empty() const;
    [[nodiscard]] bool local_empty(std::size_t worker_index) const;
    [[nodiscard]] bool any_local_work() const;
    [[nodiscard]] bool any_queued_work() const;
    [[nodiscard]] bool any_resume_work_after_immediate_cleanup() const;
    [[nodiscard]] std::size_t global_size() const;
    [[nodiscard]] std::size_t worker_count() const noexcept;
    void set_local_growth_failure_for_testing(
        std::size_t worker_index,
        bool inject) noexcept;
    void set_local_band_maintenance_for_testing(
        std::size_t worker_index,
        Priority priority,
        bool enabled) noexcept;

private:
    static constexpr std::size_t kPriorityCalendarLength = 15;
    static constexpr std::size_t kEdfDeadlineBurstLimit = 8;
    static constexpr std::array<Priority, kPriorityCalendarLength> kPriorityCalendar = {
        Priority::Critical, Priority::High, Priority::Critical, Priority::Normal,
        Priority::Critical, Priority::High, Priority::Critical, Priority::Low,
        Priority::Critical, Priority::High, Priority::Critical, Priority::Normal,
        Priority::Critical, Priority::High, Priority::Critical};
    static constexpr std::array<Priority, 4> kFallbackPriorityOrder = {
        Priority::Critical, Priority::High, Priority::Normal, Priority::Low};

    struct EdfEntry {
        TaskDeadline deadline;
        std::uint64_t admission_sequence{0};
        QueuedTask task;

        bool operator>(const EdfEntry& other) const noexcept;
    };

    struct IntrusiveFifo {
        TaskInvokerBase* head{nullptr};
        TaskInvokerBase* tail{nullptr};
        std::size_t count{0};

        IntrusiveFifo() = default;
        ~IntrusiveFifo();
        IntrusiveFifo(const IntrusiveFifo&) = delete;
        IntrusiveFifo& operator=(const IntrusiveFifo&) = delete;
        IntrusiveFifo(IntrusiveFifo&& other) noexcept;
        IntrusiveFifo& operator=(IntrusiveFifo&& other) noexcept;

        void push_back(std::unique_ptr<TaskInvokerBase> task, bool is_external) noexcept;
        bool pop_front(QueuedTask& out) noexcept;
        [[nodiscard]] bool empty() const noexcept { return head == nullptr; }
        [[nodiscard]] std::size_t size() const noexcept { return count; }
        [[nodiscard]] bool any_resume() const noexcept;
    };

    struct LocalQueues {
        explicit LocalQueues(LocalDequeBackend backend);
        ~LocalQueues();

        LocalDequeBackend backend;
        [[nodiscard]] bool uses_chase_lev() const noexcept {
            return backend == LocalDequeBackend::ChaseLevLockFree;
        }
        mutable std::mutex locked_mutex;
        std::array<std::deque<QueuedTask>, 4> locked_bands;
        std::array<std::unique_ptr<ChaseLevDeque<TaskInvokerBase*>>, 4> chase_lev_bands;

        bool push(QueuedTask& task, Priority priority);
        bool claim_back(std::size_t& calendar_index, QueuedTask& out);
        bool steal_front(std::size_t& calendar_index, QueuedTask& out);
        [[nodiscard]] bool empty() const;
        void cancel_unstarted(std::vector<QueuedTask>& resumes) noexcept;
        void set_growth_failure_for_testing(bool inject) noexcept;
        void set_band_maintenance_for_testing(Priority priority, bool enabled) noexcept;

    private:
        bool claim_chase_lev(
            std::size_t& calendar_index,
            bool owner,
            QueuedTask& out);
    };

    bool claim_global_band_locked(
        std::size_t band_index,
        std::size_t& deadline_burst,
        QueuedTask& out);
    static bool choose_local_band(
        std::array<std::deque<QueuedTask>, 4>& bands,
        std::size_t& calendar_index,
        bool from_back,
        QueuedTask& out);
    void record_claim(bool local) noexcept;

    RuntimeMetrics& metrics_;
    mutable std::mutex global_mutex_;
    std::atomic<std::uint64_t> global_admission_sequence_{0};
    std::array<std::vector<EdfEntry>, 4> global_edf_heaps_;
    std::array<IntrusiveFifo, 4> global_fifo_queues_;
    std::vector<std::unique_ptr<LocalQueues>> local_queues_;
    std::atomic<std::size_t> claimed_count_{0};
};

}  // namespace astra::detail

#endif  // ASTRA_SRC_RUNTIME_READY_QUEUES_HPP

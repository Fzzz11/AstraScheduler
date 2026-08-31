#ifndef ASTRA_TASK_CONTROL_BLOCK_HPP
#define ASTRA_TASK_CONTROL_BLOCK_HPP

#include <astra/task_handle.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace astra::detail {

class TaskControlBlock {
  public:
    explicit TaskControlBlock(TaskId id, Priority priority = Priority::Normal,
                              std::optional<TaskDeadline> deadline = std::nullopt);
    ~TaskControlBlock();

    TaskControlBlock(const TaskControlBlock&) = delete;
    TaskControlBlock& operator=(const TaskControlBlock&) = delete;

    void mark_resume_handoff() noexcept {
        resume_handoff_seq_.fetch_add(1, std::memory_order_acq_rel);
    }

    [[nodiscard]] std::uint64_t resume_handoff_seq() const noexcept {
        return resume_handoff_seq_.load(std::memory_order_acquire);
    }

    // R-074 / D-118：await_suspend 返回前不得 resume。handoff 后由 awaiter
    // 发布此序号，ResumeInvoker 等到可见后再 coro.resume()。
    void publish_await_suspend() noexcept;
    void wait_for_await_suspend_publication() const noexcept;

    [[nodiscard]] TaskId id() const noexcept {
        return id_;
    }

    [[nodiscard]] Priority priority() const noexcept {
        return priority_;
    }

    [[nodiscard]] std::optional<TaskDeadline> deadline() const noexcept {
        return deadline_;
    }

    [[nodiscard]] DeadlineDisposition deadline_disposition() const noexcept;

    [[nodiscard]] std::stop_token stop_token() const noexcept {
        return stop_source_.get_token();
    }

    [[nodiscard]] std::chrono::steady_clock::time_point admitted_at() const noexcept {
        return std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(admitted_at_ns_.load(std::memory_order_relaxed)));
    }

    [[nodiscard]] std::chrono::steady_clock::time_point ready_published_at() const noexcept {
        return std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(ready_published_at_ns_.load(std::memory_order_acquire)));
    }

    void set_ready_published_at(std::chrono::steady_clock::time_point tp) noexcept;

    void set_rescheduler(TaskRescheduler rescheduler);
    [[nodiscard]] TaskRescheduler get_rescheduler() const;
    void set_timer_functions(TimerRegistrar reg, TimerCanceller cancel);
    [[nodiscard]] TimerRegistrar get_timer_registrar() const;
    [[nodiscard]] TimerCanceller get_timer_canceller() const;

    void transition_to_suspended() noexcept;
    void transition_to_running() noexcept;
    void add_completion_callback(std::function<void()> cb);
    void record_terminal_wall_time() noexcept;
    void request_cancel() noexcept;
    bool try_start() noexcept;
    void succeed();
    void mark_observed() const noexcept;

    [[nodiscard]] TaskState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::condition_variable& cv() const noexcept {
        return cv_;
    }

    [[nodiscard]] std::mutex& mutex() const noexcept {
        return mutex_;
    }

    void set_exception(std::exception_ptr ex) noexcept;
    void set_cancelled() noexcept;
    [[nodiscard]] std::exception_ptr exception() const noexcept;

    [[nodiscard]] bool is_completed() const noexcept {
        const auto s = state_.load(std::memory_order_acquire);
        return s == TaskState::Succeeded || s == TaskState::Failed || s == TaskState::Cancelled;
    }

    [[nodiscard]] bool is_completed_locked() const noexcept {
        const auto s = state_.load(std::memory_order_relaxed);
        return s == TaskState::Succeeded || s == TaskState::Failed || s == TaskState::Cancelled;
    }

  private:
    TaskId id_;
    Priority priority_{Priority::Normal};
    std::optional<TaskDeadline> deadline_{std::nullopt};
    DeadlineDisposition deadline_disposition_{DeadlineDisposition::None};
    std::stop_source stop_source_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::atomic<TaskState> state_{TaskState::Ready};
    std::exception_ptr exception_{nullptr};
    mutable std::atomic<bool> observed_{false};
    std::atomic<std::uint64_t> resume_handoff_seq_{0};
    std::atomic<std::uint64_t> suspend_published_seq_{0};
    std::vector<std::function<void()>> completion_callbacks_;
    TaskRescheduler rescheduler_;
    TimerRegistrar timer_registrar_{nullptr};
    TimerCanceller timer_canceller_{nullptr};
    std::atomic<std::int64_t> admitted_at_ns_{0};
    std::atomic<std::int64_t> ready_published_at_ns_{0};
};

} // namespace astra::detail

#endif // ASTRA_TASK_CONTROL_BLOCK_HPP

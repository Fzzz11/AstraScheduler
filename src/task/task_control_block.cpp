// TaskControlBlock —— 任务运行协议（mutex、回调、rescheduler、timer、终态）。
// 值存储在 TaskHandle 的 private nested 结果格中（R-119 / D-173）。

#include "task_control_block.hpp"
#include "ready_linked_invoker.hpp"

#include <chrono>
#include <utility>

namespace astra::detail {

TaskControlBlock::TaskControlBlock(
    TaskId id,
    Priority priority,
    std::optional<TaskDeadline> deadline)
    : id_(id), priority_(priority), deadline_(deadline),
      admitted_at_ns_(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count()),
      ready_published_at_ns_(admitted_at_ns_.load(std::memory_order_relaxed)) {}

TaskControlBlock::~TaskControlBlock() {
    if (state_.load(std::memory_order_relaxed) == TaskState::Failed &&
        !observed_.load(std::memory_order_relaxed)) {
        record_metrics_unobserved_failure(id_);
    }
}

DeadlineDisposition TaskControlBlock::deadline_disposition() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return deadline_disposition_;
}

void TaskControlBlock::set_ready_published_at(std::chrono::steady_clock::time_point tp) noexcept {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    ready_published_at_ns_.store(ns, std::memory_order_release);
}

void TaskControlBlock::set_rescheduler(TaskRescheduler rescheduler) {
    std::lock_guard<std::mutex> lock(mutex_);
    rescheduler_ = std::move(rescheduler);
}

auto TaskControlBlock::get_rescheduler() const -> TaskRescheduler {
    std::lock_guard<std::mutex> lock(mutex_);
    return rescheduler_;
}

void TaskControlBlock::set_timer_functions(TimerRegistrar reg, TimerCanceller cancel) {
    std::lock_guard<std::mutex> lock(mutex_);
    timer_registrar_ = std::move(reg);
    timer_canceller_ = std::move(cancel);
}

auto TaskControlBlock::get_timer_registrar() const -> TimerRegistrar {
    std::lock_guard<std::mutex> lock(mutex_);
    return timer_registrar_;
}

auto TaskControlBlock::get_timer_canceller() const -> TimerCanceller {
    std::lock_guard<std::mutex> lock(mutex_);
    return timer_canceller_;
}

void TaskControlBlock::transition_to_suspended() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.load(std::memory_order_relaxed) == TaskState::Running) {
        state_.store(TaskState::Suspended, std::memory_order_release);
        ready_published_at_ns_.store(0, std::memory_order_release);
        record_metrics_suspended(id_);
    }
}

void TaskControlBlock::transition_to_running() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.load(std::memory_order_relaxed) == TaskState::Suspended ||
        state_.load(std::memory_order_relaxed) == TaskState::Ready) {
        state_.store(TaskState::Running, std::memory_order_release);
    }
}

void TaskControlBlock::add_completion_callback(std::function<void()> cb) {
    bool run_immediately = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (is_completed_locked()) {
            run_immediately = true;
        } else {
            completion_callbacks_.push_back(std::move(cb));
        }
    }
    if (run_immediately && cb) {
        cb();
    }
}

void TaskControlBlock::record_terminal_wall_time() noexcept {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto admitted_ns = admitted_at_ns_.load(std::memory_order_relaxed);
    if (now_ns >= admitted_ns && admitted_ns > 0) {
        record_metrics_task_wall_time(id_, static_cast<std::uint64_t>(now_ns - admitted_ns));
    }
}

void TaskControlBlock::request_cancel() noexcept {
    std::vector<std::function<void()>> callbacks;
    bool was_ready = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_source_.request_stop();
        if (state_.load(std::memory_order_relaxed) == TaskState::Ready) {
            state_.store(TaskState::Cancelled, std::memory_order_release);
            callbacks = std::move(completion_callbacks_);
            was_ready = true;
        }
    }
    if (was_ready) {
        record_terminal_wall_time();
        record_metrics_cancelled_before_start(id_, deadline_.has_value());
    }
    cv_.notify_all();
    for (auto& cb : callbacks) {
        if (cb) {
            cb();
        }
    }
}

bool TaskControlBlock::try_start() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.load(std::memory_order_relaxed) == TaskState::Ready) {
        state_.store(TaskState::Running, std::memory_order_release);
        const auto now = std::chrono::steady_clock::now();
        if (deadline_.has_value() && deadline_disposition_ == DeadlineDisposition::None) {
            if (now <= deadline_->time_point()) {
                deadline_disposition_ = DeadlineDisposition::Met;
            } else {
                deadline_disposition_ = DeadlineDisposition::Missed;
                const auto lateness = std::chrono::duration_cast<std::chrono::nanoseconds>(now - deadline_->time_point()).count();
                record_metrics_deadline_start_lateness(id_, lateness);
            }
        }
        const auto pub_ns = ready_published_at_ns_.load(std::memory_order_acquire);
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        if (now_ns >= pub_ns && pub_ns > 0) {
            record_metrics_ready_queue_wait(id_, static_cast<std::uint64_t>(now_ns - pub_ns));
        }
        record_metrics_first_start(id_, deadline_.has_value() ? std::optional{deadline_disposition_} : std::nullopt);
        return true;
    }
    return false;
}

void TaskControlBlock::succeed() {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.store(TaskState::Succeeded, std::memory_order_release);
        callbacks = std::move(completion_callbacks_);
        rescheduler_ = nullptr;
    }
    record_terminal_wall_time();
    record_metrics_succeeded(id_);
    cv_.notify_all();
    for (auto& cb : callbacks) {
        if (cb) {
            cb();
        }
    }
}

void TaskControlBlock::mark_observed() const noexcept {
    observed_.store(true, std::memory_order_relaxed);
}

void TaskControlBlock::set_exception(std::exception_ptr ex) noexcept {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        exception_ = std::move(ex);
        state_.store(TaskState::Failed, std::memory_order_release);
        callbacks = std::move(completion_callbacks_);
        rescheduler_ = nullptr;
    }
    record_terminal_wall_time();
    record_metrics_failed(id_);
    cv_.notify_all();
    for (auto& cb : callbacks) {
        if (cb) {
            cb();
        }
    }
}

void TaskControlBlock::set_cancelled() noexcept {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.store(TaskState::Cancelled, std::memory_order_release);
        callbacks = std::move(completion_callbacks_);
        rescheduler_ = nullptr;
    }
    record_terminal_wall_time();
    record_metrics_cancelled_cooperative(id_);
    cv_.notify_all();
    for (auto& cb : callbacks) {
        if (cb) {
            cb();
        }
    }
}

std::exception_ptr TaskControlBlock::exception() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return exception_;
}

std::shared_ptr<TaskControlBlock> make_task_control_block(
    TaskId id,
    Priority priority,
    std::optional<TaskDeadline> deadline) {
    return std::make_shared<TaskControlBlock>(id, priority, deadline);
}

TaskId tcb_id(const TaskControlBlock& tcb) noexcept {
    return tcb.id();
}

Priority tcb_priority(const TaskControlBlock& tcb) noexcept {
    return tcb.priority();
}

std::optional<TaskDeadline> tcb_deadline(const TaskControlBlock& tcb) noexcept {
    return tcb.deadline();
}

DeadlineDisposition tcb_deadline_disposition(const TaskControlBlock& tcb) noexcept {
    return tcb.deadline_disposition();
}

TaskState tcb_state(const TaskControlBlock& tcb) noexcept {
    return tcb.state();
}

bool tcb_is_completed(const TaskControlBlock& tcb) noexcept {
    return tcb.is_completed();
}

std::stop_token tcb_stop_token(const TaskControlBlock& tcb) noexcept {
    return tcb.stop_token();
}

std::chrono::steady_clock::time_point tcb_ready_published_at(const TaskControlBlock& tcb) noexcept {
    return tcb.ready_published_at();
}

void tcb_set_ready_published_at(
    TaskControlBlock& tcb,
    std::chrono::steady_clock::time_point tp) noexcept {
    tcb.set_ready_published_at(tp);
}

std::uint64_t tcb_resume_handoff_seq(const TaskControlBlock& tcb) noexcept {
    return tcb.resume_handoff_seq();
}

void tcb_mark_resume_handoff(TaskControlBlock& tcb) noexcept {
    tcb.mark_resume_handoff();
}

void tcb_set_rescheduler(TaskControlBlock& tcb, TaskRescheduler rescheduler) {
    tcb.set_rescheduler(std::move(rescheduler));
}

TaskRescheduler tcb_get_rescheduler(const TaskControlBlock& tcb) {
    return tcb.get_rescheduler();
}

void tcb_set_timer_functions(
    TaskControlBlock& tcb,
    TimerRegistrar registrar,
    TimerCanceller canceller) {
    tcb.set_timer_functions(std::move(registrar), std::move(canceller));
}

TimerRegistrar tcb_get_timer_registrar(const TaskControlBlock& tcb) {
    return tcb.get_timer_registrar();
}

TimerCanceller tcb_get_timer_canceller(const TaskControlBlock& tcb) {
    return tcb.get_timer_canceller();
}

void tcb_transition_to_suspended(TaskControlBlock& tcb) noexcept {
    tcb.transition_to_suspended();
}

void tcb_transition_to_running(TaskControlBlock& tcb) noexcept {
    tcb.transition_to_running();
}

void tcb_add_completion_callback(TaskControlBlock& tcb, std::function<void()> cb) {
    tcb.add_completion_callback(std::move(cb));
}

void tcb_request_cancel(TaskControlBlock& tcb) noexcept {
    tcb.request_cancel();
}

bool tcb_try_start(TaskControlBlock& tcb) noexcept {
    return tcb.try_start();
}

void tcb_succeed(TaskControlBlock& tcb) {
    tcb.succeed();
}

void tcb_fail(TaskControlBlock& tcb, std::exception_ptr ex) noexcept {
    tcb.set_exception(std::move(ex));
}

void tcb_set_cancelled(TaskControlBlock& tcb) noexcept {
    tcb.set_cancelled();
}

std::exception_ptr tcb_exception(const TaskControlBlock& tcb) noexcept {
    return tcb.exception();
}

void tcb_mark_observed(const TaskControlBlock& tcb) noexcept {
    tcb.mark_observed();
}

class SubmittedInvokerGate final : public ReadyLinkedInvoker {
public:
    SubmittedInvokerGate(std::unique_ptr<TaskInvokerBase> inner, std::shared_ptr<TaskControlBlock> tcb)
        : inner_(std::move(inner)), tcb_(std::move(tcb)) {}

    void execute() override {
        if (!tcb_ || !inner_) {
            return;
        }
        if (!tcb_->try_start()) {
            inner_->cancel_pre_start();
            inner_->abandon_unstarted();
            return;
        }
        TaskExecutionContextGuard guard(tcb_->id(), tcb_->priority());
        const auto t_start = std::chrono::steady_clock::now();
        inner_->execute();
        const auto t_end = std::chrono::steady_clock::now();
        record_metrics_execution_segment(
            tcb_->id(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
    }

    void cancel_pre_start() noexcept override {
        if (inner_) {
            inner_->cancel_pre_start();
        }
    }

    void abandon_unstarted() noexcept override {
        if (inner_) {
            inner_->abandon_unstarted();
        }
    }

    [[nodiscard]] bool is_resume_segment() const noexcept override {
        return inner_ && inner_->is_resume_segment();
    }

    [[nodiscard]] bool is_coroutine_node() const noexcept override {
        return inner_ && inner_->is_coroutine_node();
    }

    [[nodiscard]] Priority priority() const noexcept override {
        return inner_ ? inner_->priority() : Priority::Normal;
    }

    [[nodiscard]] std::optional<TaskDeadline> deadline() const noexcept override {
        return inner_ ? inner_->deadline() : std::nullopt;
    }

private:
    std::unique_ptr<TaskInvokerBase> inner_;
    std::shared_ptr<TaskControlBlock> tcb_;
};

std::unique_ptr<TaskInvokerBase> wrap_submitted_invoker(
    std::unique_ptr<TaskInvokerBase> inner,
    std::shared_ptr<void> protocol) {
    return std::make_unique<SubmittedInvokerGate>(
        std::move(inner),
        std::static_pointer_cast<TaskControlBlock>(std::move(protocol)));
}

}  // namespace astra::detail

// TaskSharedState —— 任务结果的"共享信箱"实现。
//
// 【这个对象的生命周期】
//   Ready（已提交待执行）-> Running -> 终态（Succeeded / Failed /
//   Cancelled），终态不可逆。挂起恢复会在 Running/Suspended 之间往返。
//
// 【并发模型】
//   一把 mutex 保护"会变的字段"（状态机、回调列表、deadline 结论、
//   rescheduler）；两个原子变量（admitted/ready 时间戳）无锁读写，
//   供 Metrics 采样。终态写入在锁内完成，随后在锁外逐个执行完成回调——
//   回调里若再调用本对象的方法也不会死锁。
//
// 【为什么 notify 与回调在锁外】
//   持锁 notify 会把被唤醒者直接"顶"到锁上，白白多一次上下文切换；
//   回调在锁内执行则可能以任意顺序重入加锁，死锁风险不可控。
//   先收集、后锁外执行是本文件所有状态迁移方法的统一模式。
//
// 【AST-056】resume 所有权代际：协程挂起把帧移交给恢复者时递增代际；
//   发起恢复的 invoker 返回后比对代际，不一致就绝不触碰协程帧
//   （防止恢复者已销毁帧的 use-after-free）。
//
// 声明见 <astra/task_handle.hpp>。

#include <astra/task_handle.hpp>

#include <chrono>
#include <utility>

namespace astra::detail {

TaskSharedStateBase::TaskSharedStateBase(
    TaskId id,
    Priority priority,
    std::optional<TaskDeadline> deadline)
    : id_(id), priority_(priority), deadline_(deadline),
      admitted_at_ns_(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count()),
      ready_published_at_ns_(admitted_at_ns_.load(std::memory_order_relaxed)) {}

TaskSharedStateBase::~TaskSharedStateBase() {
    if (state_.load(std::memory_order_relaxed) == TaskState::Failed &&
        !observed_.load(std::memory_order_relaxed)) {
        record_metrics_unobserved_failure(id_);
    }
}

DeadlineDisposition TaskSharedStateBase::deadline_disposition() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return deadline_disposition_;
}

void TaskSharedStateBase::set_ready_published_at(std::chrono::steady_clock::time_point tp) noexcept {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    ready_published_at_ns_.store(ns, std::memory_order_release);
}

void TaskSharedStateBase::set_rescheduler(ReschedulerFunc rescheduler) {
    std::lock_guard<std::mutex> lock(mutex_);
    rescheduler_ = std::move(rescheduler);
}

auto TaskSharedStateBase::get_rescheduler() const -> ReschedulerFunc {
    std::lock_guard<std::mutex> lock(mutex_);
    return rescheduler_;
}

void TaskSharedStateBase::set_timer_functions(TimerRegistrar reg, TimerCanceller cancel) {
    std::lock_guard<std::mutex> lock(mutex_);
    timer_registrar_ = std::move(reg);
    timer_canceller_ = std::move(cancel);
}

auto TaskSharedStateBase::get_timer_registrar() const -> TimerRegistrar {
    std::lock_guard<std::mutex> lock(mutex_);
    return timer_registrar_;
}

auto TaskSharedStateBase::get_timer_canceller() const -> TimerCanceller {
    std::lock_guard<std::mutex> lock(mutex_);
    return timer_canceller_;
}

void TaskSharedStateBase::transition_to_suspended() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.load(std::memory_order_relaxed) == TaskState::Running) {
        state_.store(TaskState::Suspended, std::memory_order_release);
        ready_published_at_ns_.store(0, std::memory_order_release);
        record_metrics_suspended(id_);
    }
}

void TaskSharedStateBase::transition_to_running() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.load(std::memory_order_relaxed) == TaskState::Suspended ||
        state_.load(std::memory_order_relaxed) == TaskState::Ready) {
        state_.store(TaskState::Running, std::memory_order_release);
    }
}

void TaskSharedStateBase::add_completion_callback(std::function<void()> cb) {
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

void TaskSharedStateBase::record_terminal_wall_time() noexcept {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto admitted_ns = admitted_at_ns_.load(std::memory_order_relaxed);
    if (now_ns >= admitted_ns && admitted_ns > 0) {
        record_metrics_task_wall_time(id_, static_cast<std::uint64_t>(now_ns - admitted_ns));
    }
}

void TaskSharedStateBase::request_cancel() noexcept {
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

bool TaskSharedStateBase::try_start() noexcept {
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

void TaskSharedStateBase::set_exception(std::exception_ptr ex) noexcept {
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

void TaskSharedStateBase::set_cancelled() noexcept {
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

std::exception_ptr TaskSharedStateBase::exception() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return exception_;
}

}  // namespace astra::detail

#ifndef ASTRA_SCHEDULER_HPP
#define ASTRA_SCHEDULER_HPP

// AstraScheduler 调度器公共 Handle 与契约（AST-004 / R-098 / R-099 / R-100 / R-101 / D-155）。
// Scheduler 为可复制/移动的共享 Handle，析构或操作遵循生命周期契约。
// Supported Configuration 仅 64-bit Linux（R-111，经 export.hpp 检查）。

#include <astra/export.hpp>
#include <astra/capabilities.hpp>
#include <astra/coroutine.hpp>
#include <astra/error.hpp>
#include <astra/graph.hpp>
#include <astra/id.hpp>
#include <astra/metrics.hpp>
#include <astra/scheduler_options.hpp>
#include <astra/status.hpp>
#include <astra/task_handle.hpp>

#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace astra {

class Scheduler;
class AwaitHandshake;

namespace detail {
class GraphRunSharedState;

void perform_graph_caller_wait(const GraphRunSharedState&,
                               std::optional<std::chrono::steady_clock::time_point>);

enum class AdmissionDecision : std::uint8_t {
    Success,
    Stopping,
    Stopped,
    CapacityExhausted,
};

ASTRA_EXPORT RuntimeId current_worker_runtime_id() noexcept;
void run_test_task_on_worker(Scheduler& s, std::function<void()> task);
std::size_t global_injection_queue_size(const Scheduler& s);
std::size_t external_pending_count(const Scheduler& s);
std::size_t parked_workers_count(const Scheduler& s);
std::uint64_t current_work_epoch(const Scheduler& s);
}

// 调度器共享 Handle（D-155）。
class ASTRA_EXPORT Scheduler {
public:
    // 同步验证配置并初始化 Runtime（D-155 / D-157）。
    explicit Scheduler(SchedulerOptions options = {});

    ~Scheduler() noexcept;

    // 共享 Handle 语义：复制关联同一 Runtime State（D-155）。
    Scheduler(const Scheduler&);
    Scheduler& operator=(const Scheduler&);

    // 移动构造与移动赋值：使源 Handle 为空（D-155）。
    Scheduler(Scheduler&&) noexcept;
    Scheduler& operator=(Scheduler&&) noexcept;

    // 查询当前 Handle 是否关联有效 Runtime State（D-155）。
    // [[nodiscard]] 告诉编译器这个函数的返回值不应该被忽略，帮助开发者避免潜在的错误。
    [[nodiscard]] bool valid() const noexcept;

    // 获取当前 Runtime 的强类型唯一标识；空 Handle 返回默认无效 ID（D-153 / D-155）。
    [[nodiscard]] RuntimeId runtime_id() const noexcept;

    // 获取一次线性化、非阻塞、无副作用的状态快照；空 Handle 抛 std::logic_error（D-160）。
    [[nodiscard]] SchedulerStatus status() const;

    // 获取当前 Runtime 冻结的能力快照；空 Handle 抛 std::logic_error（D-162）。
    [[nodiscard]] SchedulerCapabilities capabilities() const;

    // 获取当前 Runtime 的只读指标快照（R-084 / R-085 / D-135 / D-136 / D-137）。
    [[nodiscard]] RuntimeMetricsSnapshot metrics_snapshot() const;

    // 请求平滑停机并同步等待 Drain Work Closure 排空完成（R-006 / R-007 / R-012 / R-019）。
    void shutdown();

    // 请求立即停机（R-016 / R-019）。
    void shutdown_now();

    // 提交单次执行任务图（R-070 / R-080 / D-104 / D-106 / D-107 / D-129）。
    GraphRun run(FrozenTaskGraph&& graph);
    GraphRun run(TaskOptions options, FrozenTaskGraph&& graph);

    // 阻塞/按策略提交任务（R-048 / R-058 / R-061 / R-062 / R-080 / R-102 / D-129）。
    template <typename F, typename... Args>
        requires (!std::is_same_v<std::remove_cvref_t<F>, TaskOptions>)
    auto submit(F&& f, Args&&... args) -> TaskHandle<typename detail::InvocationTraits<F, Args...>::ResultType> {
        return submit_impl(std::nullopt, std::forward<F>(f), std::forward<Args>(args)...);
    }

    template <typename F, typename... Args>
    auto submit(TaskOptions options, F&& f, Args&&... args) -> TaskHandle<typename detail::InvocationTraits<F, Args...>::ResultType> {
        return submit_impl(options, std::forward<F>(f), std::forward<Args>(args)...);
    }

    // 非阻塞尝试提交任务（R-061 / R-062 / R-080 / D-088 / D-129）。
    template <typename F, typename... Args>
        requires (!std::is_same_v<std::remove_cvref_t<F>, TaskOptions>)
    auto try_submit(F&& f, Args&&... args) -> SubmissionResult<typename detail::InvocationTraits<F, Args...>::ResultType> {
        return try_submit_impl(std::nullopt, std::forward<F>(f), std::forward<Args>(args)...);
    }

    template <typename F, typename... Args>
    auto try_submit(TaskOptions options, F&& f, Args&&... args) -> SubmissionResult<typename detail::InvocationTraits<F, Args...>::ResultType> {
        return try_submit_impl(options, std::forward<F>(f), std::forward<Args>(args)...);
    }

    // 异步提交 C++20 Coroutine Task（R-073 / R-080 / D-114 / D-115 / D-129）
    template <typename T>
    TaskHandle<T> spawn(Task<T>&& task) {
        return spawn_impl(std::nullopt, std::move(task));
    }

    template <typename T>
    TaskHandle<T> spawn(TaskOptions options, Task<T>&& task) {
        return spawn_impl(options, std::move(task));
    }

    // 非阻塞尝试提交 C++20 Coroutine Task（R-073 / R-080 / D-114 / D-115 / D-129）
    template <typename T>
    SubmissionResult<T> try_spawn(Task<T>&& task) {
        return try_spawn_impl(std::nullopt, std::move(task));
    }

    template <typename T>
    SubmissionResult<T> try_spawn(TaskOptions options, Task<T>&& task) {
        return try_spawn_impl(options, std::move(task));
    }

private:
    template <typename F, typename... Args>
    auto submit_impl(std::optional<TaskOptions> options, F&& f, Args&&... args)
        -> TaskHandle<typename detail::InvocationTraits<F, Args...>::ResultType> {
        using Traits = detail::InvocationTraits<F, Args...>;

        static_assert(Traits::is_valid,
            "Callable must be invocable as f(args...) or f(std::stop_token, args...)");
        static_assert(!Traits::returns_reference,
            "AstraScheduler tasks cannot return raw references (R-058 / D-074). Wrap in std::reference_wrapper if needed.");
        static_assert(Traits::is_move_constructible,
            "Task result type must be move-constructible (R-058 / D-075).");

        using ResultType = typename Traits::ResultType;

        if (!valid()) {
            throw std::logic_error("operating on empty/moved-from Scheduler");
        }

        detail::record_metrics_submission_attempt(runtime_id());

        const bool is_internal = (detail::current_worker_runtime_id() == runtime_id());
        const bool is_worker = (detail::current_worker_runtime_id() != RuntimeId{0});
        const bool can_block = !is_worker; // R-061 / D-085: 仅普通非 Worker 线程可 Block

        Priority resolved_priority = Priority::Normal;
        if (options.has_value()) {
            validate_priority(options->priority);
            resolved_priority = options->priority;
        } else if (is_internal) {
            resolved_priority = detail::current_executing_task_priority();
        } else {
            resolved_priority = Priority::Normal;
        }

        const std::optional<TaskDeadline> resolved_deadline =
            options.has_value() ? options->deadline : std::nullopt;

        const auto decision = acquire_admission(can_block, is_internal);
        if (decision == detail::AdmissionDecision::Stopping) {
            throw submission_rejected(SubmissionError::Stopping);
        }
        if (decision == detail::AdmissionDecision::Stopped) {
            throw submission_rejected(SubmissionError::Stopped);
        }
        if (decision == detail::AdmissionDecision::CapacityExhausted) {
            throw submission_rejected(SubmissionError::CapacityExhausted);
        }

        // 强异常安全事务：构造过程抛出异常则回滚 slot
        const TaskId tid = detail::allocate_task_id(runtime_id());
        std::shared_ptr<detail::TaskSharedState<ResultType>> state;
        std::unique_ptr<detail::TaskInvokerBase> invoker;

        try {
            state = std::make_shared<detail::TaskSharedState<ResultType>>(tid, resolved_priority, resolved_deadline);
            invoker = detail::make_task_invoker<Traits::is_ordinary_invocable, ResultType>(
                state, std::forward<F>(f), std::forward<Args>(args)...);
        } catch (...) {
            if (!is_internal) {
                rollback_external_slot();
            }
            throw;
        }

        post_task_invoker(std::move(invoker), !is_internal);
        return TaskHandle<ResultType>(std::move(state));
    }

    template <typename F, typename... Args>
    auto try_submit_impl(std::optional<TaskOptions> options, F&& f, Args&&... args)
        -> SubmissionResult<typename detail::InvocationTraits<F, Args...>::ResultType> {
        using Traits = detail::InvocationTraits<F, Args...>;

        static_assert(Traits::is_valid,
            "Callable must be invocable as f(args...) or f(std::stop_token, args...)");
        static_assert(!Traits::returns_reference,
            "AstraScheduler tasks cannot return raw references (R-058 / D-074). Wrap in std::reference_wrapper if needed.");
        static_assert(Traits::is_move_constructible,
            "Task result type must be move-constructible (R-058 / D-075).");

        using ResultType = typename Traits::ResultType;

        if (!valid()) {
            throw std::logic_error("operating on empty/moved-from Scheduler");
        }

        detail::record_metrics_submission_attempt(runtime_id());

        const bool is_internal = (detail::current_worker_runtime_id() == runtime_id());

        Priority resolved_priority = Priority::Normal;
        if (options.has_value()) {
            validate_priority(options->priority);
            resolved_priority = options->priority;
        } else if (is_internal) {
            resolved_priority = detail::current_executing_task_priority();
        } else {
            resolved_priority = Priority::Normal;
        }
        const std::optional<TaskDeadline> resolved_deadline =
            options.has_value() ? options->deadline : std::nullopt;

        // try_submit 永不等待 capacity（R-061 / D-088）
        const auto decision = acquire_admission(false /* no block */, is_internal);
        if (decision == detail::AdmissionDecision::Stopping) {
            return SubmissionResult<ResultType>(SubmissionError::Stopping);
        }
        if (decision == detail::AdmissionDecision::Stopped) {
            return SubmissionResult<ResultType>(SubmissionError::Stopped);
        }
        if (decision == detail::AdmissionDecision::CapacityExhausted) {
            return SubmissionResult<ResultType>(SubmissionError::CapacityExhausted);
        }

        // 强异常安全事务：构造过程抛出异常则回滚 slot
        const TaskId tid = detail::allocate_task_id(runtime_id());
        std::shared_ptr<detail::TaskSharedState<ResultType>> state;
        std::unique_ptr<detail::TaskInvokerBase> invoker;

        try {
            state = std::make_shared<detail::TaskSharedState<ResultType>>(tid, resolved_priority, resolved_deadline);
            invoker = detail::make_task_invoker<Traits::is_ordinary_invocable, ResultType>(
                state, std::forward<F>(f), std::forward<Args>(args)...);
        } catch (...) {
            if (!is_internal) {
                rollback_external_slot();
            }
            throw;
        }

        post_task_invoker(std::move(invoker), !is_internal);
        return SubmissionResult<ResultType>(TaskHandle<ResultType>(std::move(state)));
    }

    template <typename T>
    TaskHandle<T> spawn_impl(std::optional<TaskOptions> options, Task<T>&& task) {
        if (!valid()) {
            throw std::logic_error("operating on empty/moved-from Scheduler");
        }
        if (!task.valid()) {
            throw std::logic_error("cannot spawn empty/invalid Task");
        }

        detail::record_metrics_submission_attempt(runtime_id());

        const bool is_internal = (detail::current_worker_runtime_id() == runtime_id());
        const bool is_worker = (detail::current_worker_runtime_id() != RuntimeId{0});
        const bool can_block = !is_worker;

        Priority resolved_priority = Priority::Normal;
        if (options.has_value()) {
            validate_priority(options->priority);
            resolved_priority = options->priority;
        } else if (is_internal) {
            resolved_priority = detail::current_executing_task_priority();
        } else {
            resolved_priority = Priority::Normal;
        }
        const std::optional<TaskDeadline> resolved_deadline =
            options.has_value() ? options->deadline : std::nullopt;

        const auto decision = acquire_admission(can_block, is_internal);
        if (decision == detail::AdmissionDecision::Stopping) {
            throw submission_rejected(SubmissionError::Stopping);
        }
        if (decision == detail::AdmissionDecision::Stopped) {
            throw submission_rejected(SubmissionError::Stopped);
        }
        if (decision == detail::AdmissionDecision::CapacityExhausted) {
            throw submission_rejected(SubmissionError::CapacityExhausted);
        }

        const TaskId tid = detail::allocate_task_id(runtime_id());
        std::shared_ptr<detail::TaskSharedState<T>> state;
        std::unique_ptr<detail::TaskInvokerBase> invoker;

        try {
            state = std::make_shared<detail::TaskSharedState<T>>(tid, resolved_priority, resolved_deadline);
            auto rescheduler = [sched = *this](std::unique_ptr<detail::TaskInvokerBase> inv) {
                sched.post_task_invoker(std::move(inv), false /* is_external */);
            };
            state->set_rescheduler(std::move(rescheduler));
            state->set_timer_functions(
                [sched = *this](std::chrono::steady_clock::time_point wt, std::shared_ptr<AwaitHandshake> hs, std::function<void()> act) {
                    return sched.register_timer(wt, std::move(hs), std::move(act));
                },
                [sched = *this](std::uint64_t tid) {
                    sched.cancel_timer(tid);
                }
            );
            task.handle().promise().shared_state = state;
            auto coro_h = task.release_handle();
            invoker = std::make_unique<detail::CoroutineTaskInvokerModel<T>>(coro_h, state);
        } catch (...) {
            if (!is_internal) {
                rollback_external_slot();
            }
            throw;
        }

        post_task_invoker(std::move(invoker), !is_internal);
        return TaskHandle<T>(std::move(state));
    }

    template <typename T>
    SubmissionResult<T> try_spawn_impl(std::optional<TaskOptions> options, Task<T>&& task) {
        if (!valid()) {
            throw std::logic_error("operating on empty/moved-from Scheduler");
        }
        if (!task.valid()) {
            throw std::logic_error("cannot spawn empty/invalid Task");
        }

        detail::record_metrics_submission_attempt(runtime_id());

        const bool is_internal = (detail::current_worker_runtime_id() == runtime_id());

        Priority resolved_priority = Priority::Normal;
        if (options.has_value()) {
            validate_priority(options->priority);
            resolved_priority = options->priority;
        } else if (is_internal) {
            resolved_priority = detail::current_executing_task_priority();
        } else {
            resolved_priority = Priority::Normal;
        }
        const std::optional<TaskDeadline> resolved_deadline =
            options.has_value() ? options->deadline : std::nullopt;

        const auto decision = acquire_admission(false /* no block */, is_internal);
        if (decision == detail::AdmissionDecision::Stopping) {
            return SubmissionResult<T>(SubmissionError::Stopping);
        }
        if (decision == detail::AdmissionDecision::Stopped) {
            return SubmissionResult<T>(SubmissionError::Stopped);
        }
        if (decision == detail::AdmissionDecision::CapacityExhausted) {
            return SubmissionResult<T>(SubmissionError::CapacityExhausted);
        }

        const TaskId tid = detail::allocate_task_id(runtime_id());
        std::shared_ptr<detail::TaskSharedState<T>> state;
        std::unique_ptr<detail::TaskInvokerBase> invoker;

        try {
            state = std::make_shared<detail::TaskSharedState<T>>(tid, resolved_priority, resolved_deadline);
            auto rescheduler = [sched = *this](std::unique_ptr<detail::TaskInvokerBase> inv) {
                sched.post_task_invoker(std::move(inv), false /* is_external */);
            };
            state->set_rescheduler(std::move(rescheduler));
            state->set_timer_functions(
                [sched = *this](std::chrono::steady_clock::time_point wt, std::shared_ptr<AwaitHandshake> hs, std::function<void()> act) {
                    return sched.register_timer(wt, std::move(hs), std::move(act));
                },
                [sched = *this](std::uint64_t tid) {
                    sched.cancel_timer(tid);
                }
            );
            task.handle().promise().shared_state = state;
            auto coro_h = task.release_handle();
            invoker = std::make_unique<detail::CoroutineTaskInvokerModel<T>>(coro_h, state);
        } catch (...) {
            if (!is_internal) {
                rollback_external_slot();
            }
            throw;
        }

        post_task_invoker(std::move(invoker), !is_internal);
        return SubmissionResult<T>(TaskHandle<T>(std::move(state)));
    }

public:
    struct ASTRA_NO_EXPORT Impl;

private:
    std::shared_ptr<Impl> impl_;

    detail::AdmissionDecision acquire_admission(bool block, bool is_internal) const;
    void rollback_external_slot() const;
    void post_task_invoker(std::unique_ptr<detail::TaskInvokerBase> invoker, bool is_external) const;
    GraphRun run_impl(std::optional<TaskOptions> options, FrozenTaskGraph&& graph);
    std::uint64_t register_timer(std::chrono::steady_clock::time_point wake_time,
                                 std::shared_ptr<AwaitHandshake> handshake,
                                 std::function<void()> resume_action) const;
    void cancel_timer(std::uint64_t timer_id) const;

    friend void detail::run_test_task_on_worker(Scheduler&, std::function<void()>);
    friend std::size_t detail::global_injection_queue_size(const Scheduler&);
    friend std::size_t detail::external_pending_count(const Scheduler&);
    friend std::size_t detail::parked_workers_count(const Scheduler&);
    friend std::uint64_t detail::current_work_epoch(const Scheduler&);
    friend void detail::perform_caller_wait(const detail::TaskSharedStateBase&,
                                            std::optional<std::chrono::steady_clock::time_point>);
    friend void detail::perform_graph_caller_wait(const detail::GraphRunSharedState&,
                                                  std::optional<std::chrono::steady_clock::time_point>);
};

namespace detail {
void run_test_task_on_worker(Scheduler& s, std::function<void()> task);
std::size_t global_injection_queue_size(const Scheduler& s);
std::size_t external_pending_count(const Scheduler& s);
std::size_t parked_workers_count(const Scheduler& s);
std::uint64_t current_work_epoch(const Scheduler& s);
}

}  // namespace astra

#endif  // ASTRA_SCHEDULER_HPP

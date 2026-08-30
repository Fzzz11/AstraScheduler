#ifndef ASTRA_SCHEDULER_HPP
#define ASTRA_SCHEDULER_HPP

// ============================================================================
// Scheduler —— AstraScheduler 的使用入口。
//
// 【这是什么】
//   Scheduler 是一个可复制/可移动的"句柄"（Handle），多个副本共享同一个
//   后台运行时（Impl，实现在 src/scheduler.cpp）。你通过它 submit() 普通函数、
//   spawn() 协程、run() 任务图；每个任务返回 TaskHandle 用来取结果。
//
// 【整体架构（为什么有 Worker / Reaper 两类线程）】
//   - Worker 线程（数量由 options.worker_count 决定）：
//         负责执行你提交的任务。任务被偷取（work-stealing）或从全局队列
//         取出；等待某个任务结果时，worker 会顺手执行其他就绪任务（helping），
//         而不是空转阻塞。
//   - Reaper（回收者）线程（全进程恰好一个，不属于任何 Scheduler）：
//         当最后一个 Scheduler 句柄被销毁、而它的 worker 还没退完时，
//         运行时的"善后"（join worker、清理状态）被移交（handoff）给 Reaper。
//         这样句柄的析构永远是立即返回的，不会卡住调用者。
//
// 【关键生命周期规则】
//   1. 最后一个句柄销毁 = 请求优雅关停：不再接受新任务，等已有任务跑完，
//      然后所有 worker 退出。析构本身不会阻塞超过任务收尾所需时间。
//   2. shutdown() 可以提前显式关停；shutdown_now()/Immediate 模式则放弃
//      未开始的任务并尽快中断。
//   3. begin_finalization()（见 finalization.hpp）控制整个进程的最终收尾：
//      永久关闭所有 Scheduler 的注册，等全部 Runtime 退出。
//   4. 一个进程只应加载一份 AstraScheduler 实现（静态库或唯一共享库）；
//      多份拷贝各自为政，进程级保证失效。
//
// 【使用速览】
//   astra::SchedulerOptions opts;            // worker_count 等默认值即可用
//   astra::Scheduler sched(opts);
//   auto handle = sched.submit([] { return 42; });
//   int result = handle.get();               // 阻塞取结果
//
// 【可观测性】
//   sched.metrics_snapshot()      // 任务计数/延迟直方图（可选开关）
//   astra::process_metrics_snapshot()  // 进程级 Reaper/Finalization 诊断
//   TraceCollector（trace.hpp）   // 高频事件时间线（离线导出）
//
// 本文件只声明公共 API；实现位于 src/scheduler.cpp（class Impl 为内部细节，
// 对使用者不可见）。
//
// （可追溯性：AST-004 / R-098 / R-099 / R-100 / R-101 / D-155；平台仅 64-bit
//   Linux，经 export.hpp 编译期检查。）
// ============================================================================

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

namespace detail {
class AwaitHandshake;
class GraphRunSharedState;
class GraphExecution;
struct SchedulerTestAccess;

void perform_graph_caller_wait(const GraphRunSharedState&,
                               std::optional<std::chrono::steady_clock::time_point>);

enum class AdmissionDecision : std::uint8_t {
    Success,
    Stopping,
    Stopped,
    CapacityExhausted,
};

ASTRA_EXPORT RuntimeId current_worker_runtime_id() noexcept;
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
        std::shared_ptr<detail::TaskSharedState<ResultType>> state;
        std::unique_ptr<detail::TaskInvokerBase> invoker;

        try {
            const TaskId tid = allocate_task_id();
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
        std::shared_ptr<detail::TaskSharedState<ResultType>> state;
        std::unique_ptr<detail::TaskInvokerBase> invoker;

        try {
            const TaskId tid = allocate_task_id();
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

        std::shared_ptr<detail::TaskSharedState<T>> state;
        std::unique_ptr<detail::TaskInvokerBase> invoker;

        try {
            const TaskId tid = allocate_task_id();
            state = std::make_shared<detail::TaskSharedState<T>>(tid, resolved_priority, resolved_deadline);
            auto rescheduler = [sched = *this](std::unique_ptr<detail::TaskInvokerBase> inv) {
                sched.post_task_invoker(std::move(inv), false /* is_external */);
            };
            state->set_rescheduler(std::move(rescheduler));
            state->set_timer_functions(
                [sched = *this](std::chrono::steady_clock::time_point wt, std::shared_ptr<detail::AwaitHandshake> hs, std::function<void()> act) {
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

        std::shared_ptr<detail::TaskSharedState<T>> state;
        std::unique_ptr<detail::TaskInvokerBase> invoker;

        try {
            const TaskId tid = allocate_task_id();
            state = std::make_shared<detail::TaskSharedState<T>>(tid, resolved_priority, resolved_deadline);
            auto rescheduler = [sched = *this](std::unique_ptr<detail::TaskInvokerBase> inv) {
                sched.post_task_invoker(std::move(inv), false /* is_external */);
            };
            state->set_rescheduler(std::move(rescheduler));
            state->set_timer_functions(
                [sched = *this](std::chrono::steady_clock::time_point wt, std::shared_ptr<detail::AwaitHandshake> hs, std::function<void()> act) {
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
    TaskId allocate_task_id() const;
    void rollback_external_slot() const;
    void post_task_invoker(std::unique_ptr<detail::TaskInvokerBase> invoker, bool is_external) const;
    GraphRun run_impl(std::optional<TaskOptions> options, FrozenTaskGraph&& graph);
    std::uint64_t register_timer(std::chrono::steady_clock::time_point wake_time,
                                 std::shared_ptr<detail::AwaitHandshake> handshake,
                                 std::function<void()> resume_action) const;
    void cancel_timer(std::uint64_t timer_id) const;

    friend struct detail::SchedulerTestAccess;
    friend class detail::GraphExecution;
    friend void detail::perform_caller_wait(const detail::TaskSharedStateBase&,
                                            std::optional<std::chrono::steady_clock::time_point>);
    friend void detail::perform_graph_caller_wait(const detail::GraphRunSharedState&,
                                                  std::optional<std::chrono::steady_clock::time_point>);
};

}  // namespace astra

#endif  // ASTRA_SCHEDULER_HPP

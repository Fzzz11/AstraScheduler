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

// 调度器共享 Handle：副本关联同一 Runtime；最后一份销毁时请求 Graceful 关停。
class ASTRA_EXPORT Scheduler {
public:
    // 同步校验 options 并启动 Runtime。
    // worker_count 等为 0 或未知枚举抛 invalid_argument。
    // 进程已 begin_finalization 后抛 scheduler_creation_rejected（R-098 / D-155 / D-157）。
    explicit Scheduler(SchedulerOptions options = {});

    // 非最后一份副本立即返回。最后一份：非 Worker 线程同步 Graceful 关停；
    // 同 Runtime 的 Worker 走异步 Reaper handoff。析构不抛异常。
    ~Scheduler() noexcept;

    // 复制后共享同一 Runtime。
    Scheduler(const Scheduler&);
    Scheduler& operator=(const Scheduler&);

    // 移动后源 Handle 变为空（valid()==false）。
    Scheduler(Scheduler&&) noexcept;
    Scheduler& operator=(Scheduler&&) noexcept;

    // 是否关联 Runtime。空/moved-from 返回 false，不抛。
    [[nodiscard]] bool valid() const noexcept;

    // 本 Runtime 的逻辑 ID。空 Handle 返回无效 RuntimeId{0}，不抛。
    [[nodiscard]] RuntimeId runtime_id() const noexcept;

    // 非阻塞生命周期快照（state + shutdown_mode）。空 Handle 抛 logic_error（D-160）。
    [[nodiscard]] SchedulerStatus status() const;

    // 启动时冻结的 Local Deque 能力。空 Handle 抛 logic_error（D-162）。
    [[nodiscard]] SchedulerCapabilities capabilities() const;

    // 只读指标快照，不改变调度状态。空 Handle 抛 logic_error（R-084 / R-085）。
    [[nodiscard]] RuntimeMetricsSnapshot metrics_snapshot() const;

    // Graceful 关停：拒绝新提交，等待已接纳工作排空后返回。
    // 空 Handle 抛 logic_error；已 Stopped 立即成功且无副作用。
    // 同 Runtime 的 Worker 调用抛 logic_error（禁止自关停）（R-012 / R-019 / R-108）。
    void shutdown();

    // Immediate 关停：放弃未开始任务并尽快打断。空 Handle / 已 Stopped / 自关停规则同 shutdown()（R-016 / R-019）。
    void shutdown_now();

    // 提交已 freeze 的任务图并消耗 FrozenTaskGraph。
    // 空 Handle 抛 logic_error；admission 失败抛 submission_rejected。
    // Worker 线程不得按 Block 策略阻塞。可用 TaskOptions 覆盖图级 priority/deadline（R-070 / R-061）。
    GraphRun run(FrozenTaskGraph&& graph);
    GraphRun run(TaskOptions options, FrozenTaskGraph&& graph);

    // 提交可调用对象，成功则返回 TaskHandle。
    // 签名须为 f(args...) 或 f(stop_token, args...)；不得返回裸引用；结果须可移动。
    // 空 Handle 抛 logic_error。外部线程按 external_backpressure 阻塞或拒绝；
    // 任意 Worker 线程不得阻塞。Stopping/Stopped/CapacityExhausted 抛 submission_rejected。
    // 未给 TaskOptions 时：同 Runtime 内部提交继承当前任务优先级，否则 Normal（R-061 / R-062）。
    template <typename F, typename... Args>
        requires (!std::is_same_v<std::remove_cvref_t<F>, TaskOptions>)
    auto submit(F&& f, Args&&... args) -> TaskHandle<typename detail::InvocationTraits<F, Args...>::ResultType> {
        return submit_impl(std::nullopt, std::forward<F>(f), std::forward<Args>(args)...);
    }

    template <typename F, typename... Args>
    auto submit(TaskOptions options, F&& f, Args&&... args) -> TaskHandle<typename detail::InvocationTraits<F, Args...>::ResultType> {
        return submit_impl(options, std::forward<F>(f), std::forward<Args>(args)...);
    }

    // 非阻塞尝试提交。admission 失败返回 SubmissionError，不抛 submission_rejected。
    // 空 Handle 仍抛 logic_error。永不按 Block 等待容量（R-061 / D-088）。
    template <typename F, typename... Args>
        requires (!std::is_same_v<std::remove_cvref_t<F>, TaskOptions>)
    auto try_submit(F&& f, Args&&... args) -> SubmissionResult<typename detail::InvocationTraits<F, Args...>::ResultType> {
        return try_submit_impl(std::nullopt, std::forward<F>(f), std::forward<Args>(args)...);
    }

    template <typename F, typename... Args>
    auto try_submit(TaskOptions options, F&& f, Args&&... args) -> SubmissionResult<typename detail::InvocationTraits<F, Args...>::ResultType> {
        return try_submit_impl(options, std::forward<F>(f), std::forward<Args>(args)...);
    }

    // 提交 cold Task 协程（消耗 Task）。空 Scheduler 或无效 Task 抛 logic_error。
    // admission 与线程约束同 submit()（R-073 / R-061）。
    template <typename T>
    TaskHandle<T> spawn(Task<T>&& task) {
        return spawn_impl(std::nullopt, std::move(task));
    }

    template <typename T>
    TaskHandle<T> spawn(TaskOptions options, Task<T>&& task) {
        return spawn_impl(options, std::move(task));
    }

    // 非阻塞尝试 spawn。admission 失败返回 SubmissionError；空 Scheduler/Task 仍抛 logic_error（R-073 / D-088）。
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

#ifndef ASTRA_SCHEDULER_HPP
#define ASTRA_SCHEDULER_HPP

// ============================================================================
// Scheduler —— AstraScheduler 的使用入口。
//
// 【这是什么】
//   Scheduler 是一个可复制/可移动的"句柄"（Handle），多个副本共享同一个
//   后台运行时（Impl，实现在 src/runtime/scheduler.cpp）。你通过它 submit() 普通函数、
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
// 本文件只声明公共 API；实现位于 src/runtime/scheduler.cpp（class Impl 为内部细节，
// 对使用者不可见）。
//
// （可追溯性：AST-004 / R-098 / R-099 / R-100 / R-101 / D-155；平台仅 64-bit
//   Linux，经 export.hpp 编译期检查。）
// ============================================================================

#include <astra/capabilities.hpp>
#include <astra/coroutine.hpp>
#include <astra/error.hpp>
#include <astra/export.hpp>
#include <astra/graph.hpp>
#include <astra/id.hpp>
#include <astra/metrics.hpp>
#include <astra/scheduler_options.hpp>
#include <astra/status.hpp>
#include <astra/task_handle.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace astra {

class Scheduler;

namespace detail {
class AwaitHandshake;
class GraphRunSharedState;
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

ASTRA_EXPORT void post_task_on_impl(void* impl, std::unique_ptr<TaskInvokerBase> invoker,
                                    bool is_external);
ASTRA_EXPORT std::uint64_t register_timer_on_impl(void* impl,
                                                  std::chrono::steady_clock::time_point wake_time,
                                                  std::shared_ptr<AwaitHandshake> handshake,
                                                  std::function<void()> resume_action);
ASTRA_EXPORT void cancel_timer_on_impl(void* impl, std::uint64_t timer_id);
} // namespace detail

/**
 * @brief AstraScheduler 的共享 Runtime 句柄。
 * @note 复制只增加观察句柄；最后一份句柄销毁时请求 Graceful 关停。
 */
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

    /** @brief 返回非阻塞的生命周期与关停模式快照。 */
    [[nodiscard]] SchedulerStatus status() const;

    /** @brief 返回启动时冻结的实际本地队列能力。 */
    [[nodiscard]] SchedulerCapabilities capabilities() const;

    /** @brief 返回只读 Runtime 指标快照，不改变调度状态。 */
    [[nodiscard]] RuntimeMetricsSnapshot metrics_snapshot() const;

    /**
     * @brief 请求 Graceful 关停并等待已接纳工作排空。
     * @throws std::logic_error 空句柄或由 Runtime worker 发起时。
     */
    // 空 Handle 抛 logic_error；已 Stopped 立即成功且无副作用。
    // 同 Runtime 的 Worker 调用抛 logic_error（禁止自关停）（R-012 / R-019 / R-108）。
    void shutdown();

    /** @brief 请求 Immediate 关停，取消未开始任务并尽快打断运行中的任务。 */
    // 空 Handle / 已 Stopped / 自关停规则同 shutdown()（R-016 / R-019）。
    void shutdown_now();

    // 提交已 freeze 的任务图并消耗 FrozenTaskGraph。
    // 空 Handle 抛 logic_error；admission 失败抛 submission_rejected。
    // Worker 线程不得按 Block 策略阻塞。可用 TaskOptions 覆盖图级 priority/deadline（R-070 /
    // R-061）。
    GraphRun run(FrozenTaskGraph&& graph);
    GraphRun run(TaskOptions options, FrozenTaskGraph&& graph);

    /**
     * @brief 提交可调用对象并返回共享结果句柄。
     * @tparam F 任务函数类型。
     * @tparam Args 传给任务函数的参数类型。
     * @param f 任务函数，须可按给定参数调用。
     * @param args 传给任务函数的参数。
     */
    // 提交可调用对象，成功则返回 TaskHandle。
    // 签名须为 f(args...) 或 f(stop_token, args...)；不得返回裸引用；结果须可移动。
    // 空 Handle 抛 logic_error。外部线程按 external_backpressure 阻塞或拒绝；
    // 任意 Worker 线程不得阻塞。Stopping/Stopped/CapacityExhausted 抛 submission_rejected。
    // 未给 TaskOptions 时：同 Runtime 内部提交继承当前任务优先级，否则 Normal（R-061 / R-062）。
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> TaskHandle<typename detail::InvocationTraits<F, Args...>::ResultType>
        requires(!std::is_same_v<std::remove_cvref_t<F>, TaskOptions>)
    {
        return submit_impl(std::nullopt, std::forward<F>(f), std::forward<Args>(args)...);
    }

    /**
     * @brief 带 TaskOptions 提交可调用对象并返回共享结果句柄。
     * @tparam F 任务函数类型。
     * @tparam Args 传给任务函数的参数类型。
     * @param options 任务选项（优先级、截止时间）。
     * @param f 任务函数，须可按给定参数调用。
     * @param args 传给任务函数的参数。
     */
    template <typename F, typename... Args>
    auto submit(TaskOptions options, F&& f, Args&&... args)
        -> TaskHandle<typename detail::InvocationTraits<F, Args...>::ResultType> {
        return submit_impl(options, std::forward<F>(f), std::forward<Args>(args)...);
    }

    /**
     * @brief 非阻塞尝试提交可调用对象。
     * @tparam F 任务函数类型。
     * @tparam Args 传给任务函数的参数类型。
     * @param f 任务函数，须可按给定参数调用。
     * @param args 传给任务函数的参数。
     * @return 成功的 TaskHandle 或机器可读的拒绝原因。
     */
    // 非阻塞尝试提交。admission 失败返回 SubmissionError，不抛 submission_rejected。
    // 空 Handle 仍抛 logic_error。永不按 Block 等待容量（R-061 / D-088）。
    template <typename F, typename... Args>
    auto try_submit(F&& f, Args&&... args)
        -> SubmissionResult<typename detail::InvocationTraits<F, Args...>::ResultType>
        requires(!std::is_same_v<std::remove_cvref_t<F>, TaskOptions>)
    {
        return try_submit_impl(std::nullopt, std::forward<F>(f), std::forward<Args>(args)...);
    }

    /**
     * @brief 带 TaskOptions 非阻塞尝试提交可调用对象。
     * @tparam F 任务函数类型。
     * @tparam Args 传给任务函数的参数类型。
     * @param options 任务选项（优先级、截止时间）。
     * @param f 任务函数，须可按给定参数调用。
     * @param args 传给任务函数的参数。
     * @return 成功的 TaskHandle 或机器可读的拒绝原因。
     */
    template <typename F, typename... Args>
    auto try_submit(TaskOptions options, F&& f, Args&&... args)
        -> SubmissionResult<typename detail::InvocationTraits<F, Args...>::ResultType> {
        return try_submit_impl(options, std::forward<F>(f), std::forward<Args>(args)...);
    }

    /**
     * @brief 提交 cold Task 协程并转移其协程帧所有权。
     * @tparam T 协程结果类型。
     * @param task 要提交的 cold 协程。
     */
    // 提交 cold Task 协程（消耗 Task）。空 Scheduler 或无效 Task 抛 logic_error。
    // admission 与线程约束同 submit()（R-073 / R-061）。
    template <typename T> TaskHandle<T> spawn(Task<T>&& task) {
        return spawn_impl(std::nullopt, std::move(task));
    }

    /**
     * @brief 带 TaskOptions 提交 cold Task 协程并转移其协程帧所有权。
     * @tparam T 协程结果类型。
     * @param options 任务选项（优先级、截止时间）。
     * @param task 要提交的 cold 协程。
     */
    template <typename T> TaskHandle<T> spawn(TaskOptions options, Task<T>&& task) {
        return spawn_impl(options, std::move(task));
    }

    /**
     * @brief 非阻塞尝试提交 cold Task 协程。
     * @tparam T 协程结果类型。
     * @param task 要提交的 cold 协程。
     * @return 成功的 TaskHandle 或机器可读的拒绝原因。
     */
    // 非阻塞尝试 spawn。admission 失败返回 SubmissionError；空 Scheduler/Task 仍抛
    // logic_error（R-073 / D-088）。
    template <typename T> SubmissionResult<T> try_spawn(Task<T>&& task) {
        return try_spawn_impl(std::nullopt, std::move(task));
    }

    /**
     * @brief 带 TaskOptions 非阻塞尝试提交 cold Task 协程。
     * @tparam T 协程结果类型。
     * @param options 任务选项（优先级、截止时间）。
     * @param task 要提交的 cold 协程。
     * @return 成功的 TaskHandle 或机器可读的拒绝原因。
     */
    template <typename T> SubmissionResult<T> try_spawn(TaskOptions options, Task<T>&& task) {
        return try_spawn_impl(options, std::move(task));
    }

  private:
    struct SubmissionContext {
        bool is_internal{false};
        Priority priority{Priority::Normal};
        std::optional<TaskDeadline> deadline{std::nullopt};
        detail::AdmissionDecision decision{detail::AdmissionDecision::Success};
    };

    SubmissionContext prepare_submission(std::optional<TaskOptions> options,
                                         bool nonblocking) const;
    static SubmissionError submission_error_for(detail::AdmissionDecision decision);

    template <typename T, typename InvokerFactory>
    TaskHandle<T> publish_submission(const SubmissionContext& context,
                                     InvokerFactory&& make_invoker) {
        using Cell = typename TaskHandle<T>::ResultCell;
        std::shared_ptr<Cell> state;
        try {
            const TaskId task_id = allocate_task_id();
            state = std::make_shared<Cell>(task_id, context.priority, context.deadline);
            auto invoker = std::forward<InvokerFactory>(make_invoker)(state);
            post_task_invoker(std::move(invoker), !context.is_internal);
            return TaskHandle<T>(std::move(state));
        } catch (...) {
            if (!context.is_internal) {
                rollback_external_slot();
            }
            throw;
        }
    }

    template <typename T>
    std::unique_ptr<detail::TaskInvokerBase>
    make_spawn_invoker(Task<T>& task, std::shared_ptr<typename TaskHandle<T>::ResultCell> state) {
        void* const impl = impl_.get();
        auto rescheduler = [impl](std::unique_ptr<detail::TaskInvokerBase> inv) {
            detail::post_task_on_impl(impl, std::move(inv), false);
        };
        state->set_rescheduler(std::move(rescheduler));
        state->set_timer_functions(
            [impl](std::chrono::steady_clock::time_point wt,
                   std::shared_ptr<detail::AwaitHandshake> hs, std::function<void()> act) {
                return detail::register_timer_on_impl(impl, wt, std::move(hs), std::move(act));
            },
            [impl](std::uint64_t timer_id) { detail::cancel_timer_on_impl(impl, timer_id); });
        task.handle().promise().shared_state = state;
        auto coroutine_handle = task.release_handle();
        return detail::wrap_submitted_invoker(
            std::make_unique<detail::CoroutineTaskInvokerModel<T>>(coroutine_handle, state),
            state->protocol_token());
    }

    template <typename F, typename... Args>
    auto submit_impl(std::optional<TaskOptions> options, F&& f, Args&&... args)
        -> TaskHandle<typename detail::InvocationTraits<F, Args...>::ResultType> {
        using Traits = detail::InvocationTraits<F, Args...>;
        static_assert(Traits::is_valid,
                      "Callable must be invocable as f(args...) or f(std::stop_token, args...)");
        static_assert(!Traits::returns_reference,
                      "AstraScheduler tasks cannot return raw references (R-058 / D-074). Wrap in "
                      "std::reference_wrapper if needed.");
        static_assert(Traits::is_move_constructible,
                      "Task result type must be move-constructible (R-058 / D-075).");

        using ResultType = typename Traits::ResultType;
        const auto context = prepare_submission(std::move(options), false);
        if (context.decision != detail::AdmissionDecision::Success) {
            throw submission_rejected(submission_error_for(context.decision));
        }
        return publish_submission<ResultType>(
            context,
            [&f, &args...](std::shared_ptr<typename TaskHandle<ResultType>::ResultCell> state) {
                return detail::wrap_submitted_invoker(
                    TaskHandle<ResultType>::template make_invoker<Traits::is_ordinary_invocable>(
                        state, std::forward<F>(f), std::forward<Args>(args)...),
                    state->protocol_token());
            });
    }

    template <typename F, typename... Args>
    auto try_submit_impl(std::optional<TaskOptions> options, F&& f, Args&&... args)
        -> SubmissionResult<typename detail::InvocationTraits<F, Args...>::ResultType> {
        using Traits = detail::InvocationTraits<F, Args...>;
        static_assert(Traits::is_valid,
                      "Callable must be invocable as f(args...) or f(std::stop_token, args...)");
        static_assert(!Traits::returns_reference,
                      "AstraScheduler tasks cannot return raw references (R-058 / D-074). Wrap in "
                      "std::reference_wrapper if needed.");
        static_assert(Traits::is_move_constructible,
                      "Task result type must be move-constructible (R-058 / D-075).");

        using ResultType = typename Traits::ResultType;
        const auto context = prepare_submission(std::move(options), true);
        if (context.decision != detail::AdmissionDecision::Success) {
            return SubmissionResult<ResultType>(submission_error_for(context.decision));
        }
        return SubmissionResult<ResultType>(publish_submission<ResultType>(
            context,
            [&f, &args...](std::shared_ptr<typename TaskHandle<ResultType>::ResultCell> state) {
                return detail::wrap_submitted_invoker(
                    TaskHandle<ResultType>::template make_invoker<Traits::is_ordinary_invocable>(
                        state, std::forward<F>(f), std::forward<Args>(args)...),
                    state->protocol_token());
            }));
    }

    template <typename T>
    TaskHandle<T> spawn_impl(std::optional<TaskOptions> options, Task<T>&& task) {
        if (!task.valid()) {
            throw std::logic_error("cannot spawn empty/invalid Task");
        }
        const auto context = prepare_submission(std::move(options), false);
        if (context.decision != detail::AdmissionDecision::Success) {
            throw submission_rejected(submission_error_for(context.decision));
        }
        return publish_submission<T>(
            context, [this, &task](std::shared_ptr<typename TaskHandle<T>::ResultCell> state) {
                return make_spawn_invoker(task, std::move(state));
            });
    }

    template <typename T>
    SubmissionResult<T> try_spawn_impl(std::optional<TaskOptions> options, Task<T>&& task) {
        if (!task.valid()) {
            throw std::logic_error("cannot spawn empty/invalid Task");
        }
        const auto context = prepare_submission(std::move(options), true);
        if (context.decision != detail::AdmissionDecision::Success) {
            return SubmissionResult<T>(submission_error_for(context.decision));
        }
        return SubmissionResult<T>(publish_submission<T>(
            context, [this, &task](std::shared_ptr<typename TaskHandle<T>::ResultCell> state) {
                return make_spawn_invoker(task, std::move(state));
            }));
    }

  public:
    struct ASTRA_NO_EXPORT Impl;

  private:
    std::shared_ptr<Impl> impl_;

    detail::AdmissionDecision acquire_admission(bool block, bool is_internal) const;
    TaskId allocate_task_id() const;
    void rollback_external_slot() const;
    void post_task_invoker(std::unique_ptr<detail::TaskInvokerBase> invoker,
                           bool is_external) const;
    ASTRA_NO_EXPORT GraphRun run_impl(std::optional<TaskOptions> options, FrozenTaskGraph&& graph);
    std::uint64_t register_timer(std::chrono::steady_clock::time_point wake_time,
                                 std::shared_ptr<detail::AwaitHandshake> handshake,
                                 std::function<void()> resume_action) const;
    void cancel_timer(std::uint64_t timer_id) const;

    friend struct detail::SchedulerTestAccess;
    friend void detail::perform_caller_wait(const detail::TaskControlBlock&,
                                            std::optional<std::chrono::steady_clock::time_point>);
    friend void
    detail::perform_graph_caller_wait(const detail::GraphRunSharedState&,
                                      std::optional<std::chrono::steady_clock::time_point>);
};

} // namespace astra

#endif // ASTRA_SCHEDULER_HPP

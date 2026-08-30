#ifndef ASTRA_TASK_HANDLE_HPP
#define ASTRA_TASK_HANDLE_HPP

#include <astra/error.hpp>
#include <astra/export.hpp>
#include <astra/id.hpp>
#include <astra/status.hpp>
#include <astra/task_options.hpp>

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

// ============================================================================
// TaskHandle / TaskSharedState —— 任务结果的生命周期与线程间交接。
//
// 【这是什么】
//   submit()/spawn() 返回 TaskHandle<T>：它是"任务结果"的共享句柄，
//   内部指向一块堆上的共享状态（TaskSharedState<T>）。任务函数在 worker
//   线程执行完毕后把返回值（或异常）写进共享状态；你可以：
//     - get()      阻塞取值（异常会在调用方重新抛出）
//     - wait()/wait_for()   只等完成不取值
//     - request_cancel()    请求协作式取消
//     - co_await handle     在另一个 Astra 协程里等待它
//
// 【为什么是"共享状态"而不是直接存结果】
//   提交任务与取结果是两个不同线程的两个独立动作，中间可能隔着
//   排队、执行、被偷取、挂起恢复。共享状态就是一个带锁与条件变量的
//   小信箱：生产者（worker）写入终态，消费者（你）等待并读取；
//   两端各自持有 shared_ptr，谁后结束谁负责释放，不存在悬垂。
//
// 【TaskHandle 的副本语义】
//   TaskHandle 可复制：多个副本指向同一共享状态，最后一个销毁时
//   触发"未被观察的失败"诊断——异常任务若从未被 get/await 过，
//   会在 Metrics 里计一次 unobserved_failures（不崩溃、不吞异常，
//   只让你知道有失败被忽略了）。
//
// 【取消模型（协作式）】
//   request_cancel() 只是发出停止信号（stop_token）；任务体通过
//   stop_token 轮询，或在挂起点（sleep/await）被动收到 task_cancelled
//   异常。绝不强制杀死正在运行的任务。
//
// 【本文件里的实现细节】
//   TaskSharedStateBase 的方法体在 src/task_shared_state.cpp（封装性：
//   实现不放在公共头文件）；文件中的 record_* 声明是"指标埋点 seam"——
//   worker/等待路径的内联代码通过它们上报计数，实现在 src/scheduler.cpp，
//   MetricsLevel::Off 时全部为空操作。
// ============================================================================

namespace astra {

class Scheduler;

// 检查并抛出取消异常辅助函数（R-054 / D-060）。
inline void throw_if_stop_requested(std::stop_token token) {
    if (token.stop_requested()) {
        throw task_cancelled{};
    }
}

namespace detail {

class AwaitHandshake;

class ASTRA_EXPORT TaskSharedStateBase;

ASTRA_EXPORT void perform_caller_wait(
    const TaskSharedStateBase& target,
    std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);

struct TaskInvokerBase {
    virtual ~TaskInvokerBase() = default;
    virtual void execute() = 0;
    virtual void cancel_pre_start() noexcept = 0;
    [[nodiscard]] virtual bool is_resume_segment() const noexcept { return false; }
    [[nodiscard]] virtual bool is_coroutine_node() const noexcept { return false; }
    [[nodiscard]] virtual Priority priority() const noexcept { return Priority::Normal; }
    [[nodiscard]] virtual std::optional<TaskDeadline> deadline() const noexcept { return std::nullopt; }
};

ASTRA_EXPORT TaskId current_executing_task_id() noexcept;
ASTRA_EXPORT Priority current_executing_task_priority() noexcept;

template <typename T>
struct TaskHandleAwaiter;

struct ASTRA_EXPORT TaskExecutionContextGuard {
    TaskId prev_id;
    Priority prev_priority;
    explicit TaskExecutionContextGuard(TaskId new_id, Priority new_priority = Priority::Normal) noexcept;
    ~TaskExecutionContextGuard() noexcept;
};

ASTRA_EXPORT void record_metrics_submission_attempt(RuntimeId id) noexcept;
ASTRA_EXPORT void record_metrics_first_start(TaskId id, std::optional<DeadlineDisposition> dl_disp) noexcept;
ASTRA_EXPORT void record_metrics_succeeded(TaskId id) noexcept;
ASTRA_EXPORT void record_metrics_failed(TaskId id) noexcept;
ASTRA_EXPORT void record_metrics_cancelled_cooperative(TaskId id) noexcept;
ASTRA_EXPORT void record_metrics_cancelled_before_start(TaskId id, bool has_deadline) noexcept;
ASTRA_EXPORT void record_metrics_unobserved_failure(TaskId id) noexcept;
ASTRA_EXPORT void record_metrics_suspended(TaskId id) noexcept;
ASTRA_EXPORT void record_metrics_resumed(TaskId id) noexcept;
ASTRA_EXPORT void record_metrics_resume_segment(TaskId id) noexcept;
ASTRA_EXPORT void record_metrics_explicit_yield() noexcept;

// Wait/Await 诊断入口（AST-048 / R-096 / D-149），实现于 scheduler.cpp。
ASTRA_EXPORT void record_wait_call(TaskId target, bool timed_out) noexcept;
ASTRA_EXPORT void record_self_wait_rejection(TaskId target) noexcept;
ASTRA_EXPORT void record_await_registration(TaskId source, TaskId target) noexcept;
ASTRA_EXPORT void record_await_triggered(TaskId source, TaskId target, bool cancelled) noexcept;
ASTRA_EXPORT void record_await_resumed(TaskId source, TaskId target, std::uint64_t duration_ns) noexcept;

ASTRA_EXPORT void record_metrics_ready_queue_wait(TaskId id, std::uint64_t duration_ns) noexcept;
ASTRA_EXPORT void record_metrics_execution_segment(TaskId id, std::uint64_t duration_ns) noexcept;
ASTRA_EXPORT void record_metrics_task_wall_time(TaskId id, std::uint64_t duration_ns) noexcept;
ASTRA_EXPORT void record_metrics_blocking_admission_wait(RuntimeId id, std::uint64_t duration_ns) noexcept;
ASTRA_EXPORT void record_metrics_timer_wake_lateness(RuntimeId id, std::uint64_t duration_ns) noexcept;
ASTRA_EXPORT void record_metrics_deadline_start_lateness(TaskId id, std::uint64_t duration_ns) noexcept;
ASTRA_EXPORT void record_metrics_worker_park_duration(RuntimeId id, std::uint64_t duration_ns) noexcept;
ASTRA_EXPORT void record_metrics_runtime_join_latency(RuntimeId id, std::uint64_t duration_ns) noexcept;

class ASTRA_EXPORT TaskSharedStateBase {
public:
    explicit TaskSharedStateBase(
        TaskId id,
        Priority priority = Priority::Normal,
        std::optional<TaskDeadline> deadline = std::nullopt);
    virtual ~TaskSharedStateBase();

    TaskSharedStateBase(const TaskSharedStateBase&) = delete;
    TaskSharedStateBase& operator=(const TaskSharedStateBase&) = delete;

    // AST-056：resume 所有权代际。awaiter 的 await_suspend 在 requeue resume
    // invoker 前递增；发起 resume 的 invoker 在 resume() 返回后比对代际——
    // 代际已变化则帧所有权已移交 resume invoker，不得再触碰/销毁帧。
    void mark_resume_handoff() noexcept {
        resume_handoff_seq_.fetch_add(1, std::memory_order_acq_rel);
    }
    [[nodiscard]] std::uint64_t resume_handoff_seq() const noexcept {
        return resume_handoff_seq_.load(std::memory_order_acquire);
    }

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

    [[nodiscard]] std::stop_token stop_token() noexcept {
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

    using ReschedulerFunc = std::function<void(std::unique_ptr<TaskInvokerBase>)>;
    using TimerRegistrar = std::function<std::uint64_t(std::chrono::steady_clock::time_point, std::shared_ptr<AwaitHandshake>, std::function<void()>)>;
    using TimerCanceller = std::function<void(std::uint64_t)>;

    void set_rescheduler(ReschedulerFunc rescheduler);
    [[nodiscard]] ReschedulerFunc get_rescheduler() const;
    void set_timer_functions(TimerRegistrar reg, TimerCanceller cancel);
    [[nodiscard]] TimerRegistrar get_timer_registrar() const;
    [[nodiscard]] TimerCanceller get_timer_canceller() const;

    void transition_to_suspended() noexcept;
    void transition_to_running() noexcept;
    void add_completion_callback(std::function<void()> cb);
    void record_terminal_wall_time() noexcept;
    void request_cancel() noexcept;
    bool try_start() noexcept;

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

    void wait() const {
        perform_caller_wait(*this);
    }

    template <typename Rep, typename Period>
    WaitResult wait_for(const std::chrono::duration<Rep, Period>& duration) const {
        if (duration <= std::chrono::duration<Rep, Period>::zero()) {
            const bool completed = is_completed();
            // R-096 / D-149：即时等待也计 call 与零/最小 bucket；TimedOut 计超时。
            record_wait_call(id_, !completed);
            return completed ? WaitResult::Completed : WaitResult::TimedOut;
        }
        const auto deadline = std::chrono::steady_clock::now() + duration;
        perform_caller_wait(*this, deadline);
        return is_completed() ? WaitResult::Completed : WaitResult::TimedOut;
    }

    [[nodiscard]] bool is_completed() const noexcept {
        const auto s = state_.load(std::memory_order_acquire);
        return s == TaskState::Succeeded || s == TaskState::Failed || s == TaskState::Cancelled;
    }

    [[nodiscard]] bool is_completed_locked() const noexcept {
        const auto s = state_.load(std::memory_order_relaxed);
        return s == TaskState::Succeeded || s == TaskState::Failed || s == TaskState::Cancelled;
    }

protected:
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
    std::vector<std::function<void()>> completion_callbacks_;
    ReschedulerFunc rescheduler_;
    TimerRegistrar timer_registrar_{nullptr};
    TimerCanceller timer_canceller_{nullptr};
    std::atomic<std::int64_t> admitted_at_ns_{0};
    std::atomic<std::int64_t> ready_published_at_ns_{0};
};

template <typename T>
class TaskSharedState : public TaskSharedStateBase {
public:
    explicit TaskSharedState(
        TaskId id,
        Priority priority = Priority::Normal,
        std::optional<TaskDeadline> deadline = std::nullopt)
        : TaskSharedStateBase(id, priority, deadline) {}

    void set_value(T val) {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            value_.emplace(std::move(val));
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

    const T& get() const {
        perform_caller_wait(*this);
        const auto s = state_.load(std::memory_order_acquire);
        if (s == TaskState::Failed) {
            observed_.store(true, std::memory_order_relaxed);
            std::rethrow_exception(exception_);
        }
        if (s == TaskState::Cancelled) {
            throw task_cancelled{};
        }
        return *value_;
    }

private:
    std::optional<T> value_;
};

template <>
class TaskSharedState<void> : public TaskSharedStateBase {
public:
    explicit TaskSharedState(
        TaskId id,
        Priority priority = Priority::Normal,
        std::optional<TaskDeadline> deadline = std::nullopt)
        : TaskSharedStateBase(id, priority, deadline) {}

    void set_value() {
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

    void get() const {
        perform_caller_wait(*this);
        const auto s = state_.load(std::memory_order_acquire);
        if (s == TaskState::Failed) {
            observed_.store(true, std::memory_order_relaxed);
            std::rethrow_exception(exception_);
        }
        if (s == TaskState::Cancelled) {
            throw task_cancelled{};
        }
    }
};

template <bool Ordinary, bool StopAware, typename DF, typename... DArgs>
struct ResultDeducer {
    using type = void;
};

template <typename DF, typename... DArgs>
struct ResultDeducer<true, false, DF, DArgs...> {
    using type = std::invoke_result_t<DF&&, DArgs&&...>;
};

template <typename DF, typename... DArgs>
struct ResultDeducer<true, true, DF, DArgs...> {
    using type = std::invoke_result_t<DF&&, DArgs&&...>;
};

template <typename DF, typename... DArgs>
struct ResultDeducer<false, true, DF, DArgs...> {
    using type = std::invoke_result_t<DF&&, std::stop_token, DArgs&&...>;
};

// 任务调用与类型特征推导（R-058 / R-102 / D-059 / D-074 / D-075 / D-165）
template <typename F, typename... Args>
struct InvocationTraits {
    using DF = std::decay_t<F>;

    static constexpr bool is_ordinary_invocable =
        std::is_invocable_v<DF&&, std::decay_t<Args>&&...>;

    static constexpr bool is_stop_aware_invocable =
        std::is_invocable_v<DF&&, std::stop_token, std::decay_t<Args>&&...>;

    static constexpr bool is_valid =
        is_ordinary_invocable || is_stop_aware_invocable;

    using RawResult = typename ResultDeducer<
        is_ordinary_invocable,
        is_stop_aware_invocable,
        DF,
        std::decay_t<Args>...>::type;

    static constexpr bool returns_reference = std::is_reference_v<RawResult>;

    using ResultType = std::conditional_t<
        std::is_void_v<RawResult>,
        void,
        std::remove_cv_t<RawResult>>;

    static constexpr bool is_move_constructible =
        std::is_void_v<ResultType> || std::is_move_constructible_v<ResultType>;
};

template <bool Ordinary, typename ResultType, typename F, typename... Args>
class TaskInvokerModel : public TaskInvokerBase {
public:
    template <typename UF, typename... UArgs>
    TaskInvokerModel(std::shared_ptr<TaskSharedState<ResultType>> state, UF&& f, UArgs&&... args)
        : state_(std::move(state)),
          fn_(std::forward<UF>(f)),
          args_(std::forward<UArgs>(args)...) {}

    void execute() override {
        // R-053 / D-052: 首次 start 竞争
        if (!state_->try_start()) {
            // Cancel 胜出：用户 Callable 一次也不执行
            return;
        }

        constexpr std::size_t tuple_size = std::tuple_size_v<decltype(args_)>;
        invoke_impl(std::make_index_sequence<tuple_size>{});
    }

    void cancel_pre_start() noexcept override {
        if (state_) {
            state_->request_cancel();
        }
    }

    [[nodiscard]] Priority priority() const noexcept override {
        return state_ ? state_->priority() : Priority::Normal;
    }

    [[nodiscard]] std::optional<TaskDeadline> deadline() const noexcept override {
        return state_ ? state_->deadline() : std::nullopt;
    }

private:
    template <std::size_t... Is>
    void invoke_impl(std::index_sequence<Is...>) {
        TaskExecutionContextGuard context_guard(state_->id(), state_->priority());
        const auto t_start = std::chrono::steady_clock::now();
        try {
            if constexpr (Ordinary) {
                if constexpr (std::is_void_v<ResultType>) {
                    std::invoke(std::move(fn_), std::get<Is>(std::move(args_))...);
                    const auto t_end = std::chrono::steady_clock::now();
                    record_metrics_execution_segment(state_->id(), std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
                    state_->set_value();
                } else {
                    auto res = std::invoke(std::move(fn_), std::get<Is>(std::move(args_))...);
                    const auto t_end = std::chrono::steady_clock::now();
                    record_metrics_execution_segment(state_->id(), std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
                    state_->set_value(std::move(res));
                }
            } else {
                if constexpr (std::is_void_v<ResultType>) {
                    std::invoke(std::move(fn_), state_->stop_token(), std::get<Is>(std::move(args_))...);
                    const auto t_end = std::chrono::steady_clock::now();
                    record_metrics_execution_segment(state_->id(), std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
                    state_->set_value();
                } else {
                    auto res = std::invoke(std::move(fn_), state_->stop_token(), std::get<Is>(std::move(args_))...);
                    const auto t_end = std::chrono::steady_clock::now();
                    record_metrics_execution_segment(state_->id(), std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
                    state_->set_value(std::move(res));
                }
            }
        } catch (const task_cancelled&) {
            const auto t_end = std::chrono::steady_clock::now();
            record_metrics_execution_segment(state_->id(), std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
            state_->set_cancelled();
        } catch (...) {
            const auto t_end = std::chrono::steady_clock::now();
            record_metrics_execution_segment(state_->id(), std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
            state_->set_exception(std::current_exception());
        }
    }

    std::shared_ptr<TaskSharedState<ResultType>> state_;
    std::decay_t<F> fn_;
    std::tuple<std::decay_t<Args>...> args_;
};

template <bool Ordinary, typename ResultType, typename F, typename... Args>
std::unique_ptr<TaskInvokerBase> make_task_invoker(
    std::shared_ptr<TaskSharedState<ResultType>> state,
    F&& f,
    Args&&... args) {
    return std::make_unique<TaskInvokerModel<Ordinary, ResultType, F, Args...>>(
        std::move(state), std::forward<F>(f), std::forward<Args>(args)...);
}

}  // namespace detail

// TaskHandle<T> — 共享任务结果句柄（R-048 / R-049 / R-050 / R-051 / R-052 / R-053 / R-054 / R-055 / R-056 / R-057 / R-058 / D-041 / D-042 / D-076）。
template <typename T>
class TaskHandle {
public:
    static_assert(!std::is_reference_v<T>, "TaskHandle does not support raw reference types (R-058 / D-074)");
    static_assert(std::is_move_constructible_v<T>, "TaskHandle result type must be move-constructible (R-058 / D-075)");

    TaskHandle() noexcept = default;
    ~TaskHandle() = default;

    TaskHandle(const TaskHandle&) = default;
    TaskHandle& operator=(const TaskHandle&) = default;

    TaskHandle(TaskHandle&&) noexcept = default;
    TaskHandle& operator=(TaskHandle&&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(state_);
    }

    [[nodiscard]] TaskId task_id() const {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return state_->id();
    }

    [[nodiscard]] Priority priority() const {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return state_->priority();
    }

    [[nodiscard]] std::optional<TaskDeadline> deadline() const {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return state_->deadline();
    }

    [[nodiscard]] DeadlineDisposition deadline_disposition() const {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return state_->deadline_disposition();
    }

    [[nodiscard]] TaskState state() const {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return state_->state();
    }

    // R-051 / D-076: 仅允许左值 Handle 显式调用（get() const &）返回 const T&
    // 临时对象 / rvalue 在编译期被 delete 拒绝以防止悬垂引用
    const T& get() const & {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return state_->get();
    }

    void get() const && = delete;

    // R-055 / D-061: 同步等待完成
    void wait() const {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        state_->wait();
    }

    // R-056 / D-063: 有界等待
    template <typename Rep, typename Period>
    [[nodiscard]] WaitResult wait_for(const std::chrono::duration<Rep, Period>& duration) const {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return state_->wait_for(duration);
    }

    // R-053 / R-057: 请求取消（空 Handle 为 no-op）
    void request_cancel() const noexcept {
        if (state_) {
            state_->request_cancel();
        }
    }

    // R-076 / D-120: co_await 左值 TaskHandle（rvalue deleted）
    [[nodiscard]] detail::TaskHandleAwaiter<T> operator co_await() const &;
    void operator co_await() const && = delete;
    void operator co_await() && = delete;

private:
    friend class Scheduler;
    friend struct detail::TaskHandleAwaiter<T>;

    explicit TaskHandle(std::shared_ptr<detail::TaskSharedState<T>> state) noexcept
        : state_(std::move(state)) {}

    [[nodiscard]] std::shared_ptr<detail::TaskSharedState<T>> shared_state_internal() const noexcept {
        return state_;
    }

    std::shared_ptr<detail::TaskSharedState<T>> state_;
};

// TaskHandle<void> 特化（R-048 / R-049 / R-050 / R-051 / R-052 / R-053 / R-054 / R-055 / R-056 / R-057 / R-058 / D-075 / D-076）。
template <>
class TaskHandle<void> {
public:
    TaskHandle() noexcept = default;
    ~TaskHandle() = default;

    TaskHandle(const TaskHandle&) = default;
    TaskHandle& operator=(const TaskHandle&) = default;

    TaskHandle(TaskHandle&&) noexcept = default;
    TaskHandle& operator=(TaskHandle&&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(state_);
    }

    [[nodiscard]] TaskId task_id() const {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return state_->id();
    }

    [[nodiscard]] Priority priority() const {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return state_->priority();
    }

    [[nodiscard]] std::optional<TaskDeadline> deadline() const {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return state_->deadline();
    }

    [[nodiscard]] DeadlineDisposition deadline_disposition() const {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return state_->deadline_disposition();
    }

    [[nodiscard]] TaskState state() const {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return state_->state();
    }

    // R-051 / D-076: 仅允许左值 Handle 显式调用（get() const &）返回 void
    void get() const & {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        state_->get();
    }

    void get() const && = delete;

    // R-055 / D-061: 同步等待完成
    void wait() const {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        state_->wait();
    }

    // R-056 / D-063: 有界等待
    template <typename Rep, typename Period>
    [[nodiscard]] WaitResult wait_for(const std::chrono::duration<Rep, Period>& duration) const {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return state_->wait_for(duration);
    }

    // R-053 / R-057: 请求取消（空 Handle 为 no-op）
    void request_cancel() const noexcept {
        if (state_) {
            state_->request_cancel();
        }
    }

    // R-076 / D-120: co_await 左值 TaskHandle<void>（rvalue deleted）
    [[nodiscard]] detail::TaskHandleAwaiter<void> operator co_await() const &;
    void operator co_await() const && = delete;
    void operator co_await() && = delete;

private:
    friend class Scheduler;
    friend struct detail::TaskHandleAwaiter<void>;

    explicit TaskHandle(std::shared_ptr<detail::TaskSharedState<void>> state) noexcept
        : state_(std::move(state)) {}

    [[nodiscard]] std::shared_ptr<detail::TaskSharedState<void>> shared_state_internal() const noexcept {
        return state_;
    }

    std::shared_ptr<detail::TaskSharedState<void>> state_;
};

// 任务提交结果类型（R-062 / D-088）。
template <typename T>
using SubmissionResult = std::variant<TaskHandle<T>, SubmissionError>;

}  // namespace astra

#endif  // ASTRA_TASK_HANDLE_HPP

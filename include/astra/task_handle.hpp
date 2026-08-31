#ifndef ASTRA_TASK_HANDLE_HPP
#define ASTRA_TASK_HANDLE_HPP

#include <astra/error.hpp>
#include <astra/export.hpp>
#include <astra/id.hpp>
#include <astra/status.hpp>
#include <astra/task_options.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

// ============================================================================
// TaskHandle —— 任务结果的生命周期与线程间交接。
//
// 【这是什么】
//   submit()/spawn() 返回 TaskHandle<T>：它是"任务结果"的共享句柄。
//   结果格（private nested）承载值、异常与终态观察；运行协议编进
//   compiled TaskControlBlock（R-119 / D-173）。你可以：
//     - get()      阻塞取值（异常会在调用方重新抛出）
//     - wait()/wait_for()   只等完成不取值
//     - request_cancel()    请求协作式取消
//     - co_await handle     在另一个 Astra 协程里等待它
//
// 【TaskHandle 的副本语义】
//   TaskHandle 可复制：多个副本指向同一结果格，最后一个销毁时
//   触发"未被观察的失败"诊断——异常任务若从未被 get/await 过，
//   会在 Metrics 里计一次 unobserved_failures（不崩溃、不吞异常，
//   只让你知道有失败被忽略了）。
//
// 【取消模型（协作式）】
//   request_cancel() 只是发出停止信号（stop_token）；任务体通过
//   stop_token 轮询，或在挂起点（sleep/await）被动收到 task_cancelled
//   异常。绝不强制杀死正在运行的任务。
// ============================================================================

namespace astra {

class Scheduler;
template <typename T>
class TaskPromise;

// 若 token 已请求停止则抛 task_cancelled，否则立即返回。供任务体协作取消轮询（R-054）。
inline void throw_if_stop_requested(std::stop_token token) {
    if (token.stop_requested()) {
        throw task_cancelled{};
    }
}

namespace detail {

class AwaitHandshake;
class TaskControlBlock;
class GraphCoroutineNodeInvoker;
class GraphExecution;
struct GraphCoroutineResumeWrapper;

template <typename T>
struct TaskPromiseBase;

template <typename T>
struct TaskHandleAwaiter;

template <typename T>
class CoroutineTaskInvokerModel;

template <typename T>
class CoroutineResumeInvokerModel;

template <bool Ordinary, typename ResultType, typename F, typename... Args>
class TaskInvokerModel;

struct TaskInvokerBase {
    virtual ~TaskInvokerBase() = default;
    virtual void execute() = 0;
    virtual void cancel_pre_start() noexcept = 0;
    virtual void abandon_unstarted() noexcept {}
    [[nodiscard]] virtual bool is_resume_segment() const noexcept { return false; }
    [[nodiscard]] virtual bool is_coroutine_node() const noexcept { return false; }
    [[nodiscard]] virtual Priority priority() const noexcept { return Priority::Normal; }
    [[nodiscard]] virtual std::optional<TaskDeadline> deadline() const noexcept { return std::nullopt; }

    // D-100：已接受 Task 的侵入式 Scheduling Reference。Local cell 与
    // Global Injection fallback 只保存该对象指针，不再额外分配队列节点。
    TaskInvokerBase* ready_next{nullptr};
    bool ready_is_external{false};
};

using TaskRescheduler = std::function<void(std::unique_ptr<TaskInvokerBase>)>;
using TimerRegistrar = std::function<std::uint64_t(
    std::chrono::steady_clock::time_point,
    std::shared_ptr<AwaitHandshake>,
    std::function<void()>)>;
using TimerCanceller = std::function<void(std::uint64_t)>;

ASTRA_EXPORT std::shared_ptr<TaskControlBlock> make_task_control_block(
    TaskId id,
    Priority priority = Priority::Normal,
    std::optional<TaskDeadline> deadline = std::nullopt);

ASTRA_EXPORT TaskId tcb_id(const TaskControlBlock&) noexcept;
ASTRA_EXPORT Priority tcb_priority(const TaskControlBlock&) noexcept;
ASTRA_EXPORT std::optional<TaskDeadline> tcb_deadline(const TaskControlBlock&) noexcept;
ASTRA_EXPORT DeadlineDisposition tcb_deadline_disposition(const TaskControlBlock&) noexcept;
ASTRA_EXPORT TaskState tcb_state(const TaskControlBlock&) noexcept;
ASTRA_EXPORT bool tcb_is_completed(const TaskControlBlock&) noexcept;
ASTRA_EXPORT std::stop_token tcb_stop_token(const TaskControlBlock&) noexcept;
ASTRA_EXPORT std::chrono::steady_clock::time_point tcb_ready_published_at(const TaskControlBlock&) noexcept;
ASTRA_EXPORT void tcb_set_ready_published_at(
    TaskControlBlock&,
    std::chrono::steady_clock::time_point) noexcept;
ASTRA_EXPORT std::uint64_t tcb_resume_handoff_seq(const TaskControlBlock&) noexcept;
ASTRA_EXPORT void tcb_mark_resume_handoff(TaskControlBlock&) noexcept;
ASTRA_EXPORT void tcb_set_rescheduler(TaskControlBlock&, TaskRescheduler);
ASTRA_EXPORT TaskRescheduler tcb_get_rescheduler(const TaskControlBlock&);
ASTRA_EXPORT void tcb_set_timer_functions(TaskControlBlock&, TimerRegistrar, TimerCanceller);
ASTRA_EXPORT TimerRegistrar tcb_get_timer_registrar(const TaskControlBlock&);
ASTRA_EXPORT TimerCanceller tcb_get_timer_canceller(const TaskControlBlock&);
ASTRA_EXPORT void tcb_transition_to_suspended(TaskControlBlock&) noexcept;
ASTRA_EXPORT void tcb_transition_to_running(TaskControlBlock&) noexcept;
ASTRA_EXPORT void tcb_add_completion_callback(TaskControlBlock&, std::function<void()>);
ASTRA_EXPORT void tcb_request_cancel(TaskControlBlock&) noexcept;
ASTRA_EXPORT bool tcb_try_start(TaskControlBlock&) noexcept;
ASTRA_EXPORT void tcb_succeed(TaskControlBlock&);
ASTRA_EXPORT void tcb_fail(TaskControlBlock&, std::exception_ptr) noexcept;
ASTRA_EXPORT void tcb_set_cancelled(TaskControlBlock&) noexcept;
ASTRA_EXPORT std::exception_ptr tcb_exception(const TaskControlBlock&) noexcept;
ASTRA_EXPORT void tcb_mark_observed(const TaskControlBlock&) noexcept;

ASTRA_EXPORT void perform_caller_wait(
    const TaskControlBlock& target,
    std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);

ASTRA_EXPORT std::unique_ptr<TaskInvokerBase> wrap_submitted_invoker(
    std::unique_ptr<TaskInvokerBase> inner,
    std::shared_ptr<void> protocol);

ASTRA_EXPORT TaskId current_executing_task_id() noexcept;
ASTRA_EXPORT Priority current_executing_task_priority() noexcept;

struct ASTRA_EXPORT TaskExecutionContextGuard {
    TaskId prev_id;
    Priority prev_priority;
    explicit TaskExecutionContextGuard(TaskId new_id, Priority new_priority = Priority::Normal) noexcept;
    ~TaskExecutionContextGuard() noexcept;
};

ASTRA_EXPORT void record_metrics_submission_attempt(RuntimeId id) noexcept;
void record_metrics_first_start(TaskId id, std::optional<DeadlineDisposition> dl_disp) noexcept;
void record_metrics_succeeded(TaskId id) noexcept;
void record_metrics_failed(TaskId id) noexcept;
void record_metrics_cancelled_cooperative(TaskId id) noexcept;
void record_metrics_cancelled_before_start(TaskId id, bool has_deadline) noexcept;
void record_metrics_unobserved_failure(TaskId id) noexcept;
void record_metrics_suspended(TaskId id) noexcept;
void record_metrics_resumed(TaskId id) noexcept;
void record_metrics_resume_segment(TaskId id) noexcept;
void record_metrics_explicit_yield() noexcept;

ASTRA_EXPORT void record_wait_call(TaskId target, bool timed_out) noexcept;
ASTRA_EXPORT void record_self_wait_rejection(TaskId target) noexcept;
void record_await_registration(TaskId source, TaskId target) noexcept;
void record_await_triggered(TaskId source, TaskId target, bool cancelled) noexcept;
void record_await_resumed(TaskId source, TaskId target, std::uint64_t duration_ns) noexcept;

void record_metrics_ready_queue_wait(TaskId id, std::uint64_t duration_ns) noexcept;
void record_metrics_execution_segment(TaskId id, std::uint64_t duration_ns) noexcept;
void record_metrics_task_wall_time(TaskId id, std::uint64_t duration_ns) noexcept;
void record_metrics_blocking_admission_wait(RuntimeId id, std::uint64_t duration_ns) noexcept;
void record_metrics_timer_wake_lateness(RuntimeId id, std::uint64_t duration_ns) noexcept;
void record_metrics_deadline_start_lateness(TaskId id, std::uint64_t duration_ns) noexcept;
void record_metrics_worker_park_duration(RuntimeId id, std::uint64_t duration_ns) noexcept;
void record_metrics_runtime_join_latency(RuntimeId id, std::uint64_t duration_ns) noexcept;

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

}  // namespace detail

/**
 * @brief 已提交任务结果的共享观察句柄。
 * @tparam T 任务结果类型。
 * @note 复制句柄不会复制任务；多个副本观察同一结果格。
 */
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
        return static_cast<bool>(cell_);
    }

    [[nodiscard]] TaskId task_id() const {
        if (!cell_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return cell_->id();
    }

    [[nodiscard]] Priority priority() const {
        if (!cell_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return cell_->priority();
    }

    [[nodiscard]] std::optional<TaskDeadline> deadline() const {
        if (!cell_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return cell_->deadline();
    }

    [[nodiscard]] DeadlineDisposition deadline_disposition() const {
        if (!cell_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return cell_->deadline_disposition();
    }

    [[nodiscard]] TaskState state() const {
        if (!cell_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return cell_->state();
    }

    /**
     * @brief 阻塞等待并返回任务结果。
     * @throws task_cancelled 任务被取消。
     * @throws std::exception 任务体抛出的异常。
     */
    const T& get() const & {
        if (!cell_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return cell_->get();
    }

    void get() const && = delete;

    /** @brief 阻塞等待任务到达终态，不读取结果值。 */
    void wait() const {
        if (!cell_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        cell_->wait();
    }

    /**
     * @brief 在给定时长内等待任务完成。
     * @param duration 最大等待时长；超时不会取消任务。
     */
    template <typename Rep, typename Period>
    [[nodiscard]] WaitResult wait_for(const std::chrono::duration<Rep, Period>& duration) const {
        if (!cell_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return cell_->wait_for(duration);
    }

    /** @brief 发出协作式取消请求，不强制终止正在运行的任务。 */
    void request_cancel() const noexcept {
        if (cell_) {
            cell_->request_cancel();
        }
    }

    [[nodiscard]] detail::TaskHandleAwaiter<T> operator co_await() const &;
    void operator co_await() const && = delete;
    void operator co_await() && = delete;

private:
    struct ResultCell {
        std::optional<T> value_;
        std::shared_ptr<detail::TaskControlBlock> protocol_;

        explicit ResultCell(
            TaskId id,
            Priority priority = Priority::Normal,
            std::optional<TaskDeadline> deadline = std::nullopt)
            : protocol_(detail::make_task_control_block(id, priority, deadline)) {}

        [[nodiscard]] std::shared_ptr<void> protocol_token() const {
            return protocol_;
        }

        [[nodiscard]] TaskId id() const noexcept { return detail::tcb_id(*protocol_); }
        [[nodiscard]] Priority priority() const noexcept { return detail::tcb_priority(*protocol_); }
        [[nodiscard]] std::optional<TaskDeadline> deadline() const noexcept { return detail::tcb_deadline(*protocol_); }
        [[nodiscard]] DeadlineDisposition deadline_disposition() const noexcept {
            return detail::tcb_deadline_disposition(*protocol_);
        }
        [[nodiscard]] TaskState state() const noexcept { return detail::tcb_state(*protocol_); }
        [[nodiscard]] bool is_completed() const noexcept { return detail::tcb_is_completed(*protocol_); }
        [[nodiscard]] std::stop_token stop_token() const noexcept { return detail::tcb_stop_token(*protocol_); }
        [[nodiscard]] std::chrono::steady_clock::time_point ready_published_at() const noexcept {
            return detail::tcb_ready_published_at(*protocol_);
        }
        void set_ready_published_at(std::chrono::steady_clock::time_point tp) noexcept {
            detail::tcb_set_ready_published_at(*protocol_, tp);
        }
        [[nodiscard]] std::uint64_t resume_handoff_seq() const noexcept {
            return detail::tcb_resume_handoff_seq(*protocol_);
        }
        void mark_resume_handoff() noexcept { detail::tcb_mark_resume_handoff(*protocol_); }
        void set_rescheduler(detail::TaskRescheduler rescheduler) {
            detail::tcb_set_rescheduler(*protocol_, std::move(rescheduler));
        }
        [[nodiscard]] detail::TaskRescheduler get_rescheduler() const {
            return detail::tcb_get_rescheduler(*protocol_);
        }
        void set_timer_functions(detail::TimerRegistrar registrar, detail::TimerCanceller canceller) {
            detail::tcb_set_timer_functions(*protocol_, std::move(registrar), std::move(canceller));
        }
        [[nodiscard]] detail::TimerRegistrar get_timer_registrar() const {
            return detail::tcb_get_timer_registrar(*protocol_);
        }
        [[nodiscard]] detail::TimerCanceller get_timer_canceller() const {
            return detail::tcb_get_timer_canceller(*protocol_);
        }
        void transition_to_suspended() noexcept { detail::tcb_transition_to_suspended(*protocol_); }
        void transition_to_running() noexcept { detail::tcb_transition_to_running(*protocol_); }
        void add_completion_callback(std::function<void()> cb) {
            detail::tcb_add_completion_callback(*protocol_, std::move(cb));
        }
        void request_cancel() noexcept { detail::tcb_request_cancel(*protocol_); }
        bool try_start() noexcept { return detail::tcb_try_start(*protocol_); }
        [[nodiscard]] std::exception_ptr exception() const noexcept { return detail::tcb_exception(*protocol_); }

        void set_value(T val) {
            value_.emplace(std::move(val));
            detail::tcb_succeed(*protocol_);
        }

        void set_exception(std::exception_ptr ex) noexcept {
            detail::tcb_fail(*protocol_, std::move(ex));
        }

        void set_cancelled() noexcept {
            detail::tcb_set_cancelled(*protocol_);
        }

        const T& get() const {
            detail::perform_caller_wait(*protocol_);
            const auto s = detail::tcb_state(*protocol_);
            if (s == TaskState::Failed) {
                detail::tcb_mark_observed(*protocol_);
                std::rethrow_exception(detail::tcb_exception(*protocol_));
            }
            if (s == TaskState::Cancelled) {
                throw task_cancelled{};
            }
            return *value_;
        }

        void wait() const {
            detail::perform_caller_wait(*protocol_);
        }

        template <typename Rep, typename Period>
        WaitResult wait_for(const std::chrono::duration<Rep, Period>& duration) const {
            if (duration <= std::chrono::duration<Rep, Period>::zero()) {
                const bool completed = detail::tcb_is_completed(*protocol_);
                detail::record_wait_call(detail::tcb_id(*protocol_), !completed);
                return completed ? WaitResult::Completed : WaitResult::TimedOut;
            }
            const auto deadline = std::chrono::steady_clock::now() + duration;
            detail::perform_caller_wait(*protocol_, deadline);
            return detail::tcb_is_completed(*protocol_) ? WaitResult::Completed : WaitResult::TimedOut;
        }
    };

    friend class Scheduler;
    friend struct detail::TaskHandleAwaiter<T>;
    friend struct detail::TaskPromiseBase<T>;
    friend class TaskPromise<T>;
    friend class detail::CoroutineTaskInvokerModel<T>;
    friend class detail::CoroutineResumeInvokerModel<T>;
    template <bool Ordinary, typename ResultType, typename F, typename... Args>
    friend class detail::TaskInvokerModel;

    template <bool Ordinary, typename F, typename... Args>
    static std::unique_ptr<detail::TaskInvokerBase> make_invoker(
        std::shared_ptr<ResultCell> state, F&& f, Args&&... args) {
        return std::make_unique<detail::TaskInvokerModel<Ordinary, T, F, Args...>>(
            std::move(state), std::forward<F>(f), std::forward<Args>(args)...);
    }

    explicit TaskHandle(std::shared_ptr<ResultCell> cell) noexcept
        : cell_(std::move(cell)) {}

    [[nodiscard]] std::shared_ptr<ResultCell> shared_state_internal() const noexcept {
        return cell_;
    }

    std::shared_ptr<ResultCell> cell_;
};

/** @brief void 任务结果的共享观察句柄特化。 */
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
        return static_cast<bool>(cell_);
    }

    [[nodiscard]] TaskId task_id() const {
        if (!cell_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return cell_->id();
    }

    [[nodiscard]] Priority priority() const {
        if (!cell_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return cell_->priority();
    }

    [[nodiscard]] std::optional<TaskDeadline> deadline() const {
        if (!cell_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return cell_->deadline();
    }

    [[nodiscard]] DeadlineDisposition deadline_disposition() const {
        if (!cell_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return cell_->deadline_disposition();
    }

    [[nodiscard]] TaskState state() const {
        if (!cell_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return cell_->state();
    }

    /**
     * @brief 阻塞等待 void 任务完成并重新抛出任务异常。
     * @throws task_cancelled 任务被取消。
     */
    void get() const & {
        if (!cell_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        cell_->get();
    }

    void get() const && = delete;

    /** @brief 阻塞等待 void 任务到达终态。 */
    void wait() const {
        if (!cell_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        cell_->wait();
    }

    /**
     * @brief 在给定时长内等待 void 任务完成。
     * @param duration 最大等待时长；超时不会取消任务。
     */
    template <typename Rep, typename Period>
    [[nodiscard]] WaitResult wait_for(const std::chrono::duration<Rep, Period>& duration) const {
        if (!cell_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return cell_->wait_for(duration);
    }

    /** @brief 发出 void 任务的协作式取消请求。 */
    void request_cancel() const noexcept {
        if (cell_) {
            cell_->request_cancel();
        }
    }

    [[nodiscard]] detail::TaskHandleAwaiter<void> operator co_await() const &;
    void operator co_await() const && = delete;
    void operator co_await() && = delete;

private:
    struct ResultCell {
        std::shared_ptr<detail::TaskControlBlock> protocol_;

        explicit ResultCell(
            TaskId id,
            Priority priority = Priority::Normal,
            std::optional<TaskDeadline> deadline = std::nullopt)
            : protocol_(detail::make_task_control_block(id, priority, deadline)) {}

        [[nodiscard]] std::shared_ptr<void> protocol_token() const {
            return protocol_;
        }

        [[nodiscard]] TaskId id() const noexcept { return detail::tcb_id(*protocol_); }
        [[nodiscard]] Priority priority() const noexcept { return detail::tcb_priority(*protocol_); }
        [[nodiscard]] std::optional<TaskDeadline> deadline() const noexcept { return detail::tcb_deadline(*protocol_); }
        [[nodiscard]] DeadlineDisposition deadline_disposition() const noexcept {
            return detail::tcb_deadline_disposition(*protocol_);
        }
        [[nodiscard]] TaskState state() const noexcept { return detail::tcb_state(*protocol_); }
        [[nodiscard]] bool is_completed() const noexcept { return detail::tcb_is_completed(*protocol_); }
        [[nodiscard]] std::stop_token stop_token() const noexcept { return detail::tcb_stop_token(*protocol_); }
        [[nodiscard]] std::chrono::steady_clock::time_point ready_published_at() const noexcept {
            return detail::tcb_ready_published_at(*protocol_);
        }
        void set_ready_published_at(std::chrono::steady_clock::time_point tp) noexcept {
            detail::tcb_set_ready_published_at(*protocol_, tp);
        }
        [[nodiscard]] std::uint64_t resume_handoff_seq() const noexcept {
            return detail::tcb_resume_handoff_seq(*protocol_);
        }
        void mark_resume_handoff() noexcept { detail::tcb_mark_resume_handoff(*protocol_); }
        void set_rescheduler(detail::TaskRescheduler rescheduler) {
            detail::tcb_set_rescheduler(*protocol_, std::move(rescheduler));
        }
        [[nodiscard]] detail::TaskRescheduler get_rescheduler() const {
            return detail::tcb_get_rescheduler(*protocol_);
        }
        void set_timer_functions(detail::TimerRegistrar registrar, detail::TimerCanceller canceller) {
            detail::tcb_set_timer_functions(*protocol_, std::move(registrar), std::move(canceller));
        }
        [[nodiscard]] detail::TimerRegistrar get_timer_registrar() const {
            return detail::tcb_get_timer_registrar(*protocol_);
        }
        [[nodiscard]] detail::TimerCanceller get_timer_canceller() const {
            return detail::tcb_get_timer_canceller(*protocol_);
        }
        void transition_to_suspended() noexcept { detail::tcb_transition_to_suspended(*protocol_); }
        void transition_to_running() noexcept { detail::tcb_transition_to_running(*protocol_); }
        void add_completion_callback(std::function<void()> cb) {
            detail::tcb_add_completion_callback(*protocol_, std::move(cb));
        }
        void request_cancel() noexcept { detail::tcb_request_cancel(*protocol_); }
        bool try_start() noexcept { return detail::tcb_try_start(*protocol_); }
        [[nodiscard]] std::exception_ptr exception() const noexcept { return detail::tcb_exception(*protocol_); }

        void set_value() {
            detail::tcb_succeed(*protocol_);
        }

        void set_exception(std::exception_ptr ex) noexcept {
            detail::tcb_fail(*protocol_, std::move(ex));
        }

        void set_cancelled() noexcept {
            detail::tcb_set_cancelled(*protocol_);
        }

        void get() const {
            detail::perform_caller_wait(*protocol_);
            const auto s = detail::tcb_state(*protocol_);
            if (s == TaskState::Failed) {
                detail::tcb_mark_observed(*protocol_);
                std::rethrow_exception(detail::tcb_exception(*protocol_));
            }
            if (s == TaskState::Cancelled) {
                throw task_cancelled{};
            }
        }

        void wait() const {
            detail::perform_caller_wait(*protocol_);
        }

        template <typename Rep, typename Period>
        WaitResult wait_for(const std::chrono::duration<Rep, Period>& duration) const {
            if (duration <= std::chrono::duration<Rep, Period>::zero()) {
                const bool completed = detail::tcb_is_completed(*protocol_);
                detail::record_wait_call(detail::tcb_id(*protocol_), !completed);
                return completed ? WaitResult::Completed : WaitResult::TimedOut;
            }
            const auto deadline = std::chrono::steady_clock::now() + duration;
            detail::perform_caller_wait(*protocol_, deadline);
            return detail::tcb_is_completed(*protocol_) ? WaitResult::Completed : WaitResult::TimedOut;
        }
    };

    friend class Scheduler;
    friend struct detail::TaskHandleAwaiter<void>;
    friend struct detail::TaskPromiseBase<void>;
    friend class TaskPromise<void>;
    friend class detail::CoroutineTaskInvokerModel<void>;
    friend class detail::CoroutineResumeInvokerModel<void>;
    friend class detail::GraphCoroutineNodeInvoker;
    friend class detail::GraphExecution;
    friend struct detail::GraphCoroutineResumeWrapper;
    template <bool Ordinary, typename ResultType, typename F, typename... Args>
    friend class detail::TaskInvokerModel;

    template <bool Ordinary, typename F, typename... Args>
    static std::unique_ptr<detail::TaskInvokerBase> make_invoker(
        std::shared_ptr<ResultCell> state, F&& f, Args&&... args) {
        return std::make_unique<detail::TaskInvokerModel<Ordinary, void, F, Args...>>(
            std::move(state), std::forward<F>(f), std::forward<Args>(args)...);
    }

    explicit TaskHandle(std::shared_ptr<ResultCell> cell) noexcept
        : cell_(std::move(cell)) {}

    [[nodiscard]] std::shared_ptr<ResultCell> shared_state_internal() const noexcept {
        return cell_;
    }

    std::shared_ptr<ResultCell> cell_;
};

namespace detail {

template <bool Ordinary, typename ResultType, typename F, typename... Args>
class TaskInvokerModel : public TaskInvokerBase {
public:
    using Cell = typename TaskHandle<ResultType>::ResultCell;

    template <typename UF, typename... UArgs>
    TaskInvokerModel(std::shared_ptr<Cell> state, UF&& f, UArgs&&... args)
        : state_(std::move(state)),
          fn_(std::forward<UF>(f)),
          args_(std::forward<UArgs>(args)...) {}

    void execute() override {
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
        try {
            if constexpr (Ordinary) {
                if constexpr (std::is_void_v<ResultType>) {
                    std::invoke(std::move(fn_), std::get<Is>(std::move(args_))...);
                    state_->set_value();
                } else {
                    auto res = std::invoke(std::move(fn_), std::get<Is>(std::move(args_))...);
                    state_->set_value(std::move(res));
                }
            } else {
                if constexpr (std::is_void_v<ResultType>) {
                    std::invoke(std::move(fn_), state_->stop_token(), std::get<Is>(std::move(args_))...);
                    state_->set_value();
                } else {
                    auto res = std::invoke(std::move(fn_), state_->stop_token(), std::get<Is>(std::move(args_))...);
                    state_->set_value(std::move(res));
                }
            }
        } catch (const task_cancelled&) {
            state_->set_cancelled();
        } catch (...) {
            state_->set_exception(std::current_exception());
        }
    }

    std::shared_ptr<Cell> state_;
    std::decay_t<F> fn_;
    std::tuple<std::decay_t<Args>...> args_;
};

}  // namespace detail

template <typename T>
using SubmissionResult = std::variant<TaskHandle<T>, SubmissionError>;

}  // namespace astra

#endif  // ASTRA_TASK_HANDLE_HPP

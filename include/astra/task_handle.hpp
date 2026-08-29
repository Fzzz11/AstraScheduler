#ifndef ASTRA_TASK_HANDLE_HPP
#define ASTRA_TASK_HANDLE_HPP

#include <astra/error.hpp>
#include <astra/export.hpp>
#include <astra/id.hpp>
#include <astra/status.hpp>

#include <atomic>
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

namespace astra {

class AwaitHandshake;

// 检查并抛出取消异常辅助函数（R-054 / D-060）。
inline void throw_if_stop_requested(std::stop_token token) {
    if (token.stop_requested()) {
        throw task_cancelled{};
    }
}

namespace detail {

inline TaskId allocate_task_id(RuntimeId runtime_id) noexcept {
    static std::atomic<std::uint64_t> global_task_sequence{0};
    std::uint64_t current = global_task_sequence.load(std::memory_order_relaxed);
    while (true) {
        if (current == std::numeric_limits<std::uint64_t>::max()) {
            return TaskId{runtime_id, std::numeric_limits<std::uint64_t>::max()};
        }
        if (global_task_sequence.compare_exchange_weak(
                current, current + 1, std::memory_order_relaxed)) {
            return TaskId{runtime_id, current + 1};
        }
    }
}

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
};

ASTRA_EXPORT TaskId current_executing_task_id() noexcept;

template <typename T>
struct TaskHandleAwaiter;

struct ASTRA_EXPORT TaskExecutionContextGuard {
    TaskId prev_id;
    explicit TaskExecutionContextGuard(TaskId new_id) noexcept;
    ~TaskExecutionContextGuard() noexcept;
};

class ASTRA_EXPORT TaskSharedStateBase {
public:
    explicit TaskSharedStateBase(TaskId id) : id_(id) {}
    virtual ~TaskSharedStateBase() = default;

    [[nodiscard]] TaskId id() const noexcept {
        return id_;
    }

    [[nodiscard]] std::stop_token stop_token() noexcept {
        return stop_source_.get_token();
    }

    using ReschedulerFunc = std::function<void(std::unique_ptr<TaskInvokerBase>)>;
    using TimerRegistrar = std::function<std::uint64_t(std::chrono::steady_clock::time_point, std::shared_ptr<AwaitHandshake>, std::function<void()>)>;
    using TimerCanceller = std::function<void(std::uint64_t)>;

    void set_rescheduler(ReschedulerFunc rescheduler) {
        std::lock_guard<std::mutex> lock(mutex_);
        rescheduler_ = std::move(rescheduler);
    }

    [[nodiscard]] ReschedulerFunc get_rescheduler() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rescheduler_;
    }

    void set_timer_functions(TimerRegistrar reg, TimerCanceller cancel) {
        std::lock_guard<std::mutex> lock(mutex_);
        timer_registrar_ = std::move(reg);
        timer_canceller_ = std::move(cancel);
    }

    [[nodiscard]] TimerRegistrar get_timer_registrar() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return timer_registrar_;
    }

    [[nodiscard]] TimerCanceller get_timer_canceller() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return timer_canceller_;
    }

    void transition_to_suspended() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.load(std::memory_order_relaxed) == TaskState::Running) {
            state_.store(TaskState::Suspended, std::memory_order_release);
        }
    }

    void transition_to_running() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.load(std::memory_order_relaxed) == TaskState::Suspended ||
            state_.load(std::memory_order_relaxed) == TaskState::Ready) {
            state_.store(TaskState::Running, std::memory_order_release);
        }
    }

    void add_completion_callback(std::function<void()> cb) {
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

    void request_cancel() noexcept {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_source_.request_stop();
            if (state_.load(std::memory_order_relaxed) == TaskState::Ready) {
                state_.store(TaskState::Cancelled, std::memory_order_release);
                callbacks = std::move(completion_callbacks_);
            }
        }
        cv_.notify_all();
        for (auto& cb : callbacks) {
            if (cb) {
                cb();
            }
        }
    }

    bool try_start() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.load(std::memory_order_relaxed) == TaskState::Ready) {
            state_.store(TaskState::Running, std::memory_order_release);
            return true;
        }
        return false;
    }

    [[nodiscard]] TaskState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::condition_variable& cv() const noexcept {
        return cv_;
    }

    [[nodiscard]] std::mutex& mutex() const noexcept {
        return mutex_;
    }

    void set_exception(std::exception_ptr ex) noexcept {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            exception_ = std::move(ex);
            state_.store(TaskState::Failed, std::memory_order_release);
            callbacks = std::move(completion_callbacks_);
            rescheduler_ = nullptr;
        }
        cv_.notify_all();
        for (auto& cb : callbacks) {
            if (cb) {
                cb();
            }
        }
    }

    void set_cancelled() noexcept {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.store(TaskState::Cancelled, std::memory_order_release);
            callbacks = std::move(completion_callbacks_);
            rescheduler_ = nullptr;
        }
        cv_.notify_all();
        for (auto& cb : callbacks) {
            if (cb) {
                cb();
            }
        }
    }

    [[nodiscard]] std::exception_ptr exception() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return exception_;
    }

    void wait() const {
        perform_caller_wait(*this);
    }

    template <typename Rep, typename Period>
    WaitResult wait_for(const std::chrono::duration<Rep, Period>& duration) const {
        if (duration <= std::chrono::duration<Rep, Period>::zero()) {
            return is_completed() ? WaitResult::Completed : WaitResult::TimedOut;
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
    std::stop_source stop_source_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::atomic<TaskState> state_{TaskState::Ready};
    std::exception_ptr exception_{nullptr};
    mutable std::atomic<bool> observed_{false};
    std::vector<std::function<void()>> completion_callbacks_;
    ReschedulerFunc rescheduler_;
    TimerRegistrar timer_registrar_{nullptr};
    TimerCanceller timer_canceller_{nullptr};
};

template <typename T>
class TaskSharedState : public TaskSharedStateBase {
public:
    explicit TaskSharedState(TaskId id) : TaskSharedStateBase(id) {}

    void set_value(T val) {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            value_.emplace(std::move(val));
            state_.store(TaskState::Succeeded, std::memory_order_release);
            callbacks = std::move(completion_callbacks_);
            rescheduler_ = nullptr;
        }
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
    explicit TaskSharedState(TaskId id) : TaskSharedStateBase(id) {}

    void set_value() {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.store(TaskState::Succeeded, std::memory_order_release);
            callbacks = std::move(completion_callbacks_);
            rescheduler_ = nullptr;
        }
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

private:
    template <std::size_t... Is>
    void invoke_impl(std::index_sequence<Is...>) {
        TaskExecutionContextGuard context_guard(state_->id());
        try {
            if constexpr (Ordinary) {
                if constexpr (std::is_void_v<ResultType>) {
                    std::invoke(std::move(fn_), std::get<Is>(std::move(args_))...);
                    state_->set_value();
                } else {
                    state_->set_value(std::invoke(std::move(fn_), std::get<Is>(std::move(args_))...));
                }
            } else {
                if constexpr (std::is_void_v<ResultType>) {
                    std::invoke(std::move(fn_), state_->stop_token(), std::get<Is>(std::move(args_))...);
                    state_->set_value();
                } else {
                    state_->set_value(std::invoke(std::move(fn_), state_->stop_token(), std::get<Is>(std::move(args_))...));
                }
            }
        } catch (const task_cancelled&) {
            state_->set_cancelled();
        } catch (...) {
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

    explicit TaskHandle(std::shared_ptr<detail::TaskSharedState<T>> state) noexcept
        : state_(std::move(state)) {}

    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(state_);
    }

    [[nodiscard]] TaskId task_id() const {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return state_->id();
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

    [[nodiscard]] std::shared_ptr<detail::TaskSharedState<T>> shared_state_internal() const noexcept {
        return state_;
    }

private:
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

    explicit TaskHandle(std::shared_ptr<detail::TaskSharedState<void>> state) noexcept
        : state_(std::move(state)) {}

    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(state_);
    }

    [[nodiscard]] TaskId task_id() const {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return state_->id();
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

    [[nodiscard]] std::shared_ptr<detail::TaskSharedState<void>> shared_state_internal() const noexcept {
        return state_;
    }

private:
    std::shared_ptr<detail::TaskSharedState<void>> state_;
};

// 任务提交结果类型（R-062 / D-088）。
template <typename T>
using SubmissionResult = std::variant<TaskHandle<T>, SubmissionError>;

}  // namespace astra

#endif  // ASTRA_TASK_HANDLE_HPP

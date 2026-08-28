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

class ASTRA_EXPORT TaskSharedStateBase {
public:
    virtual ~TaskSharedStateBase() = default;
    [[nodiscard]] virtual TaskId id() const noexcept = 0;
    [[nodiscard]] virtual TaskState state() const noexcept = 0;
    [[nodiscard]] virtual bool is_completed() const noexcept = 0;
    virtual void request_stop() noexcept = 0;
    [[nodiscard]] virtual std::stop_token stop_token() noexcept = 0;
    [[nodiscard]] virtual std::condition_variable& cv() const noexcept = 0;
    [[nodiscard]] virtual std::mutex& mutex() const noexcept = 0;
};

ASTRA_EXPORT void perform_caller_wait(
    const TaskSharedStateBase& target,
    std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);

struct ASTRA_EXPORT TaskExecutionContextGuard {
    TaskId prev_id;
    explicit TaskExecutionContextGuard(TaskId new_id) noexcept;
    ~TaskExecutionContextGuard() noexcept;
};

struct TaskInvokerBase {
    virtual ~TaskInvokerBase() = default;
    virtual void execute() = 0;
};

template <typename T>
class TaskSharedState : public TaskSharedStateBase {
public:
    explicit TaskSharedState(TaskId id) : id_(id) {}

    ~TaskSharedState() override {
        // R-060: 未观察异常析构时不抛出、不终止
    }

    [[nodiscard]] TaskId id() const noexcept override {
        return id_;
    }

    [[nodiscard]] std::stop_token stop_token() noexcept override {
        return stop_source_.get_token();
    }

    void request_stop() noexcept override {
        stop_source_.request_stop();
    }

    [[nodiscard]] TaskState state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::condition_variable& cv() const noexcept override {
        return cv_;
    }

    [[nodiscard]] std::mutex& mutex() const noexcept override {
        return mutex_;
    }

    void mark_running() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.load(std::memory_order_relaxed) == TaskState::Ready) {
            state_.store(TaskState::Running, std::memory_order_release);
        }
    }

    void set_value(T val) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            value_.emplace(std::move(val));
            state_.store(TaskState::Succeeded, std::memory_order_release);
        }
        cv_.notify_all();
    }

    void set_exception(std::exception_ptr ex) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            exception_ = std::move(ex);
            state_.store(TaskState::Failed, std::memory_order_release);
        }
        cv_.notify_all();
    }

    void set_cancelled() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.store(TaskState::Cancelled, std::memory_order_release);
        }
        cv_.notify_all();
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

    [[nodiscard]] bool is_completed() const noexcept override {
        const auto s = state_.load(std::memory_order_acquire);
        return s == TaskState::Succeeded || s == TaskState::Failed || s == TaskState::Cancelled;
    }

private:
    TaskId id_;
    std::stop_source stop_source_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::atomic<TaskState> state_{TaskState::Ready};
    std::optional<T> value_;
    std::exception_ptr exception_{nullptr};
    mutable std::atomic<bool> observed_{false};
};

template <>
class TaskSharedState<void> : public TaskSharedStateBase {
public:
    explicit TaskSharedState(TaskId id) : id_(id) {}

    ~TaskSharedState() override {
        // R-060: 未观察异常析构时不抛出、不终止
    }

    [[nodiscard]] TaskId id() const noexcept override {
        return id_;
    }

    [[nodiscard]] std::stop_token stop_token() noexcept override {
        return stop_source_.get_token();
    }

    void request_stop() noexcept override {
        stop_source_.request_stop();
    }

    [[nodiscard]] TaskState state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::condition_variable& cv() const noexcept override {
        return cv_;
    }

    [[nodiscard]] std::mutex& mutex() const noexcept override {
        return mutex_;
    }

    void mark_running() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.load(std::memory_order_relaxed) == TaskState::Ready) {
            state_.store(TaskState::Running, std::memory_order_release);
        }
    }

    void set_value() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.store(TaskState::Succeeded, std::memory_order_release);
        }
        cv_.notify_all();
    }

    void set_exception(std::exception_ptr ex) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            exception_ = std::move(ex);
            state_.store(TaskState::Failed, std::memory_order_release);
        }
        cv_.notify_all();
    }

    void set_cancelled() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.store(TaskState::Cancelled, std::memory_order_release);
        }
        cv_.notify_all();
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

    [[nodiscard]] bool is_completed() const noexcept override {
        const auto s = state_.load(std::memory_order_acquire);
        return s == TaskState::Succeeded || s == TaskState::Failed || s == TaskState::Cancelled;
    }

private:
    TaskId id_;
    std::stop_source stop_source_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::atomic<TaskState> state_{TaskState::Ready};
    std::exception_ptr exception_{nullptr};
    mutable std::atomic<bool> observed_{false};
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
        constexpr std::size_t tuple_size = std::tuple_size_v<decltype(args_)>;
        invoke_impl(std::make_index_sequence<tuple_size>{});
    }

private:
    template <std::size_t... Is>
    void invoke_impl(std::index_sequence<Is...>) {
        TaskExecutionContextGuard context_guard(state_->id());
        state_->mark_running();
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

// TaskHandle<T> — 共享任务结果句柄（R-048 / R-049 / R-050 / R-051 / R-052 / R-055 / R-056 / R-057 / R-058 / D-041 / D-042 / D-076）。
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
            state_->request_stop();
        }
    }

private:
    std::shared_ptr<detail::TaskSharedState<T>> state_;
};

// TaskHandle<void> 特化（R-048 / R-049 / R-050 / R-051 / R-052 / R-055 / R-056 / R-057 / R-058 / D-075 / D-076）。
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
            state_->request_stop();
        }
    }

private:
    std::shared_ptr<detail::TaskSharedState<void>> state_;
};

// 任务提交结果类型（R-062 / D-088）。
template <typename T>
using SubmissionResult = std::variant<TaskHandle<T>, SubmissionError>;

}  // namespace astra

#endif  // ASTRA_TASK_HANDLE_HPP

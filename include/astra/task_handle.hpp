#ifndef ASTRA_TASK_HANDLE_HPP
#define ASTRA_TASK_HANDLE_HPP

#include <astra/export.hpp>
#include <astra/id.hpp>

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

namespace astra {

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

struct TaskInvokerBase {
    virtual ~TaskInvokerBase() = default;
    virtual void execute() = 0;
};

template <typename T>
class TaskSharedState {
public:
    explicit TaskSharedState(TaskId id) : id_(id) {}

    TaskId id() const noexcept {
        return id_;
    }

    std::stop_token stop_token() noexcept {
        return stop_source_.get_token();
    }

    void request_stop() noexcept {
        stop_source_.request_stop();
    }

    void set_value(T&& val) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            value_.emplace(std::move(val));
            completed_ = true;
        }
        cv_.notify_all();
    }

    void set_value(const T& val) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            value_.emplace(val);
            completed_ = true;
        }
        cv_.notify_all();
    }

    void set_exception(std::exception_ptr ex) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            exception_ = ex;
            completed_ = true;
        }
        cv_.notify_all();
    }

    const T& get() const {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return completed_; });
        if (exception_) {
            std::rethrow_exception(exception_);
        }
        return *value_;
    }

    bool is_completed() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return completed_;
    }

private:
    TaskId id_;
    std::stop_source stop_source_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    bool completed_{false};
    std::optional<T> value_;
    std::exception_ptr exception_{nullptr};
};

template <>
class TaskSharedState<void> {
public:
    explicit TaskSharedState(TaskId id) : id_(id) {}

    TaskId id() const noexcept {
        return id_;
    }

    std::stop_token stop_token() noexcept {
        return stop_source_.get_token();
    }

    void request_stop() noexcept {
        stop_source_.request_stop();
    }

    void set_value() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            completed_ = true;
        }
        cv_.notify_all();
    }

    void set_exception(std::exception_ptr ex) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            exception_ = ex;
            completed_ = true;
        }
        cv_.notify_all();
    }

    void get() const {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return completed_; });
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

    bool is_completed() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return completed_;
    }

private:
    TaskId id_;
    std::stop_source stop_source_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    bool completed_{false};
    std::exception_ptr exception_{nullptr};
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

// TaskHandle<T> — 共享任务结果句柄（R-048 / R-058 / D-041 / D-042 / D-076）。
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

    [[nodiscard]] TaskId task_id() const noexcept {
        return state_ ? state_->id() : TaskId{};
    }

    // R-058 / D-076: 仅允许左值 Handle 显式调用（get() const &）
    // 临时对象 / rvalue 在编译期被 delete 拒绝以防止悬垂引用
    const T& get() const & {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        return state_->get();
    }

    void get() const && = delete;

private:
    std::shared_ptr<detail::TaskSharedState<T>> state_;
};

// TaskHandle<void> 特化（R-048 / R-058 / D-075 / D-076）。
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

    [[nodiscard]] TaskId task_id() const noexcept {
        return state_ ? state_->id() : TaskId{};
    }

    // R-058 / D-076: 仅允许左值 Handle 显式调用（get() const &）
    void get() const & {
        if (!state_) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        state_->get();
    }

    void get() const && = delete;

private:
    std::shared_ptr<detail::TaskSharedState<void>> state_;
};

}  // namespace astra

#endif  // ASTRA_TASK_HANDLE_HPP

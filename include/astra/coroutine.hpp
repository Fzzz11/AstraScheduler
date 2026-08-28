#ifndef ASTRA_COROUTINE_HPP
#define ASTRA_COROUTINE_HPP

#include <astra/error.hpp>
#include <astra/export.hpp>
#include <astra/graph.hpp>
#include <astra/task_handle.hpp>

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace astra {

// 前向声明
template <typename T = void>
class Task;

namespace detail {

template <typename T>
struct TaskPromiseBase {
    std::shared_ptr<TaskSharedState<T>> shared_state{nullptr};

    std::suspend_always initial_suspend() noexcept {
        return {};
    }

    std::suspend_always final_suspend() noexcept {
        return {};
    }

    void unhandled_exception() {
        if (shared_state) {
            try {
                std::rethrow_exception(std::current_exception());
            } catch (const task_cancelled&) {
                shared_state->set_cancelled();
            } catch (...) {
                shared_state->set_exception(std::current_exception());
            }
        }
    }
};

}  // namespace detail

template <typename T = void>
class TaskPromise final : public detail::TaskPromiseBase<T> {
public:
    using value_type = T;

    Task<T> get_return_object() noexcept;

    template <typename U>
        requires std::convertible_to<U, T>
    void return_value(U&& val) {
        if (this->shared_state) {
            this->shared_state->set_value(std::forward<U>(val));
        }
    }
};

template <>
class TaskPromise<void> final : public detail::TaskPromiseBase<void> {
public:
    using value_type = void;

    Task<void> get_return_object() noexcept;

    void return_void() {
        if (this->shared_state) {
            this->shared_state->set_value();
        }
    }
};

// -----------------------------------------------------------------------------
// Task<T> (R-073 / D-114 / D-115)
// Cold C++20 Coroutine Task handle
// -----------------------------------------------------------------------------
template <typename T>
class Task {
public:
    using promise_type = TaskPromise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

    Task() noexcept = default;

    explicit Task(handle_type h) noexcept : coro_(h) {}

    ~Task() {
        if (coro_) {
            coro_.destroy();
            coro_ = nullptr;
        }
    }

    Task(Task&& other) noexcept : coro_(std::exchange(other.coro_, nullptr)) {}

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (coro_) {
                coro_.destroy();
            }
            coro_ = std::exchange(other.coro_, nullptr);
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(coro_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid();
    }

    [[nodiscard]] handle_type handle() const noexcept {
        return coro_;
    }

    [[nodiscard]] handle_type release_handle() noexcept {
        return std::exchange(coro_, nullptr);
    }

private:
    handle_type coro_{nullptr};
};

template <typename T>
inline Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

// -----------------------------------------------------------------------------
// AwaitHandshake (R-074 / R-075 / D-118 / D-119)
// -----------------------------------------------------------------------------
class AwaitHandshake {
public:
    enum class State : std::uint8_t {
        Init = 0,
        Triggered = 1,
        Armed = 2,
        Resolved = 3,
    };

    static constexpr std::uint8_t kStateMask = 0x0F;
    static constexpr std::uint8_t kCancelled = 0x80;

    AwaitHandshake() noexcept = default;

    template <typename PostAction>
    void trigger(PostAction&& post_action) {
        std::uint8_t expected = static_cast<std::uint8_t>(State::Init);
        if (raw_state_.compare_exchange_strong(expected, static_cast<std::uint8_t>(State::Triggered),
                                                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }

        if ((expected & kStateMask) == static_cast<std::uint8_t>(State::Armed)) {
            std::uint8_t new_state = static_cast<std::uint8_t>(State::Resolved) | (expected & kCancelled);
            if (raw_state_.compare_exchange_strong(expected, new_state,
                                                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                post_action();
            }
        }
    }

    template <typename PostAction>
    void trigger_cancel(PostAction&& post_action) {
        std::uint8_t current = raw_state_.load(std::memory_order_acquire);
        while (true) {
            if ((current & kStateMask) == static_cast<std::uint8_t>(State::Resolved)) {
                return;
            }
            if ((current & kStateMask) == static_cast<std::uint8_t>(State::Triggered)) {
                return;
            }
            if ((current & kStateMask) == static_cast<std::uint8_t>(State::Init)) {
                std::uint8_t next = static_cast<std::uint8_t>(State::Triggered) | kCancelled;
                if (raw_state_.compare_exchange_weak(current, next,
                                                      std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return;
                }
                continue;
            }
            if ((current & kStateMask) == static_cast<std::uint8_t>(State::Armed)) {
                std::uint8_t next = static_cast<std::uint8_t>(State::Resolved) | kCancelled;
                if (raw_state_.compare_exchange_weak(current, next,
                                                      std::memory_order_acq_rel, std::memory_order_acquire)) {
                    post_action();
                    return;
                }
                continue;
            }
            break;
        }
    }

    template <typename PostAction>
    void arm(PostAction&& post_action) {
        std::uint8_t expected = static_cast<std::uint8_t>(State::Init);
        if (raw_state_.compare_exchange_strong(expected, static_cast<std::uint8_t>(State::Armed),
                                                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }

        if ((expected & kStateMask) == static_cast<std::uint8_t>(State::Triggered)) {
            std::uint8_t new_state = static_cast<std::uint8_t>(State::Resolved) | (expected & kCancelled);
            if (raw_state_.compare_exchange_strong(expected, new_state,
                                                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                post_action();
            }
        }
    }

    [[nodiscard]] bool is_resolved() const noexcept {
        return (raw_state_.load(std::memory_order_acquire) & kStateMask) == static_cast<std::uint8_t>(State::Resolved);
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return (raw_state_.load(std::memory_order_acquire) & kCancelled) != 0;
    }

    [[nodiscard]] State state() const noexcept {
        return static_cast<State>(raw_state_.load(std::memory_order_acquire) & kStateMask);
    }

private:
    std::atomic<std::uint8_t> raw_state_{static_cast<std::uint8_t>(State::Init)};
};

namespace detail {

template <typename T>
class CoroutineTaskInvokerModel final : public TaskInvokerBase {
public:
    std::coroutine_handle<TaskPromise<T>> coro;
    std::shared_ptr<TaskSharedState<T>> state;

    CoroutineTaskInvokerModel(std::coroutine_handle<TaskPromise<T>> h,
                              std::shared_ptr<TaskSharedState<T>> s)
        : coro(h), state(std::move(s)) {}

    ~CoroutineTaskInvokerModel() override = default;

    void execute() override {
        if (!state->try_start()) {
            if (coro) {
                coro.destroy();
                coro = nullptr;
            }
            return;
        }

        TaskExecutionContextGuard guard(state->id());

        try {
            if (coro && !coro.done()) {
                coro.resume();
            }
        } catch (const task_cancelled&) {
            state->set_cancelled();
        } catch (...) {
            state->set_exception(std::current_exception());
        }

        if (coro && coro.done()) {
            coro.destroy();
            coro = nullptr;
        }
    }

    void cancel_pre_start() noexcept override {
        if (state) {
            state->request_cancel();
        }
    }
};

template <typename T>
class CoroutineResumeInvokerModel final : public TaskInvokerBase {
public:
    std::coroutine_handle<TaskPromise<T>> coro;
    std::shared_ptr<TaskSharedState<T>> state;

    CoroutineResumeInvokerModel(std::coroutine_handle<TaskPromise<T>> h,
                                std::shared_ptr<TaskSharedState<T>> s)
        : coro(h), state(std::move(s)) {}

    ~CoroutineResumeInvokerModel() override = default;

    void execute() override {
        state->transition_to_running();
        TaskExecutionContextGuard guard(state->id());

        try {
            if (coro && !coro.done()) {
                coro.resume();
            }
        } catch (const task_cancelled&) {
            state->set_cancelled();
        } catch (...) {
            state->set_exception(std::current_exception());
        }

        if (coro && coro.done()) {
            coro.destroy();
            coro = nullptr;
        }
    }

    void cancel_pre_start() noexcept override {
    }

    [[nodiscard]] bool is_resume_segment() const noexcept override {
        return true;
    }
};

class GraphCoroutineNodeInvoker final : public TaskInvokerBase {
public:
    std::coroutine_handle<TaskPromise<void>> coro;
    std::shared_ptr<TaskSharedState<void>> task_state{nullptr};

    explicit GraphCoroutineNodeInvoker(std::coroutine_handle<TaskPromise<void>> h)
        : coro(h) {}

    ~GraphCoroutineNodeInvoker() override {
        if (coro) {
            coro.destroy();
            coro = nullptr;
        }
    }

    void execute() override {}

    void cancel_pre_start() noexcept override {
        if (task_state) {
            task_state->request_cancel();
        }
    }

    [[nodiscard]] bool is_coroutine_node() const noexcept override {
        return true;
    }
};

// -----------------------------------------------------------------------------
// TaskHandle Awaiter (R-076 / D-120)
// -----------------------------------------------------------------------------
template <typename T>
struct TaskHandleAwaiter {
    TaskHandle<T> handle;
    std::shared_ptr<AwaitHandshake> handshake{std::make_shared<AwaitHandshake>()};
    std::optional<std::stop_callback<std::function<void()>>> stop_cb;

    explicit TaskHandleAwaiter(const TaskHandle<T>& h) : handle(h) {}

    bool await_ready() const {
        if (!handle.valid()) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        if (current_executing_task_id() == handle.task_id()) {
            throw std::logic_error("direct self-await detected (D-120 / R-076)");
        }
        const auto st = handle.state();
        return st == TaskState::Succeeded || st == TaskState::Failed || st == TaskState::Cancelled;
    }

    template <typename PromiseType>
    bool await_suspend(std::coroutine_handle<PromiseType> coro) {
        static_assert(requires { coro.promise().shared_state; },
                      "co_await TaskHandle is only permitted within astra::Task coroutines (D-120)");

        auto task_state = coro.promise().shared_state;
        if (!task_state) {
            throw std::logic_error("invalid coroutine shared_state");
        }

        if (task_state->id() == handle.task_id()) {
            throw std::logic_error("direct self-await detected (D-120 / R-076)");
        }

        if (task_state->stop_token().stop_requested()) {
            throw task_cancelled{};
        }

        auto rescheduler = task_state->get_rescheduler();

        auto post_action = [coro, task_state, rescheduler]() mutable {
            if (rescheduler) {
                auto invoker = std::make_unique<CoroutineResumeInvokerModel<typename PromiseType::value_type>>(
                    coro, std::move(task_state));
                rescheduler(std::move(invoker));
            }
        };

        auto hs = handshake;
        handle.shared_state_internal()->add_completion_callback([hs, post_action]() mutable {
            hs->trigger(post_action);
        });

        stop_cb.emplace(task_state->stop_token(), [hs, post_action]() mutable {
            hs->trigger_cancel(post_action);
        });

        task_state->transition_to_suspended();
        hs->arm(std::move(post_action));
        return true;
    }

    decltype(auto) await_resume() {
        stop_cb.reset();
        if (handshake->is_cancelled()) {
            throw task_cancelled{};
        }
        if constexpr (std::is_void_v<T>) {
            handle.get();
        } else {
            return handle.get();
        }
    }
};

// -----------------------------------------------------------------------------
// GraphRun Awaiter (R-076 / D-121)
// -----------------------------------------------------------------------------
struct GraphRunAwaiter {
    GraphRun run;
    std::shared_ptr<AwaitHandshake> handshake{std::make_shared<AwaitHandshake>()};
    std::optional<std::stop_callback<std::function<void()>>> stop_cb;

    explicit GraphRunAwaiter(const GraphRun& r) : run(r) {}

    bool await_ready() const {
        if (!run.valid()) {
            throw std::logic_error("operating on empty/moved-from GraphRun");
        }
        if (current_executing_graph_run_id() == run.id()) {
            throw std::logic_error("self-run await detected (D-121 / R-076)");
        }
        return run.is_completed();
    }

    template <typename PromiseType>
    bool await_suspend(std::coroutine_handle<PromiseType> coro) {
        static_assert(requires { coro.promise().shared_state; },
                      "co_await GraphRun is only permitted within astra::Task coroutines (D-121)");

        auto task_state = coro.promise().shared_state;
        if (!task_state) {
            throw std::logic_error("invalid coroutine shared_state");
        }

        if (current_executing_graph_run_id() == run.id()) {
            throw std::logic_error("self-run await detected (D-121 / R-076)");
        }

        if (task_state->stop_token().stop_requested()) {
            throw task_cancelled{};
        }

        auto rescheduler = task_state->get_rescheduler();

        auto post_action = [coro, task_state, rescheduler]() mutable {
            if (rescheduler) {
                auto invoker = std::make_unique<CoroutineResumeInvokerModel<typename PromiseType::value_type>>(
                    coro, std::move(task_state));
                rescheduler(std::move(invoker));
            }
        };

        auto hs = handshake;
        run.add_completion_callback_internal([hs, post_action]() mutable {
            hs->trigger(post_action);
        });

        stop_cb.emplace(task_state->stop_token(), [hs, post_action]() mutable {
            hs->trigger_cancel(post_action);
        });

        task_state->transition_to_suspended();
        hs->arm(std::move(post_action));
        return true;
    }

    const GraphReport& await_resume() {
        stop_cb.reset();
        if (handshake->is_cancelled()) {
            throw task_cancelled{};
        }
        return run.get_report();
    }
};

}  // namespace detail

template <typename T>
inline detail::TaskHandleAwaiter<T> TaskHandle<T>::operator co_await() const & {
    if (!state_) {
        throw std::logic_error("operating on empty/moved-from TaskHandle");
    }
    return detail::TaskHandleAwaiter<T>(*this);
}

inline detail::TaskHandleAwaiter<void> TaskHandle<void>::operator co_await() const & {
    if (!state_) {
        throw std::logic_error("operating on empty/moved-from TaskHandle");
    }
    return detail::TaskHandleAwaiter<void>(*this);
}

inline detail::GraphRunAwaiter GraphRun::operator co_await() const & {
    if (!state_) {
        throw std::logic_error("operating on empty/moved-from GraphRun");
    }
    return detail::GraphRunAwaiter(*this);
}

// -----------------------------------------------------------------------------
// cancellation_point (R-076 / D-122)
// -----------------------------------------------------------------------------
struct CancellationPointAwaiter {
    constexpr bool await_ready() const noexcept {
        return false;
    }

    template <typename PromiseType>
    bool await_suspend(std::coroutine_handle<PromiseType> coro) const {
        static_assert(requires { coro.promise().shared_state; },
                      "cancellation_point is only permitted within astra::Task coroutines (D-122)");
        auto task_state = coro.promise().shared_state;
        if (task_state && task_state->stop_token().stop_requested()) {
            throw task_cancelled{};
        }
        return false;
    }

    constexpr void await_resume() const noexcept {}
};

[[nodiscard]] inline CancellationPointAwaiter cancellation_point() noexcept {
    return CancellationPointAwaiter{};
}

// -----------------------------------------------------------------------------
// yield (R-076 / D-122 / D-147)
// -----------------------------------------------------------------------------
struct YieldAwaiter {
    constexpr bool await_ready() const noexcept {
        return false;
    }

    template <typename PromiseType>
    bool await_suspend(std::coroutine_handle<PromiseType> coro) {
        static_assert(requires { coro.promise().shared_state; },
                      "yield is only permitted within astra::Task coroutines (D-122)");

        auto task_state = coro.promise().shared_state;
        if (!task_state) {
            throw std::logic_error("invalid coroutine shared_state");
        }

        if (task_state->stop_token().stop_requested()) {
            throw task_cancelled{};
        }

        task_state->transition_to_suspended();

        auto rescheduler = task_state->get_rescheduler();
        if (rescheduler) {
            auto invoker = std::make_unique<detail::CoroutineResumeInvokerModel<typename PromiseType::value_type>>(
                coro, std::move(task_state));
            rescheduler(std::move(invoker));
        }

        return true;
    }

    constexpr void await_resume() const noexcept {}
};

[[nodiscard]] inline YieldAwaiter yield() noexcept {
    return YieldAwaiter{};
}

// -----------------------------------------------------------------------------
// TaskGraph::emplace_coroutine (R-077 / D-123)
// -----------------------------------------------------------------------------
inline NodeId TaskGraph::emplace_coroutine(Task<void>&& task) {
    if (!task.valid()) {
        throw std::logic_error("cannot emplace empty/invalid Task<void> into TaskGraph");
    }
    const std::uint64_t seq = nodes_.size() + 1;
    const NodeId id{seq};
    nodes_.push_back(FrozenTaskGraph::NodeData{
        id,
        std::make_unique<detail::GraphCoroutineNodeInvoker>(task.release_handle())
    });
    return id;
}

}  // namespace astra

#endif  // ASTRA_COROUTINE_HPP

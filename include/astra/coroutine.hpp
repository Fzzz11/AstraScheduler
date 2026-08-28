#ifndef ASTRA_COROUTINE_HPP
#define ASTRA_COROUTINE_HPP

#include <astra/error.hpp>
#include <astra/export.hpp>
#include <astra/task_handle.hpp>

#include <coroutine>
#include <exception>
#include <memory>
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

template <typename T>
struct TaskPromise : TaskPromiseBase<T> {
    using value_type = T;

    Task<T> get_return_object() noexcept;

    template <typename U>
    void return_value(U&& value) {
        if (this->shared_state) {
            this->shared_state->set_value(std::forward<U>(value));
        }
    }
};

template <>
struct TaskPromise<void> : TaskPromiseBase<void> {
    using value_type = void;

    Task<void> get_return_object() noexcept;

    void return_void() {
        if (this->shared_state) {
            this->shared_state->set_value();
        }
    }
};

}  // namespace detail

// -----------------------------------------------------------------------------
// astra::Task<T> (R-073 / D-114 / D-115)
// cold、move-only、single-shot coroutine frame owner
// -----------------------------------------------------------------------------
template <typename T>
class [[nodiscard]] Task {
public:
    static_assert(!std::is_reference_v<T>,
        "Task<T> does not support raw reference types (R-058 / D-074 / D-114)");
    static_assert(std::is_move_constructible_v<T> || std::is_void_v<T>,
        "Task<T> result type must be move-constructible or void (R-058 / D-075 / D-114)");

    using promise_type = detail::TaskPromise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

    Task() noexcept = default;

    explicit Task(handle_type h) noexcept : handle_(h) {}

    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(handle_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid();
    }

    [[nodiscard]] handle_type handle() const noexcept {
        return handle_;
    }

    [[nodiscard]] handle_type release_handle() noexcept {
        handle_type h = handle_;
        handle_ = nullptr;
        return h;
    }

private:
    handle_type handle_{nullptr};
};

// -----------------------------------------------------------------------------
// AwaitHandshake (R-074 / R-075 / D-118 / D-119)
// 线性化内建 awaiter 的挂起与触发/取消唤醒竞争，保证恰好发布一个 Ready ticket，
// 且不在 await_suspend 返回前发生并发 resume。
// -----------------------------------------------------------------------------
class AwaitHandshake {
public:
    enum Flag : std::uint32_t {
        kInit = 0,
        kTriggered = 1 << 0,
        kArmed = 1 << 1,
        kResolved = 1 << 2,
        kCancelled = 1 << 3
    };

    AwaitHandshake() noexcept = default;

    // 目标完成时触发（可在 await_suspend 注册期间或之后发生）
    template <typename PostFn>
    void trigger(PostFn&& post_fn) {
        const std::uint32_t prev = state_.fetch_or(kTriggered, std::memory_order_acq_rel);
        if ((prev & kArmed) != 0 && (prev & kResolved) == 0) {
            std::uint32_t expected = prev | kTriggered;
            if (state_.compare_exchange_strong(expected, expected | kResolved, std::memory_order_acq_rel)) {
                post_fn();
            }
        }
    }

    // 收到取消/stop 信号时触发（R-075 / D-119）
    template <typename PostFn>
    void trigger_cancel(PostFn&& post_fn) {
        std::uint32_t current = state_.load(std::memory_order_acquire);
        while ((current & (kTriggered | kResolved)) == 0) {
            std::uint32_t next = current | kTriggered | kCancelled;
            if (current & kArmed) {
                next |= kResolved;
            }
            if (state_.compare_exchange_weak(current, next, std::memory_order_acq_rel)) {
                if (current & kArmed) {
                    post_fn();
                }
                return;
            }
        }
    }

    // await_suspend 完成注册并提交 Suspended 状态后 arm
    template <typename PostFn>
    void arm(PostFn&& post_fn) {
        const std::uint32_t prev = state_.fetch_or(kArmed, std::memory_order_acq_rel);
        if ((prev & kTriggered) != 0 && (prev & kResolved) == 0) {
            std::uint32_t expected = prev | kArmed;
            if (state_.compare_exchange_strong(expected, expected | kResolved, std::memory_order_acq_rel)) {
                post_fn();
            }
        }
    }

    [[nodiscard]] bool is_triggered() const noexcept {
        return (state_.load(std::memory_order_acquire) & kTriggered) != 0;
    }

    [[nodiscard]] bool is_armed() const noexcept {
        return (state_.load(std::memory_order_acquire) & kArmed) != 0;
    }

    [[nodiscard]] bool is_resolved() const noexcept {
        return (state_.load(std::memory_order_acquire) & kResolved) != 0;
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return (state_.load(std::memory_order_acquire) & kCancelled) != 0;
    }

private:
    std::atomic<std::uint32_t> state_{kInit};
};

namespace detail {

template <typename T>
inline Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

// 首次启动 Coroutine Task Invoker（D-114 / D-115 / D-116 / D-154）
template <typename T>
struct CoroutineTaskInvokerModel final : TaskInvokerBase {
    std::coroutine_handle<TaskPromise<T>> coro;
    std::shared_ptr<TaskSharedState<T>> state;
    bool executed{false};

    explicit CoroutineTaskInvokerModel(
        std::coroutine_handle<TaskPromise<T>> h,
        std::shared_ptr<TaskSharedState<T>> st) noexcept
        : coro(h), state(std::move(st)) {}

    ~CoroutineTaskInvokerModel() override {
        if (!executed && coro && state && state->is_completed()) {
            coro.destroy();
            coro = nullptr;
        }
    }

    void execute() override {
        executed = true;
        // 首次 start 竞争（R-053 / D-052）
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

    [[nodiscard]] bool is_resume_segment() const noexcept override {
        return false;
    }
};

// 后续恢复 Coroutine Resume Invoker（R-074 / R-075 / D-116 / D-117 / D-118 / D-154）
template <typename T>
struct CoroutineResumeInvokerModel final : TaskInvokerBase {
    std::coroutine_handle<TaskPromise<T>> coro;
    std::shared_ptr<TaskSharedState<T>> state;

    explicit CoroutineResumeInvokerModel(
        std::coroutine_handle<TaskPromise<T>> h,
        std::shared_ptr<TaskSharedState<T>> st) noexcept
        : coro(h), state(std::move(st)) {}

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
        // R-075 / D-154: Resume segment 不属于 never-started 任务
    }

    [[nodiscard]] bool is_resume_segment() const noexcept override {
        return true;
    }
};

}  // namespace detail

}  // namespace astra

#endif  // ASTRA_COROUTINE_HPP

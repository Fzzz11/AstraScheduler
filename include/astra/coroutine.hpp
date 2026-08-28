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
            shared_state->set_exception(std::current_exception());
        }
    }
};

template <typename T>
struct TaskPromise : TaskPromiseBase<T> {
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

namespace detail {

template <typename T>
inline Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

// Coroutine Task Invoker（D-114 / D-115）
template <typename T>
struct CoroutineTaskInvokerModel final : TaskInvokerBase {
    std::coroutine_handle<TaskPromise<T>> coro;
    std::shared_ptr<TaskSharedState<T>> state;

    explicit CoroutineTaskInvokerModel(
        std::coroutine_handle<TaskPromise<T>> h,
        std::shared_ptr<TaskSharedState<T>> st) noexcept
        : coro(h), state(std::move(st)) {}

    ~CoroutineTaskInvokerModel() override {
        if (coro) {
            coro.destroy();
            coro = nullptr;
        }
    }

    void execute() override {
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
};

}  // namespace detail

}  // namespace astra

#endif  // ASTRA_COROUTINE_HPP

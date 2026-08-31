#ifndef ASTRA_COROUTINE_HPP
#define ASTRA_COROUTINE_HPP

#include <astra/error.hpp>
#include <astra/export.hpp>
#include <astra/graph.hpp>
#include <astra/task_handle.hpp>

#include <chrono>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

// ============================================================================
// Task<T> 协程 —— 用 co_await/co_return 编写异步任务。
//
// 【这是什么】
//   `co_return` 的函数就是一个 Astra 协程：用 sched.spawn(coro(sched))
//   提交后，它在 worker 线程上执行。协程可以在执行中途"挂起"（把 CPU
//   让给别的任务），等条件满足后从挂起点继续：
//     co_await astra::yield();                    // 主动让出一次
//     co_await astra::sleep_for(100ms);           // 定时唤醒
//     co_await some_task_handle;                  // 等另一个任务完成并取其结果
//     co_await graph_run;                         // 等一个任务图跑完
//
// 【为什么协程比回调好（以及帧在哪里）】
//   编译器把局部变量与恢复点自动保存进"协程帧"（堆上的一块内存），
//   挂起/恢复不需要你手写状态机。帧的所有权规则很严格：
//   恰好由一个 invoker 对象持有，谁最后执行完协程谁销毁帧——
//   挂起时所有权会移交给"恢复者"（resume invoker），避免两方同时
//   拥有或销毁同一帧（这是并发正确性的关键，见 TaskControlBlock 的
//   resume_handoff_seq 代际标记）。
//
// 【等待别人的三种路径】
//   - co_await TaskHandle：目标完成/取消时被唤醒（AwaitHandshake 仲裁，
//     保证"先触发"与"先挂起"无论谁先到都恰好执行一次恢复）。
//   - co_await yield/sleep：重新排队或定时唤醒。
//   - 在 worker 线程里同步 get()/wait() 另一个任务：worker 会边等边
//     执行其他任务（helping），不会白白占着线程。
//
// 【取消】
//   停止信号到达挂起点时，协程体内抛出 task_cancelled 异常——用
//   try/catch 或 RAII 做清理即可，与普通异常处理一致。
//
// 【注意】
//   协程体内不要阻塞（长 sleep/锁等待）：线程被占住，其他任务会饿死。
//   需要等待就用上面的挂起原语。
// ============================================================================

namespace astra {

// 前向声明
class Scheduler;
class TaskGraph;
template <typename T = void>
class Task;

namespace detail {

template <typename T>
struct TaskPromiseBase {
    std::shared_ptr<typename TaskHandle<T>::ResultCell> shared_state{nullptr};

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

/** @brief 非 void Astra 协程的 promise 类型。 */
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

/** @brief void Astra 协程的 promise 类型。 */
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

// Cold 协程句柄：函数写 co_return 即得到 Task<T>，须再 spawn/emplace_coroutine 才会执行。
// 仅可移动；未提交就销毁会销毁协程帧。复制被 delete（R-073 / D-114）。
/**
 * @brief 尚未提交执行的 cold 协程句柄。
 * @tparam T 协程返回值类型。
 * @note Task 仅可移动；需交给 Scheduler::spawn() 才会开始执行。
 */
template <typename T>
class Task {
public:
    using promise_type = TaskPromise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

    Task() noexcept = default;

    explicit Task(handle_type h) noexcept : coro_(h) {}

    // 未 spawn 的有效 Task 在析构时销毁协程帧。
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

    // 是否持有协程帧。空/moved-from 返回 false，不抛。
    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(coro_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid();
    }

private:
    friend class Scheduler;
    friend class TaskGraph;

    [[nodiscard]] handle_type handle() const noexcept {
        return coro_;
    }

    [[nodiscard]] handle_type release_handle() noexcept {
        return std::exchange(coro_, nullptr);
    }

    handle_type coro_{nullptr};
};

template <typename T>
inline Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

namespace detail {

ASTRA_EXPORT std::shared_ptr<void> tcb_arm_task_await(
    std::shared_ptr<void> waiter,
    std::shared_ptr<void> target,
    std::coroutine_handle<> coro);
ASTRA_EXPORT std::shared_ptr<void> tcb_arm_graph_await(
    std::shared_ptr<void> waiter,
    GraphRun& run,
    std::coroutine_handle<> coro);
ASTRA_EXPORT void tcb_arm_yield(std::shared_ptr<void> waiter, std::coroutine_handle<> coro);
ASTRA_EXPORT std::shared_ptr<void> tcb_arm_sleep(
    std::shared_ptr<void> waiter,
    std::chrono::steady_clock::time_point wake_time,
    std::coroutine_handle<> coro);
ASTRA_EXPORT void tcb_finish_await(std::shared_ptr<void>& token, TaskId target);
ASTRA_EXPORT bool tcb_await_cancelled(const std::shared_ptr<void>& token) noexcept;

template <typename T>
class CoroutineTaskInvokerModel final : public TaskInvokerBase {
public:
    std::coroutine_handle<TaskPromise<T>> coro;
    std::shared_ptr<typename TaskHandle<T>::ResultCell> state;

    CoroutineTaskInvokerModel(std::coroutine_handle<TaskPromise<T>> h,
                              std::shared_ptr<typename TaskHandle<T>::ResultCell> s)
        : coro(h), state(std::move(s)) {}

    ~CoroutineTaskInvokerModel() override = default;

    void execute() override {
        const std::uint64_t handoff_seq_before = state->resume_handoff_seq();
        TaskExecutionContextGuard guard(state->id(), state->priority());
        try {
            if (coro && !coro.done()) {
                coro.resume();
            }
        } catch (const task_cancelled&) {
            state->set_cancelled();
        } catch (...) {
            state->set_exception(std::current_exception());
        }

        if (state->resume_handoff_seq() != handoff_seq_before) {
            coro = nullptr;
        } else if (coro && coro.done()) {
            coro.destroy();
            coro = nullptr;
        }
    }

    void cancel_pre_start() noexcept override {
        if (state) {
            state->request_cancel();
        }
    }

    void abandon_unstarted() noexcept override {
        if (coro) {
            coro.destroy();
            coro = nullptr;
        }
    }

    [[nodiscard]] Priority priority() const noexcept override {
        return state ? state->priority() : Priority::Normal;
    }

    [[nodiscard]] std::optional<TaskDeadline> deadline() const noexcept override {
        return state ? state->deadline() : std::nullopt;
    }
};

class GraphCoroutineNodeInvoker final : public TaskInvokerBase {
public:
    std::coroutine_handle<TaskPromise<void>> coro;
    std::shared_ptr<typename TaskHandle<void>::ResultCell> task_state{nullptr};

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

    [[nodiscard]] Priority priority() const noexcept override {
        return task_state ? task_state->priority() : Priority::Normal;
    }

    [[nodiscard]] std::optional<TaskDeadline> deadline() const noexcept override {
        return task_state ? task_state->deadline() : std::nullopt;
    }
};

// -----------------------------------------------------------------------------
// TaskHandle Awaiter (R-076 / D-120)
// -----------------------------------------------------------------------------
template <typename T>
struct TaskHandleAwaiter {
    TaskHandle<T> handle;
    std::shared_ptr<void> token;

    explicit TaskHandleAwaiter(const TaskHandle<T>& h) : handle(h) {}

    bool await_ready() const {
        if (!handle.valid()) {
            throw std::logic_error("operating on empty/moved-from TaskHandle");
        }
        if (current_executing_task_id() == handle.task_id()) {
            record_self_wait_rejection(handle.task_id());
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
            record_self_wait_rejection(handle.task_id());
            throw std::logic_error("direct self-await detected (D-120 / R-076)");
        }

        if (task_state->stop_token().stop_requested()) {
            throw task_cancelled{};
        }

        token = tcb_arm_task_await(
            task_state->protocol_token(),
            handle.shared_state_internal()->protocol_token(),
            std::coroutine_handle<>(coro));
        return true;
    }

    decltype(auto) await_resume() {
        tcb_finish_await(token, handle.task_id());
        if (tcb_await_cancelled(token)) {
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
    std::shared_ptr<void> token;

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

        token = tcb_arm_graph_await(
            task_state->protocol_token(),
            run,
            std::coroutine_handle<>(coro));
        return true;
    }

    const GraphReport& await_resume() {
        tcb_finish_await(token, TaskId{});
        if (tcb_await_cancelled(token)) {
            throw task_cancelled{};
        }
        return run.get_report();
    }
};

}  // namespace detail

template <typename T>
inline detail::TaskHandleAwaiter<T> TaskHandle<T>::operator co_await() const & {
    if (!cell_) {
        throw std::logic_error("operating on empty/moved-from TaskHandle");
    }
    return detail::TaskHandleAwaiter<T>(*this);
}

inline detail::TaskHandleAwaiter<void> TaskHandle<void>::operator co_await() const & {
    if (!cell_) {
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

// 在 Astra 协程内检查取消：已请求停止则抛 task_cancelled，否则立即继续。
// 只能 co_await；普通函数请用 throw_if_stop_requested（R-076 / D-122）。
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

        detail::tcb_arm_yield(task_state->protocol_token(), std::coroutine_handle<>(coro));
        return true;
    }

    constexpr void await_resume() const noexcept {}
};

// 主动让出当前 Worker，稍后再入队。只能在 astra::Task 协程内 co_await。
// 挂起点若已取消则抛 task_cancelled。协程体外使用无法通过编译（R-076 / D-122）。
[[nodiscard]] inline YieldAwaiter yield() noexcept {
    return YieldAwaiter{};
}

// -----------------------------------------------------------------------------
// sleep_until / sleep_for (R-079 / D-126 / D-127 / D-128)
// -----------------------------------------------------------------------------
struct SleepAwaiter {
    std::chrono::steady_clock::time_point wake_time;
    std::shared_ptr<void> token;

    explicit SleepAwaiter(std::chrono::steady_clock::time_point wt) noexcept
        : wake_time(wt) {}

    constexpr bool await_ready() const noexcept {
        return false;
    }

    template <typename PromiseType>
    bool await_suspend(std::coroutine_handle<PromiseType> coro) {
        static_assert(requires { coro.promise().shared_state; },
                      "sleep is only permitted within astra::Task coroutines (D-126)");

        auto task_state = coro.promise().shared_state;
        if (!task_state) {
            throw std::logic_error("invalid coroutine shared_state");
        }

        if (task_state->stop_token().stop_requested()) {
            throw task_cancelled{};
        }

        if (wake_time <= std::chrono::steady_clock::now()) {
            return false;
        }

        token = detail::tcb_arm_sleep(
            task_state->protocol_token(),
            wake_time,
            std::coroutine_handle<>(coro));
        return true;
    }

    void await_resume() {
        detail::tcb_finish_await(token, TaskId{});
        if (detail::tcb_await_cancelled(token)) {
            throw task_cancelled{};
        }
    }
};

// 挂起到绝对时刻。只能在已 spawn 的 astra::Task 协程内 co_await。
// 到期前取消则 await 点抛 task_cancelled。未绑定 Runtime 抛 logic_error（R-079）。
[[nodiscard]] inline SleepAwaiter sleep_until(std::chrono::steady_clock::time_point wake_time) noexcept {
    return SleepAwaiter(wake_time);
}

// 相对 now 挂起；非正时长视为立即到期。取消 / 线程约束同 sleep_until()（R-079）。
template <typename Rep, typename Period>
[[nodiscard]] inline SleepAwaiter sleep_for(const std::chrono::duration<Rep, Period>& d) {
    if (d <= std::chrono::duration<Rep, Period>::zero()) {
        return SleepAwaiter(std::chrono::steady_clock::now() + d);
    }
    const auto now = std::chrono::steady_clock::now();
    const auto max_tp = std::chrono::steady_clock::time_point::max();
    const auto max_dur = max_tp - now;
    auto d_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d);
    if (d_ns > max_dur) {
        return SleepAwaiter(max_tp);
    }
    return SleepAwaiter(now + d_ns);
}

// -----------------------------------------------------------------------------
// TaskGraph::emplace_coroutine (R-077 / R-080 / D-123 / D-129)
// -----------------------------------------------------------------------------
inline NodeId TaskGraph::emplace_coroutine(Task<void>&& task) {
    if (!task.valid()) {
        throw std::logic_error("cannot emplace empty/invalid Task<void> into TaskGraph");
    }
    const std::uint64_t seq = nodes_.size() + 1;
    const NodeId id{seq};
    nodes_.push_back(FrozenTaskGraph::NodeData{
        id,
        std::make_unique<detail::GraphCoroutineNodeInvoker>(task.release_handle()),
        std::nullopt
    });
    return id;
}

inline NodeId TaskGraph::emplace_coroutine(TaskOptions options, Task<void>&& task) {
    validate_priority(options.priority);
    if (!task.valid()) {
        throw std::logic_error("cannot emplace empty/invalid Task<void> into TaskGraph");
    }
    const std::uint64_t seq = nodes_.size() + 1;
    const NodeId id{seq};
    nodes_.push_back(FrozenTaskGraph::NodeData{
        id,
        std::make_unique<detail::GraphCoroutineNodeInvoker>(task.release_handle()),
        options
    });
    return id;
}

}  // namespace astra

#endif  // ASTRA_COROUTINE_HPP

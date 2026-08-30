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
//   拥有或销毁同一帧（这是并发正确性的关键，见 TaskSharedState 的
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

// Cold 协程句柄：函数写 co_return 即得到 Task<T>，须再 spawn/emplace_coroutine 才会执行。
// 仅可移动；未提交就销毁会销毁协程帧。复制被 delete（R-073 / D-114）。
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

// -----------------------------------------------------------------------------
// AwaitHandshake —— "先 arm 还是先 trigger"的两方竞态仲裁器。
//
// 【解决什么问题】
//   协程 A 挂起等待任务 B 时，要注册一个"B 完成后唤醒我"的回调。
//   两个事件可能以任意顺序发生：B 先完成（回调先触发，此时 A 还没挂起），
//   或 A 先挂起（回调后触发）。这个握手对象用 4 状态原子状态机保证
//   无论顺序如何，唤醒动作恰好执行一次：
//     Init --arm--> Armed --trigger--> Resolved（执行回调）
//     Init --trigger--> Triggered --arm--> Resolved（立即执行回调）
//   trigger_cancel 同理，但唤醒时协程会收到取消异常。
//
// 【使用纪律】
//   arm/trigger 都可以带"恢复动作"（把协程重新排队的函数）；谁观察到
//   状态从非 Resolved 迁到 Resolved，谁负责执行它。已 Resolved 再触发
//   是无操作——幂等是并发安全的根基。
// -----------------------------------------------------------------------------
namespace detail {

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

        // AST-056: 记录 resume 前的 handoff 代际; resume 期间若发生所有权移交
        // (yield/sleep/await 挂起并 requeue), 帧归 resume invoker 所有,
        // 本 invoker 不得再读取 done() 或销毁帧。
        const std::uint64_t handoff_seq_before = state->resume_handoff_seq();
        TaskExecutionContextGuard guard(state->id(), state->priority());
        const auto t_start = std::chrono::steady_clock::now();
        try {
            if (coro && !coro.done()) {
                coro.resume();
            }
            const auto t_end = std::chrono::steady_clock::now();
            record_metrics_execution_segment(state->id(), std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
        } catch (const task_cancelled&) {
            const auto t_end = std::chrono::steady_clock::now();
            record_metrics_execution_segment(state->id(), std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
            state->set_cancelled();
        } catch (...) {
            const auto t_end = std::chrono::steady_clock::now();
            record_metrics_execution_segment(state->id(), std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
            state->set_exception(std::current_exception());
        }

        if (state->resume_handoff_seq() != handoff_seq_before) {
            coro = nullptr;  // 所有权已移交: 绝不触碰帧
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

    [[nodiscard]] Priority priority() const noexcept override {
        return state ? state->priority() : Priority::Normal;
    }

    [[nodiscard]] std::optional<TaskDeadline> deadline() const noexcept override {
        return state ? state->deadline() : std::nullopt;
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
        const auto now = std::chrono::steady_clock::now();
        const auto pub = state->ready_published_at();
        if (now >= pub && pub != std::chrono::steady_clock::time_point{}) {
            record_metrics_ready_queue_wait(state->id(), std::chrono::duration_cast<std::chrono::nanoseconds>(now - pub).count());
        }
        record_metrics_resume_segment(state->id());
        // AST-056: 与 CoroutineTaskInvokerModel 相同的 handoff 代际裁决。
        const std::uint64_t handoff_seq_before = state->resume_handoff_seq();
        TaskExecutionContextGuard guard(state->id(), state->priority());
        const auto t_start = std::chrono::steady_clock::now();

        try {
            if (coro && !coro.done()) {
                coro.resume();
            }
            const auto t_end = std::chrono::steady_clock::now();
            record_metrics_execution_segment(state->id(), std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
        } catch (const task_cancelled&) {
            const auto t_end = std::chrono::steady_clock::now();
            record_metrics_execution_segment(state->id(), std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
            state->set_cancelled();
        } catch (...) {
            const auto t_end = std::chrono::steady_clock::now();
            record_metrics_execution_segment(state->id(), std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
            state->set_exception(std::current_exception());
        }

        if (state->resume_handoff_seq() != handoff_seq_before) {
            coro = nullptr;  // 所有权已移交: 绝不触碰帧
        } else if (coro && coro.done()) {
            coro.destroy();
            coro = nullptr;
        }
    }

    void cancel_pre_start() noexcept override {
    }

    [[nodiscard]] bool is_resume_segment() const noexcept override {
        return true;
    }

    [[nodiscard]] Priority priority() const noexcept override {
        return state ? state->priority() : Priority::Normal;
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
    std::shared_ptr<AwaitHandshake> handshake{std::make_shared<AwaitHandshake>()};
    std::optional<std::stop_callback<std::function<void()>>> stop_cb;
    // AST-048 / R-096：await 诊断的 source identity 与 arm 时间戳。
    TaskId source_id{};
    std::chrono::steady_clock::time_point armed_at{};

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

        auto rescheduler = task_state->get_rescheduler();

        auto post_action = [coro, task_state, rescheduler]() mutable {
            if (rescheduler) {
                task_state->set_ready_published_at(std::chrono::steady_clock::now());
                auto invoker = std::make_unique<CoroutineResumeInvokerModel<typename PromiseType::value_type>>(
                    coro, std::move(task_state));
                rescheduler(std::move(invoker));
            }
        };

        source_id = task_state->id();
        armed_at = std::chrono::steady_clock::now();

        // AST-056: 完成回调可能在任意线程立即触发 requeue, 先移交帧所有权。
        task_state->mark_resume_handoff();
        auto hs = handshake;
        handle.shared_state_internal()->add_completion_callback(
            [hs, post_action, src = source_id, tgt = handle.task_id()]() mutable {
                record_await_triggered(src, tgt, false);
                hs->trigger(post_action);
            });

        stop_cb.emplace(task_state->stop_token(), [hs, post_action, src = source_id, tgt = handle.task_id()]() mutable {
            record_await_triggered(src, tgt, true);
            hs->trigger_cancel(post_action);
        });

        task_state->transition_to_suspended();
        hs->arm(std::move(post_action));
        // R-096 / D-149：await 注册计数与 AwaitArmed trace 事件（source/target identity）。
        record_await_registration(source_id, handle.task_id());
        return true;
    }

    decltype(auto) await_resume() {
        stop_cb.reset();
        // R-096 / D-149：AwaitResumed 与 await 时长 histogram（arm → resume）。
        if (source_id.valid()) {
            const auto dur_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - armed_at).count());
            record_await_resumed(source_id, handle.task_id(), dur_ns);
        }
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
                task_state->set_ready_published_at(std::chrono::steady_clock::now());
                auto invoker = std::make_unique<CoroutineResumeInvokerModel<typename PromiseType::value_type>>(
                    coro, std::move(task_state));
                rescheduler(std::move(invoker));
            }
        };

        // AST-056: 完成回调可能在任意线程立即触发 requeue, 先移交帧所有权。
        task_state->mark_resume_handoff();
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

        task_state->transition_to_suspended();
        detail::record_metrics_explicit_yield();

        auto rescheduler = task_state->get_rescheduler();
        if (rescheduler) {
            // AST-056: 帧所有权移交 resume invoker; 挂起方此后不得触碰帧。
            task_state->mark_resume_handoff();
            task_state->set_ready_published_at(std::chrono::steady_clock::now());
            auto invoker = std::make_unique<detail::CoroutineResumeInvokerModel<typename PromiseType::value_type>>(
                coro, std::move(task_state));
            rescheduler(std::move(invoker));
        }

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
    std::shared_ptr<detail::AwaitHandshake> handshake{nullptr};
    std::optional<std::stop_callback<std::function<void()>>> stop_cb;

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

        auto rescheduler = task_state->get_rescheduler();
        auto registrar = task_state->get_timer_registrar();
        auto canceller = task_state->get_timer_canceller();

        if (!rescheduler || !registrar || !canceller) {
            throw std::logic_error("cannot sleep outside an AstraScheduler coroutine runtime");
        }

        task_state->transition_to_suspended();

        handshake = std::make_shared<detail::AwaitHandshake>();

        struct RegistrationContext {
            std::atomic<std::uint64_t> timer_id{0};
            std::atomic<bool> cancelled{false};
        };
        auto ctx = std::make_shared<RegistrationContext>();

        stop_cb.emplace(task_state->stop_token(), [handshake = this->handshake, coro, task_state, rescheduler, canceller, ctx]() mutable {
            ctx->cancelled.store(true, std::memory_order_release);
            std::uint64_t tid = ctx->timer_id.load(std::memory_order_acquire);
            if (tid != 0 && canceller) {
                canceller(tid);
            }
            handshake->trigger_cancel([coro, task_state, rescheduler]() mutable {
                if (rescheduler) {
                    task_state->set_ready_published_at(std::chrono::steady_clock::now());
                    auto invoker = std::make_unique<detail::CoroutineResumeInvokerModel<typename PromiseType::value_type>>(
                        coro, std::move(task_state));
                    rescheduler(std::move(invoker));
                }
            });
        });

        if (task_state->stop_token().stop_requested()) {
            stop_cb.reset();
            throw task_cancelled{};
        }

        auto on_expiry = [coro, task_state, rescheduler]() mutable {
            if (rescheduler) {
                task_state->set_ready_published_at(std::chrono::steady_clock::now());
                auto invoker = std::make_unique<detail::CoroutineResumeInvokerModel<typename PromiseType::value_type>>(
                    coro, std::move(task_state));
                rescheduler(std::move(invoker));
            }
        };

        // AST-056: 定时器 resume 路径移交帧所有权。
        task_state->mark_resume_handoff();
        std::uint64_t tid = registrar(wake_time, handshake, on_expiry);
        ctx->timer_id.store(tid, std::memory_order_release);
        if (ctx->cancelled.load(std::memory_order_acquire) || task_state->stop_token().stop_requested()) {
            canceller(tid);
        }

        handshake->arm(on_expiry);
        return true;
    }

    void await_resume() {
        stop_cb.reset();
        if (handshake && handshake->is_cancelled()) {
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

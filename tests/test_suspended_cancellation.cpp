#include "task/await_handshake.hpp"
#include "task/coroutine_resume.hpp"

#include "astra/coroutine.hpp"
#include "astra/scheduler.hpp"
#include "astra/task_handle.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace {

// -----------------------------------------------------------------------------
// Astra Cancellation-Aware Awaiter (R-075 / D-119)
// -----------------------------------------------------------------------------
struct CancellationAwareEvent {
    struct State {
        astra::detail::AwaitHandshake handshake;
        std::atomic<bool> completed{false};
        std::function<void()> trigger_fn;
        std::function<void()> cancel_fn;
        std::mutex mtx;

        void signal() {
            std::function<void()> fn;
            {
                std::lock_guard<std::mutex> lk(mtx);
                completed.store(true, std::memory_order_release);
                fn = std::move(trigger_fn);
            }
            if (fn) {
                fn();
            }
        }

        void request_cancel_trigger() {
            std::function<void()> fn;
            {
                std::lock_guard<std::mutex> lk(mtx);
                fn = std::move(cancel_fn);
            }
            if (fn) {
                fn();
            }
        }
    };

    std::shared_ptr<State> state{std::make_shared<State>()};

    void signal() {
        if (state) {
            state->signal();
        }
    }

    struct Awaiter {
        std::shared_ptr<State> st;
        std::optional<std::stop_callback<std::function<void()>>> stop_cb;

        bool await_ready() const noexcept {
            return st->completed.load(std::memory_order_acquire);
        }

        template <typename PromiseType>
        bool await_suspend(std::coroutine_handle<PromiseType> coro) {
            auto task_state = coro.promise().shared_state;
            auto rescheduler = task_state->get_rescheduler();

            auto post_action = [coro, task_state, rescheduler]() mutable {
                if (rescheduler) {
                    auto invoker = std::make_unique<astra::detail::CoroutineResumeInvokerModel<typename PromiseType::value_type>>(
                        coro, std::move(task_state));
                    rescheduler(std::move(invoker));
                }
            };

            {
                std::lock_guard<std::mutex> lk(st->mtx);
                st->trigger_fn = [&hs = st->handshake, post_action]() mutable {
                    hs.trigger(post_action);
                };
                st->cancel_fn = [&hs = st->handshake, post_action]() mutable {
                    hs.trigger_cancel(post_action);
                };
            }

            // 注册 stop_callback 竞争 handshake winner
            stop_cb.emplace(task_state->stop_token(), [st_ptr = st.get()]() {
                st_ptr->request_cancel_trigger();
            });

            task_state->transition_to_suspended();

            st->handshake.arm(std::move(post_action));
            return true;
        }

        void await_resume() {
            stop_cb.reset();
            if (st->handshake.is_cancelled()) {
                throw astra::task_cancelled{};
            }
        }
    };

    Awaiter operator co_await() const {
        return Awaiter{state, std::nullopt};
    }
};

// -----------------------------------------------------------------------------
// Foreign Awaitable（不响应 stop_callback，按 D-119 仅保留 stop request）
// -----------------------------------------------------------------------------
struct ForeignEvent {
    struct State {
        astra::detail::AwaitHandshake handshake;
        std::atomic<bool> completed{false};
        std::function<void()> trigger_fn;
        std::mutex mtx;

        void signal() {
            std::function<void()> fn;
            {
                std::lock_guard<std::mutex> lk(mtx);
                completed.store(true, std::memory_order_release);
                fn = std::move(trigger_fn);
            }
            if (fn) {
                fn();
            }
        }
    };

    std::shared_ptr<State> state{std::make_shared<State>()};

    void signal() {
        if (state) {
            state->signal();
        }
    }

    struct Awaiter {
        std::shared_ptr<State> st;

        bool await_ready() const noexcept {
            return st->completed.load(std::memory_order_acquire);
        }

        template <typename PromiseType>
        bool await_suspend(std::coroutine_handle<PromiseType> coro) {
            auto task_state = coro.promise().shared_state;
            auto rescheduler = task_state->get_rescheduler();

            auto post_action = [coro, task_state, rescheduler]() mutable {
                if (rescheduler) {
                    auto invoker = std::make_unique<astra::detail::CoroutineResumeInvokerModel<typename PromiseType::value_type>>(
                        coro, std::move(task_state));
                    rescheduler(std::move(invoker));
                }
            };

            {
                std::lock_guard<std::mutex> lk(st->mtx);
                st->trigger_fn = [&hs = st->handshake, post_action]() mutable {
                    hs.trigger(post_action);
                };
            }

            task_state->transition_to_suspended();
            st->handshake.arm(std::move(post_action));
            return true;
        }

        void await_resume() const noexcept {}
    };

    Awaiter operator co_await() const {
        return Awaiter{state};
    }
};

// -----------------------------------------------------------------------------
// 1. Suspended 取消后通过 await_resume 抛出 task_cancelled 并完成 Cancelled（R-075）
// -----------------------------------------------------------------------------
astra::Task<int> coro_suspended_cancel_unhandled(CancellationAwareEvent ev, std::atomic<bool>& started, std::atomic<bool>& raii_cleaned) {
    struct RaiiGuard {
        std::atomic<bool>& flag;
        ~RaiiGuard() { flag.store(true, std::memory_order_release); }
    } guard{raii_cleaned};

    started.store(true, std::memory_order_release);
    co_await ev;
    co_return 100;
}

void test_R075_suspended_cancellation_unhandled() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler scheduler(opts);

    CancellationAwareEvent ev;
    std::atomic<bool> started{false};
    std::atomic<bool> raii_cleaned{false};

    auto handle = scheduler.spawn(coro_suspended_cancel_unhandled(ev, started, raii_cleaned));

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    assert(handle.state() == astra::TaskState::Suspended);

    // 请求取消
    handle.request_cancel();

    // 等待任务结束
    bool threw_cancelled = false;
    try {
        handle.get();
    } catch (const astra::task_cancelled&) {
        threw_cancelled = true;
    }

    assert(threw_cancelled);
    assert(handle.state() == astra::TaskState::Cancelled);
    assert(raii_cleaned.load(std::memory_order_acquire));

    scheduler.shutdown();
}

// -----------------------------------------------------------------------------
// 2. Suspended 取消被用户 catch 并在恢复后继续完成（R-075 / D-119 / D-154）
// -----------------------------------------------------------------------------
astra::Task<int> coro_suspended_cancel_user_catch(CancellationAwareEvent ev, std::atomic<bool>& started, std::atomic<bool>& caught_cancel) {
    started.store(true, std::memory_order_release);
    try {
        co_await ev;
        co_return 100;
    } catch (const astra::task_cancelled&) {
        caught_cancel.store(true, std::memory_order_release);
        co_return 777; // 合作恢复并正常返回
    }
}

void test_R075_suspended_cancellation_user_catch() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler scheduler(opts);

    CancellationAwareEvent ev;
    std::atomic<bool> started{false};
    std::atomic<bool> caught_cancel{false};

    auto handle = scheduler.spawn(coro_suspended_cancel_user_catch(ev, started, caught_cancel));

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    assert(handle.state() == astra::TaskState::Suspended);

    handle.request_cancel();

    int res = handle.get();
    assert(res == 777);
    assert(handle.state() == astra::TaskState::Succeeded);
    assert(caught_cancel.load(std::memory_order_acquire));

    scheduler.shutdown();
}

// -----------------------------------------------------------------------------
// 3. Foreign Awaitable 忽略 stop，frame 保留不被强毁，并在后续手动触发后完成（R-075 / D-119）
// -----------------------------------------------------------------------------
astra::Task<int> coro_foreign_awaitable_test(ForeignEvent ev, std::atomic<bool>& started) {
    started.store(true, std::memory_order_release);
    co_await ev;
    co_return 888;
}

void test_R075_foreign_awaitable_ignores_stop() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler scheduler(opts);

    ForeignEvent ev;
    std::atomic<bool> started{false};

    auto handle = scheduler.spawn(coro_foreign_awaitable_test(ev, started));

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    assert(handle.state() == astra::TaskState::Suspended);

    // 请求取消 foreign awaitable
    handle.request_cancel();

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    // 依然处于 Suspended，未被直接 destroy 或伪造 Cancelled
    assert(handle.state() == astra::TaskState::Suspended);

    // 手动触发 foreign event
    ev.signal();

    int res = handle.get();
    assert(res == 888);
    assert(handle.state() == astra::TaskState::Succeeded);

    scheduler.shutdown();
}

// -----------------------------------------------------------------------------
// 4. Immediate Shutdown 取消未启动 Task，但允许已启动 Coroutine Resume Segment 执行合作取消（R-075 / D-154）
// -----------------------------------------------------------------------------
astra::Task<int> coro_immediate_started_resume(CancellationAwareEvent ev, std::atomic<bool>& started, std::atomic<bool>& raii_done) {
    struct Guard {
        std::atomic<bool>& f;
        ~Guard() { f.store(true, std::memory_order_release); }
    } g{raii_done};

    started.store(true, std::memory_order_release);
    co_await ev;
    co_return 555;
}

void test_R075_immediate_cancels_unstarted_resumes_started() {
    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    astra::Scheduler scheduler(opts);

    CancellationAwareEvent ev;
    std::atomic<bool> started{false};
    std::atomic<bool> raii_done{false};

    auto handle = scheduler.spawn(coro_immediate_started_resume(ev, started, raii_done));

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(handle.state() == astra::TaskState::Suspended);

    // 请求协程取消（触发其 resume segment 排队）
    handle.request_cancel();

    // 触发 Immediate 关停（允许 handle 的 resume segment 执行完成）
    scheduler.shutdown_now();

    // 验证 handle 的 resume segment 正常执行了 RAII 清理并发布 Cancelled
    bool threw = false;
    try {
        handle.get();
    } catch (const astra::task_cancelled&) {
        threw = true;
    }

    assert(threw);
    assert(handle.state() == astra::TaskState::Cancelled);
    assert(raii_done.load(std::memory_order_acquire));
}

// -----------------------------------------------------------------------------
// 5. Normal Completion 与 Late Cancel 竞争，正常完成胜出（R-074 / R-075）
// -----------------------------------------------------------------------------
astra::Task<int> coro_completion_wins_race(CancellationAwareEvent ev, std::atomic<bool>& started) {
    started.store(true, std::memory_order_release);
    co_await ev;
    co_return 333;
}

void test_R075_normal_completion_wins_race() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler scheduler(opts);

    CancellationAwareEvent ev;
    std::atomic<bool> started{false};

    auto handle = scheduler.spawn(coro_completion_wins_race(ev, started));

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // 先 signal
    ev.signal();
    // 紧接着 request_cancel
    handle.request_cancel();

    int res = handle.get();
    assert(res == 333);
    assert(handle.state() == astra::TaskState::Succeeded);

    scheduler.shutdown();
}

}  // namespace

int main() {
    std::cout << "Running astra_suspended_cancellation_test..." << std::endl;

    test_R075_suspended_cancellation_unhandled();
    test_R075_suspended_cancellation_user_catch();
    test_R075_foreign_awaitable_ignores_stop();
    test_R075_immediate_cancels_unstarted_resumes_started();
    test_R075_normal_completion_wins_race();

    std::cout << "All AST-034 Suspended cancellation tests passed successfully!" << std::endl;
    return 0;
}

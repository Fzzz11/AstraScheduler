#include <astra/error.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond,     \
                         __FILE__, __LINE__);                                  \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

namespace {

// -----------------------------------------------------------------------------
// R-053: request_cancel 先于首次 start 胜出（用户代码 0 次执行）
// -----------------------------------------------------------------------------
void test_R053_cancel_before_start_wins() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 1;
    astra::Scheduler s(opt);

    std::promise<void> block_promise;
    std::shared_future<void> block_future = block_promise.get_future().share();
    std::atomic<bool> worker_blocked{false};

    // 1. 占住 Worker 0
    auto blocker = s.submit([block_future, &worker_blocked]() {
        worker_blocked.store(true);
        block_future.wait();
    });

    while (!worker_blocked.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 2. 提交排队任务并在 Worker 启动它之前取消
    std::atomic<int> execution_count{0};
    auto h_queued = s.submit([&execution_count]() {
        execution_count.fetch_add(1);
        return 123;
    });

    // 在排队状态下请求取消
    h_queued.request_cancel();
    TEST_ASSERT(h_queued.state() == astra::TaskState::Cancelled);

    // 3. 释放阻塞
    block_promise.set_value();
    blocker.wait();

    // 4. 等待已取消任务并验证 Callable 0 次执行
    h_queued.wait();
    TEST_ASSERT(h_queued.state() == astra::TaskState::Cancelled);
    TEST_ASSERT(execution_count.load() == 0);

    bool caught_cancelled = false;
    try {
        (void)h_queued.get();
    } catch (const astra::task_cancelled&) {
        caught_cancelled = true;
    }
    TEST_ASSERT(caught_cancelled);
}

// -----------------------------------------------------------------------------
// R-053 & R-054: start 胜出、request-only 返回与真实退出决定 Outcome
// -----------------------------------------------------------------------------
void test_R053_R054_running_cooperative_stop_outcomes() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 2;
    astra::Scheduler s(opt);

    // Case 1: Running 任务收到 stop request 后正常返回 -> 发布 Value (Succeeded)
    {
        std::promise<void> start_p;
        std::shared_future<void> started = start_p.get_future().share();

        auto h = s.submit([started, &start_p](std::stop_token token) {
            start_p.set_value();
            while (!token.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // 忽略取消请求，正常返回业务值
            return 888;
        });

        started.wait();
        TEST_ASSERT(h.state() == astra::TaskState::Running);

        // request_cancel 是 request-only，立即返回而不等待任务完成
        h.request_cancel();
        h.wait();

        TEST_ASSERT(h.state() == astra::TaskState::Succeeded);
        TEST_ASSERT(h.get() == 888);
    }

    // Case 2: Running 任务抛出 task_cancelled -> 发布 Cancelled
    {
        std::promise<void> start_p;
        std::shared_future<void> started = start_p.get_future().share();

        auto h = s.submit([started, &start_p](std::stop_token token) {
            start_p.set_value();
            while (!token.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            astra::throw_if_stop_requested(token);
            return 999;
        });

        started.wait();
        h.request_cancel();
        h.wait();

        TEST_ASSERT(h.state() == astra::TaskState::Cancelled);
        bool threw = false;
        try {
            (void)h.get();
        } catch (const astra::task_cancelled&) {
            threw = true;
        }
        TEST_ASSERT(threw);
    }

    // Case 3: Running 任务在 stop 期间抛出其他普通异常 -> 发布 Exception (Failed)
    {
        std::promise<void> start_p;
        std::shared_future<void> started = start_p.get_future().share();

        auto h = s.submit([started, &start_p](std::stop_token token) -> int {
            start_p.set_value();
            while (!token.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            throw std::runtime_error("ordinary error during stop");
        });

        started.wait();
        h.request_cancel();
        h.wait();

        TEST_ASSERT(h.state() == astra::TaskState::Failed);
        bool caught_runtime_error = false;
        try {
            (void)h.get();
        } catch (const std::runtime_error& ex) {
            caught_runtime_error = true;
            TEST_ASSERT(std::string(ex.what()) == "ordinary error during stop");
        }
        TEST_ASSERT(caught_runtime_error);
    }
}

// -----------------------------------------------------------------------------
// R-053: request_cancel 幂等性与空 Handle no-op
// -----------------------------------------------------------------------------
void test_R053_idempotent_and_empty_handle() {
    astra::Scheduler s;

    // 1. 空 Handle request_cancel 为 safe no-op
    astra::TaskHandle<int> empty_h;
    empty_h.request_cancel();
    TEST_ASSERT(!empty_h.valid());

    // 2. 终态任务重复调用 request_cancel 为 safe no-op
    auto h = s.submit([]() { return 10; });
    TEST_ASSERT(h.get() == 10);
    TEST_ASSERT(h.state() == astra::TaskState::Succeeded);

    // 重复请求取消不改变已有 Succeeded 终态
    h.request_cancel();
    h.request_cancel();
    TEST_ASSERT(h.state() == astra::TaskState::Succeeded);
    TEST_ASSERT(h.get() == 10);
}

// -----------------------------------------------------------------------------
// R-054: submit 优先选择普通调用，不意外注入 stop_token
// -----------------------------------------------------------------------------
void test_R054_submit_prefers_ordinary_invocation() {
    astra::Scheduler s;

    // Generic lambda: 同时支持 0 参数和 1 参数调用
    auto generic_fn = [](auto&&... args) {
        return sizeof...(args);
    };

    // 优先匹配 0 参数普通调用 -> args 数量应为 0
    auto h = s.submit(generic_fn);
    TEST_ASSERT(h.get() == 0);
}

}  // namespace

int main() {
    std::printf("Running astra_task_cancellation_test...\n");
    test_R053_cancel_before_start_wins();
    test_R053_R054_running_cooperative_stop_outcomes();
    test_R053_idempotent_and_empty_handle();
    test_R054_submit_prefers_ordinary_invocation();
    std::printf("All AST-013 task cancellation tests passed successfully!\n");
    return 0;
}

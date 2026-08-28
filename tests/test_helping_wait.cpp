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
// R-052: 单 Worker 嵌套 Helping Wait（Parent 等 Child 零死锁）
// -----------------------------------------------------------------------------
void test_R052_single_worker_nested_helping_wait() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 1; // 严苛单 Worker 场景
    astra::Scheduler s(opt);

    auto parent_h = s.submit([&s]() {
        // 在 Worker 0 内部提交 3 个子任务
        auto c1 = s.submit([]() { return 10; });
        auto c2 = s.submit([]() { return 20; });
        auto c3 = s.submit([]() { return 30; });

        // Worker 0 在等待 c1/c2/c3 时通过 Helping Wait 协作窃取并执行子任务
        int sum = c1.get() + c2.get() + c3.get();
        return sum;
    });

    TEST_ASSERT(parent_h.get() == 60);
}

// -----------------------------------------------------------------------------
// R-052 & D-049: Direct Self-Wait 在副作用前抛出 logic_error
// -----------------------------------------------------------------------------
void test_R052_direct_self_wait_rejected() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 1;
    astra::Scheduler s(opt);

    std::shared_ptr<astra::TaskHandle<int>> self_holder =
        std::make_shared<astra::TaskHandle<int>>();
    std::promise<void> ready_promise;
    std::shared_future<void> ready = ready_promise.get_future().share();

    auto h = s.submit([self_holder, ready]() -> int {
        ready.wait();
        // 当前 Task 调用自身的 Handle
        (void)self_holder->get(); // 必须抛出 std::logic_error
        return 1;
    });

    *self_holder = h;
    ready_promise.set_value();

    // 外层获取该任务结果：因为未捕获的 self-wait logic_error 成为 Exception Outcome
    bool caught_logic_error = false;
    try {
        (void)h.get();
    } catch (const std::logic_error& ex) {
        caught_logic_error = true;
        TEST_ASSERT(std::string(ex.what()).find("self-wait") != std::string::npos);
    }
    TEST_ASSERT(caught_logic_error);
    TEST_ASSERT(h.state() == astra::TaskState::Failed);
}

// -----------------------------------------------------------------------------
// R-055: wait() 复用 Helping 且正常返回
// -----------------------------------------------------------------------------
void test_R055_wait_reuses_helping() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 1;
    astra::Scheduler s(opt);

    auto parent = s.submit([&s]() {
        auto child = s.submit([]() {
            return 42;
        });
        child.wait(); // Worker 0 协作执行 child
        TEST_ASSERT(child.state() == astra::TaskState::Succeeded);
        return child.get();
    });

    TEST_ASSERT(parent.get() == 42);
}

// -----------------------------------------------------------------------------
// R-056: wait_for 有界等待、帮助与超时不伪造完成
// -----------------------------------------------------------------------------
void test_R056_wait_for_timeout_and_helping() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 2;
    astra::Scheduler s(opt);

    std::promise<void> release_promise;
    std::shared_future<void> release = release_promise.get_future().share();

    auto h_slow = s.submit([release]() {
        release.wait();
        return 100;
    });

    // 1. Worker 内部 wait_for 较短时间 -> 返回 TimedOut
    auto h_waiter = s.submit([&h_slow]() {
        auto res = h_slow.wait_for(std::chrono::milliseconds(20));
        return res;
    });

    TEST_ASSERT(h_waiter.get() == astra::WaitResult::TimedOut);
    TEST_ASSERT(h_slow.state() != astra::TaskState::Cancelled);

    // 2. 释放任务
    release_promise.set_value();
    TEST_ASSERT(h_slow.get() == 100);
}

// -----------------------------------------------------------------------------
// R-059: Helping depth 超过 max_helping_depth 抛出 helping_depth_exceeded
// -----------------------------------------------------------------------------
void test_R059_helping_depth_limit() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 1;
    opt.max_helping_depth = 2; // 设置上限为 2
    astra::Scheduler s(opt);

    // depth 1: level1 等待 level2
    // depth 2: level2 等待 level3
    // depth 3: level3 等待 level4 -> 超限抛 helping_depth_exceeded!
    auto h_top = s.submit([&s]() -> int {
        auto h_l2 = s.submit([&s]() -> int {
            auto h_l3 = s.submit([&s]() -> int {
                auto h_l4 = s.submit([]() -> int {
                    return 4;
                });
                return h_l4.get(); // depth 3 > 2! 抛 helping_depth_exceeded
            });
            return h_l3.get();
        });
        return h_l2.get();
    });

    bool caught_depth_exceeded = false;
    try {
        (void)h_top.get();
    } catch (const astra::helping_depth_exceeded&) {
        caught_depth_exceeded = true;
    }
    TEST_ASSERT(caught_depth_exceeded);
}

}  // namespace

int main() {
    std::printf("Running astra_helping_wait_test...\n");
    test_R052_single_worker_nested_helping_wait();
    test_R052_direct_self_wait_rejected();
    test_R055_wait_reuses_helping();
    test_R056_wait_for_timeout_and_helping();
    test_R059_helping_depth_limit();
    std::printf("All AST-012 Helping Wait tests passed successfully!\n");
    return 0;
}

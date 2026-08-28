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
// R-006 & R-012: Graceful Stopping 接受 Internal 提交并排空传递闭包
// -----------------------------------------------------------------------------
void test_R006_R012_graceful_drain_internal_closure() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 2;
    astra::Scheduler s(opt);

    std::promise<void> start_p;
    std::shared_future<void> started = start_p.get_future().share();
    std::atomic<bool> child_completed{false};
    std::atomic<bool> grandchild_completed{false};

    // 提交父任务
    auto parent_h = s.submit([&s, started, &start_p, &child_completed, &grandchild_completed]() {
        start_p.set_value();
        // 稍作等待确保外部 shutdown() 已触发并使 Runtime 进入 Stopping 状态
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        // R-006: 在 Graceful Stopping 期间提交 Internal 子任务
        auto child_h = s.submit([&s, &child_completed, &grandchild_completed]() {
            // 提交孙子任务（递归 internal fan-out）
            auto grand_h = s.submit([&grandchild_completed]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                grandchild_completed.store(true);
                return 300;
            });
            int g_res = grand_h.get();
            child_completed.store(true);
            return g_res + 20;
        });

        return child_h.get() + 1;
    });

    started.wait();

    // 非 Worker 线程发起 shutdown()（R-012）
    s.shutdown();

    // shutdown() 返回后，整个 Drain Work Closure 必须完全终结且已处于 Stopped 状态
    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopped);
    TEST_ASSERT(s.status().shutdown_mode == astra::ShutdownMode::Graceful);
    TEST_ASSERT(child_completed.load());
    TEST_ASSERT(grandchild_completed.load());
    TEST_ASSERT(parent_h.get() == 321);
}

// -----------------------------------------------------------------------------
// R-007: Graceful 转换线性化关闭 External Submission
// -----------------------------------------------------------------------------
void test_R007_external_submission_rejected_after_stopping() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 1;
    astra::Scheduler s(opt);

    std::promise<void> hold_p;
    std::shared_future<void> hold_f = hold_p.get_future().share();
    std::atomic<bool> blocker_started{false};

    auto blocker = s.submit([hold_f, &blocker_started]() {
        blocker_started.store(true);
        hold_f.wait();
    });

    while (!blocker_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 在另一个线程发起 shutdown
    std::thread shutdown_th([&s]() {
        s.shutdown();
    });

    // 等待状态变为 Stopping
    while (s.status().state == astra::SchedulerState::Running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopping);

    // R-007: 外部提交被线性化拒绝
    bool caught_stopping = false;
    try {
        (void)s.submit([]() { return 1; });
    } catch (const astra::submission_rejected& ex) {
        caught_stopping = true;
        TEST_ASSERT(ex.reason() == astra::SubmissionError::Stopping);
    }
    TEST_ASSERT(caught_stopping);

    // 释放 blocker
    hold_p.set_value();
    shutdown_th.join();

    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopped);
}

// -----------------------------------------------------------------------------
// R-019: Stopped 是关停吸收状态，后续 shutdown/shutdown_now 立即无副作用返回
// -----------------------------------------------------------------------------
void test_R019_stopped_is_absorbing_state() {
    astra::Scheduler s;
    s.shutdown();
    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopped);
    TEST_ASSERT(s.status().shutdown_mode == astra::ShutdownMode::Graceful);

    // 重复调用 shutdown()：必须立即成功返回且无任何副作用
    s.shutdown();
    s.shutdown();
    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopped);
    TEST_ASSERT(s.status().shutdown_mode == astra::ShutdownMode::Graceful);

    // 调用 shutdown_now()：模式不被改变，保持历史 Graceful
    s.shutdown_now();
    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopped);
    TEST_ASSERT(s.status().shutdown_mode == astra::ShutdownMode::Graceful);
}

}  // namespace

int main() {
    std::printf("Running astra_graceful_drain_test...\n");
    test_R006_R012_graceful_drain_internal_closure();
    test_R007_external_submission_rejected_after_stopping();
    test_R019_stopped_is_absorbing_state();
    std::printf("All AST-014 graceful drain tests passed successfully!\n");
    return 0;
}

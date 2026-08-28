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
// R-014: Graceful 向 Immediate 单向原子升级，且不可降级
// -----------------------------------------------------------------------------
void test_R014_escalation_graceful_to_immediate() {
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

    // 1. 发起 graceful shutdown -> 状态变为 Stopping (Graceful)
    std::thread th_graceful([&s]() {
        s.shutdown();
    });

    while (s.status().state != astra::SchedulerState::Stopping) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_ASSERT(s.status().shutdown_mode == astra::ShutdownMode::Graceful);

    // 2. 发起 shutdown_now() 升级（R-014）
    std::thread th_immediate([&s]() {
        s.shutdown_now();
    });

    while (s.status().shutdown_mode != astra::ShutdownMode::Immediate) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopping);
    TEST_ASSERT(s.status().shutdown_mode == astra::ShutdownMode::Immediate);

    // 3. 释放 blocker
    hold_p.set_value();
    th_graceful.join();
    th_immediate.join();

    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopped);
    TEST_ASSERT(s.status().shutdown_mode == astra::ShutdownMode::Immediate);
}

// -----------------------------------------------------------------------------
// R-106: Immediate 直接取消从未首次 start 的任务且 Callable 0 次执行
// -----------------------------------------------------------------------------
void test_R106_immediate_cancels_unstarted_tasks() {
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

    // 提交 3 个排队任务
    std::atomic<int> exec_count{0};
    auto h1 = s.submit([&exec_count]() { exec_count.fetch_add(1); return 1; });
    auto h2 = s.submit([&exec_count]() { exec_count.fetch_add(1); return 2; });
    auto h3 = s.submit([&exec_count]() { exec_count.fetch_add(1); return 3; });

    // 在另一个线程调用 shutdown_now
    std::thread th_now([&s]() {
        s.shutdown_now();
    });

    // 等待状态变为 Stopping Immediate
    while (s.status().shutdown_mode != astra::ShutdownMode::Immediate) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // R-106: 未启动任务全部被直接取消
    h1.wait();
    h2.wait();
    h3.wait();
    TEST_ASSERT(h1.state() == astra::TaskState::Cancelled);
    TEST_ASSERT(h2.state() == astra::TaskState::Cancelled);
    TEST_ASSERT(h3.state() == astra::TaskState::Cancelled);
    TEST_ASSERT(exec_count.load() == 0);

    // 释放 blocker
    hold_p.set_value();
    th_now.join();

    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopped);
}

// -----------------------------------------------------------------------------
// R-009: Immediate 对 Running Task 仅请求协作停止，不强行杀死已开始任务
// -----------------------------------------------------------------------------
void test_R009_immediate_does_not_force_kill_running_tasks() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 1;
    astra::Scheduler s(opt);

    std::promise<void> started_p;
    std::shared_future<void> started_f = started_p.get_future().share();

    auto h_running = s.submit([started_f, &started_p](std::stop_token token) {
        started_p.set_value();
        // 观察 stop_requested 但选择正常执行完毕
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return 777;
    });

    started_f.wait();
    TEST_ASSERT(h_running.state() == astra::TaskState::Running);

    // 发起 shutdown_now()：必须等待 running 任务正常终结后再发布 Stopped
    s.shutdown_now();

    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopped);
    TEST_ASSERT(h_running.state() == astra::TaskState::Succeeded);
    TEST_ASSERT(h_running.get() == 777);
}

// -----------------------------------------------------------------------------
// R-015: Immediate 升级后关闭内部准入
// -----------------------------------------------------------------------------
void test_R015_internal_submission_rejected_in_immediate() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 1;
    astra::Scheduler s(opt);

    std::promise<void> started_p;
    std::shared_future<void> started_f = started_p.get_future().share();
    std::atomic<bool> caught_internal_rejection{false};

    auto h_parent = s.submit([&s, started_f, &started_p, &caught_internal_rejection]() {
        started_p.set_value();
        // 等待外部升级为 Immediate
        while (s.status().shutdown_mode != astra::ShutdownMode::Immediate) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // R-015: Immediate 下内部提交被拒绝
        try {
            (void)s.submit([]() { return 10; });
        } catch (const astra::submission_rejected& ex) {
            caught_internal_rejection.store(true);
            TEST_ASSERT(ex.reason() == astra::SubmissionError::Stopping);
        }
        return 1;
    });

    started_f.wait();

    s.shutdown_now();

    TEST_ASSERT(caught_internal_rejection.load());
    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopped);
}

}  // namespace

int main() {
    std::printf("Running astra_immediate_escalation_test...\n");
    test_R014_escalation_graceful_to_immediate();
    test_R106_immediate_cancels_unstarted_tasks();
    test_R009_immediate_does_not_force_kill_running_tasks();
    test_R015_internal_submission_rejected_in_immediate();
    std::printf("All AST-016 immediate escalation tests passed successfully!\n");
    return 0;
}

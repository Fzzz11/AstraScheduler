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
// R-010: 非 Worker shutdown_now 同步完成
// -----------------------------------------------------------------------------
void test_R010_non_worker_shutdown_now() {
    astra::Scheduler s;
    s.shutdown_now();
    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopped);
    TEST_ASSERT(s.status().shutdown_mode == astra::ShutdownMode::Immediate);
}

// -----------------------------------------------------------------------------
// R-011 & R-013 & R-108: 目标 Worker self-shutdown / self-shutdown_now 抛 logic_error
// -----------------------------------------------------------------------------
void test_R011_R013_R108_same_worker_self_shutdown_rejected() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 1;
    astra::Scheduler s(opt);

    std::atomic<bool> caught_graceful_logic_error{false};
    std::atomic<bool> caught_immediate_logic_error{false};

    auto h = s.submit([&s, &caught_graceful_logic_error, &caught_immediate_logic_error]() {
        // 1. 同 Runtime Worker 调用 self-shutdown()（R-013 / R-108）
        try {
            s.shutdown();
        } catch (const std::logic_error& ex) {
            caught_graceful_logic_error.store(true);
        }

        // 2. 同 Runtime Worker 调用 self-shutdown_now()（R-011 / R-108）
        try {
            s.shutdown_now();
        } catch (const std::logic_error& ex) {
            caught_immediate_logic_error.store(true);
        }

        return 999;
    });

    TEST_ASSERT(h.get() == 999);
    TEST_ASSERT(caught_graceful_logic_error.load());
    TEST_ASSERT(caught_immediate_logic_error.load());
    TEST_ASSERT(s.status().state == astra::SchedulerState::Running);

    s.shutdown();
    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopped);
}

// -----------------------------------------------------------------------------
// R-108: 跨 Runtime Worker 调用目标 Scheduler 的 shutdown 允许并同步完成
// -----------------------------------------------------------------------------
void test_R108_other_worker_shutdown_allowed() {
    astra::Scheduler s_target;
    astra::Scheduler s_other;

    auto h = s_other.submit([&s_target]() {
        // s_other 的 worker 线程对 s_target 属于外部非 worker 调用
        s_target.shutdown();
        TEST_ASSERT(s_target.status().state == astra::SchedulerState::Stopped);
        return true;
    });

    TEST_ASSERT(h.get() == true);
    TEST_ASSERT(s_target.status().state == astra::SchedulerState::Stopped);
    s_other.shutdown();
}

// -----------------------------------------------------------------------------
// R-016: 并发非 Worker 关停共享一次完成（Leader-Waiter 恰好 join 一次）
// -----------------------------------------------------------------------------
void test_R016_concurrent_shutdown_leader_waiter() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 4;
    astra::Scheduler s(opt);

    // 提交一些简单任务
    std::vector<astra::TaskHandle<int>> handles;
    for (int i = 0; i < 10; ++i) {
        handles.push_back(s.submit([i]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return i;
        }));
    }

    // 启动 8 个并发线程同时调用 shutdown() / shutdown_now()
    constexpr int THREAD_COUNT = 8;
    std::vector<std::thread> callers;
    callers.reserve(THREAD_COUNT);

    std::promise<void> start_p;
    std::shared_future<void> start_f = start_p.get_future().share();

    for (int i = 0; i < THREAD_COUNT; ++i) {
        callers.emplace_back([&s, start_f, i]() {
            start_f.wait();
            if (i % 2 == 0) {
                s.shutdown();
            } else {
                s.shutdown_now();
            }
            TEST_ASSERT(s.status().state == astra::SchedulerState::Stopped);
        });
    }

    start_p.set_value();

    for (auto& t : callers) {
        t.join();
    }

    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopped);
}

}  // namespace

int main() {
    std::printf("Running astra_shutdown_guards_test...\n");
    test_R010_non_worker_shutdown_now();
    test_R011_R013_R108_same_worker_self_shutdown_rejected();
    test_R108_other_worker_shutdown_allowed();
    test_R016_concurrent_shutdown_leader_waiter();
    std::printf("All AST-015 shutdown guards tests passed successfully!\n");
    return 0;
}

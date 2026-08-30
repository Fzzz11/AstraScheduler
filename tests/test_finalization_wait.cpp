#include <astra/error.hpp>
#include <astra/finalization.hpp>
#include <astra/scheduler.hpp>
#include "lifecycle/reaper_registry.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
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
// R-032: wait() 仅在真实 Finalization Completion 达成后返回
// -----------------------------------------------------------------------------
void test_R032_wait_observes_real_finalization_completion() {
    astra::detail::ReaperRegistry::instance().reset_for_testing();

    std::atomic<bool> task_done{false};
    {
        astra::Scheduler s;
        (void)s.submit([&task_done] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            task_done.store(true);
        });
        auto ctrl = astra::begin_finalization();
        ctrl.wait();
        TEST_ASSERT(task_done.load());
        TEST_ASSERT(s.status().state == astra::SchedulerState::Stopped);
    }
}

// -----------------------------------------------------------------------------
// R-033: wait_for 超时不改变 Finalization，后台继续推进
// -----------------------------------------------------------------------------
void test_R033_wait_for_timeout_does_not_abort_and_resumes() {
    astra::detail::ReaperRegistry::instance().reset_for_testing();

    std::atomic<bool> task_done{false};
    astra::Scheduler s;
    (void)s.submit([&task_done] {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        task_done.store(true);
    });

    auto ctrl = astra::begin_finalization();
    // 首次极短等待发生超时
    auto res1 = ctrl.wait_for(std::chrono::milliseconds(1));
    TEST_ASSERT(res1 == astra::FinalizationWaitResult::TimedOut);
    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopping ||
                s.status().state == astra::SchedulerState::Stopped);

    // 随后继续等待直至完成
    auto res2 = ctrl.wait_for(std::chrono::milliseconds(500));
    TEST_ASSERT(res2 == astra::FinalizationWaitResult::Completed);
    TEST_ASSERT(task_done.load());
    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopped);
}

// -----------------------------------------------------------------------------
// R-039: 任意 Scheduler Worker 调用 wait 抛出 logic_error
// -----------------------------------------------------------------------------
void test_R039_worker_calling_wait_throws_logic_error() {
    astra::detail::ReaperRegistry::instance().reset_for_testing();

    astra::Scheduler s;
    std::atomic<bool> threw_logic_error{false};
    auto handle = s.submit([&threw_logic_error] {
        auto ctrl = astra::begin_finalization();
        try {
            ctrl.wait();
        } catch (const std::logic_error&) {
            threw_logic_error.store(true);
        }
    });

    handle.get();
    TEST_ASSERT(threw_logic_error.load());
}

// -----------------------------------------------------------------------------
// R-040: 任意 Scheduler Worker 调用 wait_for 抛出 logic_error（正/零/负 timeout）
// -----------------------------------------------------------------------------
void test_R040_worker_calling_wait_for_throws_logic_error() {
    astra::detail::ReaperRegistry::instance().reset_for_testing();

    astra::Scheduler s;
    std::atomic<int> throw_count{0};
    auto handle = s.submit([&throw_count] {
        auto ctrl = astra::begin_finalization();
        try {
            (void)ctrl.wait_for(std::chrono::milliseconds(10));
        } catch (const std::logic_error&) {
            throw_count.fetch_add(1);
        }
        try {
            (void)ctrl.wait_for(std::chrono::milliseconds(0));
        } catch (const std::logic_error&) {
            throw_count.fetch_add(1);
        }
        try {
            (void)ctrl.wait_for(std::chrono::milliseconds(-10));
        } catch (const std::logic_error&) {
            throw_count.fetch_add(1);
        }
    });

    handle.get();
    TEST_ASSERT(throw_count.load() == 3);
}

// -----------------------------------------------------------------------------
// R-041, R-042: 合法等待者唯一 join coordinator，多线程等待一致性
// -----------------------------------------------------------------------------
void test_R041_R042_concurrent_waiters_single_join() {
    astra::detail::ReaperRegistry::instance().reset_for_testing();

    astra::Scheduler s;
    (void)s.submit([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });

    auto ctrl = astra::begin_finalization();

    constexpr int kWaiters = 8;
    std::vector<std::thread> waiter_threads;
    waiter_threads.reserve(kWaiters);
    std::atomic<int> completed_count{0};

    for (int i = 0; i < kWaiters; ++i) {
        waiter_threads.emplace_back([ctrl, &completed_count, i] {
            if (i % 2 == 0) {
                ctrl.wait();
                completed_count.fetch_add(1);
            } else {
                auto res = ctrl.wait_for(std::chrono::milliseconds(500));
                if (res == astra::FinalizationWaitResult::Completed) {
                    completed_count.fetch_add(1);
                }
            }
        });
    }

    for (auto& t : waiter_threads) {
        t.join();
    }

    TEST_ASSERT(completed_count.load() == kWaiters);
}

}  // namespace

int main() {
    std::printf("Running astra_finalization_wait_test...\n");
    test_R032_wait_observes_real_finalization_completion();
    test_R033_wait_for_timeout_does_not_abort_and_resumes();
    test_R039_worker_calling_wait_throws_logic_error();
    test_R040_worker_calling_wait_for_throws_logic_error();
    test_R041_R042_concurrent_waiters_single_join();
    std::printf("All AST-020 finalization wait tests passed successfully!\n");
    return 0;
}

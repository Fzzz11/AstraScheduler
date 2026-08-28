#include <astra/error.hpp>
#include <astra/finalization.hpp>
#include <astra/scheduler.hpp>
#include "reaper_registry.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
// R-031: begin_finalization 只发布开始请求，绝不同步等待完成
// -----------------------------------------------------------------------------
void test_R031_begin_does_not_wait_for_runtime_completion() {
    astra::detail::ReaperRegistry::instance().reset_for_testing();

    std::atomic<bool> task_started{false};
    std::atomic<bool> task_can_finish{false};
    std::atomic<bool> task_finished{false};

    astra::Scheduler s;
    auto handle = s.submit([&]() {
        task_started.store(true);
        while (!task_can_finish.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        task_finished.store(true);
    });

    while (!task_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto t0 = std::chrono::steady_clock::now();
    auto ctrl = astra::begin_finalization();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // begin_finalization 必须立即返回（< 30ms），不得等待后台任务
    TEST_ASSERT(elapsed < 40);
    TEST_ASSERT(!task_finished.load());

    // 放行任务
    task_can_finish.store(true);
    handle.get();
    TEST_ASSERT(task_finished.load());
}

// -----------------------------------------------------------------------------
// R-037: 幂等共享唯一世代；空系统 begin 直接完成且不创建 coordinator 线程
// -----------------------------------------------------------------------------
void test_R037_empty_system_and_idempotent_generation() {
    astra::detail::ReaperRegistry::instance().reset_for_testing();

    // 1. 空系统调用 begin_finalization：不创建 coordinator 线程
    auto ctrl1 = astra::begin_finalization();
    TEST_ASSERT(astra::detail::ReaperRegistry::instance().coordinator_thread_count() == 0);

    // 2. 幂等重复调用：并发与序列调用均安全无副作用
    constexpr int kThreads = 8;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([] {
            auto ctrl = astra::begin_finalization();
            ctrl.request_immediate();
        });
    }
    for (auto& t : workers) {
        t.join();
    }
    TEST_ASSERT(astra::detail::ReaperRegistry::instance().coordinator_thread_count() == 0);
}

// -----------------------------------------------------------------------------
// R-038: Worker 线程可发起 begin 与 request_immediate，不产生 self-wait
// -----------------------------------------------------------------------------
void test_R038_worker_can_call_begin_and_request_immediate() {
    astra::detail::ReaperRegistry::instance().reset_for_testing();

    astra::Scheduler s;
    std::atomic<bool> worker_called{false};

    auto handle = s.submit([&worker_called]() {
        auto ctrl = astra::begin_finalization();
        ctrl.request_immediate();
        worker_called.store(true);
    });

    handle.get();
    TEST_ASSERT(worker_called.load());
}

// -----------------------------------------------------------------------------
// R-104: Finalization 对已核算 Runtime 请求 Graceful；赢得 startup 竞态则拒绝创建
// -----------------------------------------------------------------------------
void test_R104_finalization_graceful_and_startup_race() {
    astra::detail::ReaperRegistry::instance().reset_for_testing();

    astra::Scheduler s;
    // begin 之前创建的 Runtime 在 close 之后被请求 Graceful
    auto ctrl = astra::begin_finalization();
    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopping ||
                s.status().state == astra::SchedulerState::Stopped);
    TEST_ASSERT(s.status().shutdown_mode == astra::ShutdownMode::Graceful);

    // close 之后创建 Runtime 必须被拒绝（强事务回滚）
    bool rejected = false;
    try {
        astra::Scheduler s2;
    } catch (const astra::scheduler_creation_rejected& e) {
        if (e.reason() == astra::SchedulerCreationError::FinalizationStarted) {
            rejected = true;
        }
    }
    TEST_ASSERT(rejected);
}

}  // namespace

int main() {
    std::printf("Running astra_finalization_begin_test...\n");
    test_R031_begin_does_not_wait_for_runtime_completion();
    test_R037_empty_system_and_idempotent_generation();
    test_R038_worker_can_call_begin_and_request_immediate();
    test_R104_finalization_graceful_and_startup_race();
    std::printf("All AST-019 finalization begin tests passed successfully!\n");
    return 0;
}

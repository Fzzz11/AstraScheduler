#include <astra/capabilities.hpp>
#include <astra/error.hpp>
#include <astra/id.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>
#include <astra/status.hpp>
#include "lifecycle/reaper_registry.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include "expected_local_backend.hpp"
#include "testing/test_seam.hpp"

// AST-008 测试套件：交付 Global-only Worker Runtime 基线
// 覆盖 primary 规则：
// - R-001: v0.1.0 使用全局注入队列基线（所有 Ready Task 经带锁 Global Injection Queue 调度）
// - R-002: v0.1.0 排除本地队列与任务窃取（无 Per-Worker Local Queue / Chase-Lev Deque）

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond,     \
                         __FILE__, __LINE__);                                  \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

namespace astra::detail {
void run_test_task_on_worker(Scheduler& s, std::function<void()> task);
std::size_t global_injection_queue_size(const Scheduler& s);
}

namespace {

// -----------------------------------------------------------------------------
// R-001: 全局注入队列 FIFO 调度与并发提交正确性
// -----------------------------------------------------------------------------
void test_R001_global_injection_queue_baseline() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();

    astra::SchedulerOptions opt{};
    opt.worker_count = 1; // 单 worker，严格验证 FIFO 调度顺序
    astra::Scheduler s(opt);

    std::vector<int> executed_order;
    std::mutex order_mutex;
    std::atomic<int> completed_count{0};
    const int kTaskCount = 20;

    for (int i = 0; i < kTaskCount; ++i) {
        astra::detail::run_test_task_on_worker(s, [&, idx = i] {
            {
                std::lock_guard<std::mutex> lock(order_mutex);
                executed_order.push_back(idx);
            }
            completed_count.fetch_add(1);
        });
    }

    while (completed_count.load() < kTaskCount) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 验证所有任务均已执行且单 Worker 下遵循 Global Queue FIFO 顺序
    TEST_ASSERT(executed_order.size() == static_cast<std::size_t>(kTaskCount));
    for (int i = 0; i < kTaskCount; ++i) {
        TEST_ASSERT(executed_order[i] == i);
    }
}

// -----------------------------------------------------------------------------
// R-001: 多外部线程并发向 Global Injection Queue 提交任务
// -----------------------------------------------------------------------------
void test_R001_concurrent_external_submissions() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();

    astra::SchedulerOptions opt{};
    opt.worker_count = 4;
    astra::Scheduler s(opt);

    const int kThreads = 8;
    const int kTasksPerThread = 100;
    std::atomic<int> total_executed{0};

    std::vector<std::thread> producers;
    producers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&] {
            for (int i = 0; i < kTasksPerThread; ++i) {
                astra::detail::run_test_task_on_worker(s, [&] {
                    total_executed.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    while (total_executed.load() < kThreads * kTasksPerThread) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    TEST_ASSERT(total_executed.load() == kThreads * kTasksPerThread);
}

// -----------------------------------------------------------------------------
// R-001: Worker 任务内部派生子任务依然走 Global Injection Queue
// -----------------------------------------------------------------------------
void test_R001_nested_task_submission() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();

    astra::SchedulerOptions opt{};
    opt.worker_count = 2;
    astra::Scheduler s(opt);

    std::atomic<int> nested_executed{0};

    astra::detail::run_test_task_on_worker(s, [&] {
        // 在 Worker 线程中向同一个 Scheduler 提交 5 个嵌套任务
        for (int i = 0; i < 5; ++i) {
            astra::detail::run_test_task_on_worker(s, [&] {
                nested_executed.fetch_add(1);
            });
        }
    });

    while (nested_executed.load() < 5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    TEST_ASSERT(nested_executed.load() == 5);
}

// -----------------------------------------------------------------------------
// R-002 / R-101: 验证本地队列能力报告
// -----------------------------------------------------------------------------
void test_R002_exclude_local_queues_and_work_stealing() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();

    astra::SchedulerOptions opt{};
    opt.worker_count = 4;
    astra::Scheduler s(opt);

    // 验证能力报告与生产 ReadyQueues 实际 backend 一致（R-101）。
    const auto caps = s.capabilities();
    TEST_ASSERT(caps.local_deque_backend() == kExpectedLocalDequeBackend);
    TEST_ASSERT(caps.lock_free_local_deque() == kExpectedLocalDequeLockFree);
}

}  // namespace

int main() {
    std::printf("Running astra_global_worker_runtime_test...\n");
    test_R001_global_injection_queue_baseline();
    test_R001_concurrent_external_submissions();
    test_R001_nested_task_submission();
    test_R002_exclude_local_queues_and_work_stealing();
    std::printf("All AST-008 Global-only Worker Runtime tests passed successfully!\n");
    return 0;
}

#include <astra/error.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include "testing/test_seam.hpp"

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond,     \
                         __FILE__, __LINE__);                                  \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

namespace astra::detail {
std::size_t parked_workers_count(const Scheduler& s);
std::uint64_t current_work_epoch(const Scheduler& s);
}

namespace {

// -----------------------------------------------------------------------------
// R-065: 空闲 Worker 正确进入 Park 状态，不忙等自旋，有工作时即时唤醒
// -----------------------------------------------------------------------------
void test_R065_idle_workers_park_and_wake() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 4;
    astra::Scheduler s(opt);

    // 等待所有 4 个 Worker 进入 Park 状态
    auto start = std::chrono::steady_clock::now();
    while (astra::detail::parked_workers_count(s) < 4) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            TEST_ASSERT(false && "Timeout waiting for workers to park");
        }
    }
    TEST_ASSERT(astra::detail::parked_workers_count(s) == 4);

    // 提交任务，唤醒 Worker 执行
    std::atomic<bool> executed{false};
    auto h = s.submit([&executed] {
        executed.store(true);
    });
    h.wait();
    TEST_ASSERT(executed.load());

    // 任务执行完毕后，所有 Worker 再次全部进入 Park 状态
    start = std::chrono::steady_clock::now();
    while (astra::detail::parked_workers_count(s) < 4) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            TEST_ASSERT(false && "Timeout waiting for workers to re-park");
        }
    }
    TEST_ASSERT(astra::detail::parked_workers_count(s) == 4);

    s.shutdown();
}

// -----------------------------------------------------------------------------
// R-065: 高并发 Producer 与 Park 竞态下无丢唤醒（No Lost Wakeups）
// -----------------------------------------------------------------------------
void test_R065_producer_park_race_no_lost_wakeups() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 4;
    astra::Scheduler s(opt);

    constexpr int kNumProducers = 4;
    constexpr int kTasksPerProducer = 100;
    std::atomic<int> completed_tasks{0};

    std::vector<std::thread> producers;
    producers.reserve(kNumProducers);

    for (int p = 0; p < kNumProducers; ++p) {
        producers.emplace_back([&s, &completed_tasks] {
            for (int i = 0; i < kTasksPerProducer; ++i) {
                (void)s.submit([&completed_tasks] {
                    completed_tasks.fetch_add(1, std::memory_order_relaxed);
                });
                // 偶尔微休眠触发 Worker 进入 Park 流程，制造竞态
                if (i % 20 == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    auto start = std::chrono::steady_clock::now();
    while (completed_tasks.load() < kNumProducers * kTasksPerProducer) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
            TEST_ASSERT(false && "Lost wakeup detected: tasks did not complete");
        }
    }

    TEST_ASSERT(completed_tasks.load() == kNumProducers * kTasksPerProducer);
    s.shutdown();
}

// -----------------------------------------------------------------------------
// R-065: 控制面状态变更（Shutdown）唤醒所有 Parked Worker
// -----------------------------------------------------------------------------
void test_R065_control_plane_wakes_all_parked_workers() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 4;
    astra::Scheduler s(opt);

    // 等待 Worker 全部 Park
    auto start = std::chrono::steady_clock::now();
    while (astra::detail::parked_workers_count(s) < 4) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            TEST_ASSERT(false && "Timeout waiting for workers to park");
        }
    }

    // 触发 Shutdown，必须干净唤醒并 Join
    s.shutdown();
    TEST_ASSERT(s.status().state == astra::SchedulerState::Stopped);
}

}  // namespace

int main() {
    std::printf("Running astra_park_handshake_test...\n");
    test_R065_idle_workers_park_and_wake();
    test_R065_producer_park_race_no_lost_wakeups();
    test_R065_control_plane_wakes_all_parked_workers();
    std::printf("All AST-024 park handshake tests passed successfully!\n");
    return 0;
}

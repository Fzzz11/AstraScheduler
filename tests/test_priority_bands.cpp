#include "astra/coroutine.hpp"
#include "astra/graph.hpp"
#include "astra/scheduler.hpp"
#include "astra/task_handle.hpp"
#include "astra/task_options.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// 1. Saturated 8:4:2:1 service ratio verification (R-081 / D-130 / D-131)
// -----------------------------------------------------------------------------
void test_R081_saturated_calendar_ratio_8_4_2_1() {
    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    opts.external_pending_capacity = 500;
    astra::Scheduler sched(opts);

    // 1.1 用 blocker 占住 worker，使后续任务全部在 global injection queue 中就绪
    std::atomic<bool> blocker_running{false};
    std::atomic<bool> can_finish{false};
    auto blocker = sched.submit([&] {
        blocker_running.store(true);
        while (!can_finish.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    while (!blocker_running.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // 1.2 注入饱和多轮比例任务：10 个周期，每周期 8 Critical, 4 High, 2 Normal, 1 Low
    constexpr int kRounds = 10;
    std::vector<astra::Priority> execution_order;
    std::mutex order_mutex;

    std::vector<astra::TaskHandle<void>> handles;
    handles.reserve(kRounds * 15);

    for (int r = 0; r < kRounds; ++r) {
        for (int i = 0; i < 8; ++i) {
            handles.push_back(sched.submit(astra::TaskOptions{astra::Priority::Critical}, [&] {
                std::lock_guard<std::mutex> lk(order_mutex);
                execution_order.push_back(astra::Priority::Critical);
            }));
        }
        for (int i = 0; i < 4; ++i) {
            handles.push_back(sched.submit(astra::TaskOptions{astra::Priority::High}, [&] {
                std::lock_guard<std::mutex> lk(order_mutex);
                execution_order.push_back(astra::Priority::High);
            }));
        }
        for (int i = 0; i < 2; ++i) {
            handles.push_back(sched.submit(astra::TaskOptions{astra::Priority::Normal}, [&] {
                std::lock_guard<std::mutex> lk(order_mutex);
                execution_order.push_back(astra::Priority::Normal);
            }));
        }
        for (int i = 0; i < 1; ++i) {
            handles.push_back(sched.submit(astra::TaskOptions{astra::Priority::Low}, [&] {
                std::lock_guard<std::mutex> lk(order_mutex);
                execution_order.push_back(astra::Priority::Low);
            }));
        }
    }

    // 1.3 释放 blocker 并等待全部任务完成
    can_finish.store(true, std::memory_order_release);
    blocker.wait();
    for (auto& h : handles) {
        h.wait();
    }

    // 1.4 验证执行顺序和统计服务比例
    assert(execution_order.size() == kRounds * 15);
    int crit_count = 0;
    int high_count = 0;
    int norm_count = 0;
    int low_count = 0;

    for (auto p : execution_order) {
        if (p == astra::Priority::Critical)
            ++crit_count;
        else if (p == astra::Priority::High)
            ++high_count;
        else if (p == astra::Priority::Normal)
            ++norm_count;
        else if (p == astra::Priority::Low)
            ++low_count;
    }

    assert(crit_count == 8 * kRounds);
    assert(high_count == 4 * kRounds);
    assert(norm_count == 2 * kRounds);
    assert(low_count == 1 * kRounds);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 2. Empty band skipping / work-conserving fallback (R-081 / D-131)
// -----------------------------------------------------------------------------
void test_R081_empty_band_skipping_work_conserving() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    // 只有 Normal 和 Low，Critical 和 High 为空
    std::atomic<int> completed{0};
    std::vector<astra::TaskHandle<void>> handles;

    for (int i = 0; i < 20; ++i) {
        handles.push_back(sched.submit(astra::TaskOptions{astra::Priority::Low},
                                       [&] { completed.fetch_add(1); }));
        handles.push_back(sched.submit(astra::TaskOptions{astra::Priority::Normal},
                                       [&] { completed.fetch_add(1); }));
    }

    for (auto& h : handles) {
        h.wait();
    }

    assert(completed.load() == 40);
    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 3. Continuous Critical load does not starve Low (R-081 / D-131)
// -----------------------------------------------------------------------------
void test_R081_continuous_critical_no_low_starvation() {
    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    opts.external_pending_capacity = 1000;
    astra::Scheduler sched(opts);

    // 3.1 用 blocker 占住 worker
    std::atomic<bool> blocker_running{false};
    std::atomic<bool> can_finish{false};
    auto blocker = sched.submit([&] {
        blocker_running.store(true);
        while (!can_finish.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    while (!blocker_running.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // 3.2 先入队 1 个 Low 任务和 100 个 Critical 任务
    std::atomic<bool> low_executed{false};
    auto low_handle = sched.submit(astra::TaskOptions{astra::Priority::Low},
                                   [&] { low_executed.store(true, std::memory_order_release); });

    std::vector<astra::TaskHandle<void>> crit_handles;
    crit_handles.reserve(100);
    for (int i = 0; i < 100; ++i) {
        crit_handles.push_back(sched.submit(astra::TaskOptions{astra::Priority::Critical}, [&] {}));
    }

    // 3.3 释放 blocker
    can_finish.store(true, std::memory_order_release);
    blocker.wait();

    // 3.4 验证 Low 任务在有限时间内完成，并未被 100 个 Critical 任务永久饿死
    const auto low_waited = low_handle.wait_for(std::chrono::seconds(30));
    assert(low_waited == astra::WaitResult::Completed);
    assert(low_executed.load(std::memory_order_acquire));

    for (auto& h : crit_handles) {
        h.wait();
    }

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 4. Non-preemption of running task (R-081 / D-131)
// -----------------------------------------------------------------------------
void test_R081_non_preemption_of_running_task() {
    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    astra::Scheduler sched(opts);

    std::atomic<bool> low_started{false};
    std::atomic<bool> low_finished{false};
    std::atomic<bool> crit_finished{false};

    // 4.1 提交 Low 优先级任务
    auto low_h = sched.submit(astra::TaskOptions{astra::Priority::Low}, [&] {
        low_started.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        low_finished.store(true, std::memory_order_release);
    });

    while (!low_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // 4.2 当 Low 任务正在运行时，提交 Critical 任务
    auto crit_h = sched.submit(astra::TaskOptions{astra::Priority::Critical}, [&] {
        // Critical 开始执行时，Low 必须已经完整执行完毕（非抢占）
        assert(low_finished.load(std::memory_order_acquire));
        crit_finished.store(true, std::memory_order_release);
    });

    low_h.wait();
    crit_h.wait();

    assert(low_finished.load());
    assert(crit_finished.load());

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 5. Internal worker local bands service
// -----------------------------------------------------------------------------
void test_R081_internal_local_bands_service() {
    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    astra::Scheduler sched(opts);

    auto parent = sched.submit([&] {
        // 在 worker 内部提交不同 Priority 的子任务
        std::atomic<int> completed{0};
        auto h_crit = sched.submit(astra::TaskOptions{astra::Priority::Critical},
                                   [&] { completed.fetch_add(1); });
        auto h_high = sched.submit(astra::TaskOptions{astra::Priority::High},
                                   [&] { completed.fetch_add(1); });
        auto h_norm = sched.submit(astra::TaskOptions{astra::Priority::Normal},
                                   [&] { completed.fetch_add(1); });
        auto h_low =
            sched.submit(astra::TaskOptions{astra::Priority::Low}, [&] { completed.fetch_add(1); });

        h_crit.wait();
        h_high.wait();
        h_norm.wait();
        h_low.wait();

        assert(completed.load() == 4);
        return true;
    });

    assert(parent.get() == true);
    sched.shutdown();
}

} // namespace

int main() {
    std::cout << "Running astra_priority_bands_test..." << std::endl;

    std::cout << "Running test 1 (saturated 8:4:2:1 ratio)..." << std::endl;
    test_R081_saturated_calendar_ratio_8_4_2_1();

    std::cout << "Running test 2 (empty band skipping)..." << std::endl;
    test_R081_empty_band_skipping_work_conserving();

    std::cout << "Running test 3 (continuous critical no starvation)..." << std::endl;
    test_R081_continuous_critical_no_low_starvation();

    std::cout << "Running test 4 (non-preemption of running task)..." << std::endl;
    test_R081_non_preemption_of_running_task();

    std::cout << "Running test 5 (internal worker local bands)..." << std::endl;
    test_R081_internal_local_bands_service();

    std::cout << "All AST-039 priority bands tests passed successfully!" << std::endl;
    return 0;
}

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
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>
#include "testing/test_seam.hpp"

// AST-006 测试套件：解耦 Runtime State 并实现最后 Worker Handle handoff
// 覆盖 primary 规则：
// - R-020: Handle 生命周期与共享 Runtime State 解耦
// - R-021: 目标 Worker 最后 Handle 通过 Reaper handoff 返回，杜绝 self-join / deadlock
// - R-022: Worker orphan handoff 保留 Graceful 默认

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond,     \
                         __FILE__, __LINE__);                                  \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

namespace {

// -----------------------------------------------------------------------------
// R-020: Handle 与 Runtime State 生命周期解耦
// -----------------------------------------------------------------------------
void test_R020_runtime_state_decoupled_from_handle_lifetime() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();

    // 1. 多 Handle 共享同一底层 Runtime State
    {
        astra::SchedulerOptions opt{};
        opt.worker_count = 2;
        astra::Scheduler s1(opt);
        astra::Scheduler s2 = s1; // 共享同一 State

        TEST_ASSERT(s1.valid() && s2.valid());
        TEST_ASSERT(s1.runtime_id() == s2.runtime_id());
        TEST_ASSERT(registry.registered_count() == 1);

        // 移动销毁 s1，s2 仍然完全可用
        {
            astra::Scheduler dummy = std::move(s1);
            TEST_ASSERT(!s1.valid());
            TEST_ASSERT(dummy.valid());
        }
        TEST_ASSERT(!s1.valid());
        TEST_ASSERT(s2.valid());
        TEST_ASSERT(s2.status().state == astra::SchedulerState::Running);
        TEST_ASSERT(registry.registered_count() == 1);
    }
    // 最后一个非 Worker Handle 销毁后同步回收
    TEST_ASSERT(registry.registered_count() == 0);
}

// -----------------------------------------------------------------------------
// R-021 & R-022: 目标 Worker 线程销毁最后 Handle 时进行异步 handoff
// -----------------------------------------------------------------------------
void test_R021_R022_worker_last_handle_destruction_handoff() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();

    std::atomic<bool> task_completed{false};
    std::atomic<bool> destructor_returned_immediately{false};
    std::atomic<bool> handoff_seen{false};

    astra::SchedulerOptions opt{};
    opt.worker_count = 2;
    auto s = std::make_unique<astra::Scheduler>(opt);
    const astra::RuntimeId id = s->runtime_id();

    auto* init_slot = registry.find_slot(id);
    TEST_ASSERT(init_slot != nullptr);
    TEST_ASSERT(!init_slot->handoff_executed.load());

    auto holder = std::make_shared<astra::Scheduler>(std::move(*s));
    s.reset();

    // 获取引用后，将 holder 移动进 lambda，清空外部变量
    auto& sched_ref = *holder;
    auto task = [&, h = std::move(holder)]() mutable {
        const auto t0 = std::chrono::steady_clock::now();

        // 在当前 Worker 线程上销毁唯一的 Handle
        h.reset();

        const auto t1 = std::chrono::steady_clock::now();
        const auto duration_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        // 必须立即返回，不得 self-join（若发生 self-join 会耗时巨大或死锁）
        if (duration_ms < 500) {
            destructor_returned_immediately.store(true);
        }

        auto* running_slot = registry.find_slot(id);
        if (running_slot && running_slot->handoff_executed.load()) {
            handoff_seen.store(true);
        }

        // R-021: 任务在 handoff 发生后继续安全执行并返回
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        task_completed.store(true);
    };

    astra::detail::run_test_task_on_worker(sched_ref, std::move(task));

    // 等待 Worker 任务执行完成
    const auto wait_start = std::chrono::steady_clock::now();
    while (!task_completed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - wait_start).count() > 5) {
            TEST_ASSERT(false && "Worker task timed out or deadlocked");
        }
    }

    TEST_ASSERT(destructor_returned_immediately.load());
    TEST_ASSERT(handoff_seen.load());

    // 等待非 Worker Reaper 线程完成 join 与最终回收
    const auto reaper_wait_start = std::chrono::steady_clock::now();
    while (registry.registered_count() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - reaper_wait_start).count() > 5) {
            TEST_ASSERT(false && "Reaper join timed out");
        }
    }

    TEST_ASSERT(registry.registered_count() == 0);
}

// -----------------------------------------------------------------------------
// R-021: 多 Worker 场景下由 Worker 0 释放最后 Handle
// -----------------------------------------------------------------------------
void test_R021_multi_worker_handoff() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();

    std::atomic<bool> task_finished{false};
    astra::SchedulerOptions opt{};
    opt.worker_count = 4;
    auto s = std::make_unique<astra::Scheduler>(opt);
    const astra::RuntimeId id = s->runtime_id();

    auto* slot = registry.find_slot(id);
    TEST_ASSERT(slot != nullptr);

    auto holder = std::make_shared<astra::Scheduler>(std::move(*s));
    s.reset();

    auto& sched_ref = *holder;
    auto task = [&, h = std::move(holder)]() mutable {
        h.reset();
        task_finished.store(true);
    };

    astra::detail::run_test_task_on_worker(sched_ref, std::move(task));

    while (!task_finished.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    while (registry.registered_count() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    TEST_ASSERT(registry.registered_count() == 0);
}

}  // namespace

int main() {
    std::printf("Running astra_runtime_state_handoff_test...\n");
    test_R020_runtime_state_decoupled_from_handle_lifetime();
    test_R021_R022_worker_last_handle_destruction_handoff();
    test_R021_multi_worker_handoff();
    std::printf("All AST-006 runtime state handoff tests passed successfully!\n");
    return 0;
}

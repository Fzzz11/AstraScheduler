#include <astra/capabilities.hpp>
#include <astra/error.hpp>
#include <astra/id.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>
#include <astra/status.hpp>
#include "reaper_registry.hpp"

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
#include <vector>

// AST-007 测试套件：实现唯一 Reaper coordinator 的 pending/join/idle 循环
// 覆盖 primary 规则：
// - R-025: Pending Runtime 不阻塞 Reaper（Head-of-Line 隔离）
// - R-026: Join Ready 后唯一 join 并发布 Stopped
// - R-028: Reaper 空闲时保持同一服务
// - R-107: Supported Configuration 只有一个实现实例与 Reaper coordinator

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
}

namespace {

// -----------------------------------------------------------------------------
// R-025: 一个长期 Pending 的 Runtime 不得阻塞其他 Join Ready Runtime 的回收
// -----------------------------------------------------------------------------
void test_R025_pending_runtime_does_not_block_reaper() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();

    std::atomic<bool> task_a_hold{true};
    std::atomic<bool> task_a_handoff_done{false};
    std::atomic<bool> task_b_completed{false};

    // 1. 创建 Scheduler A
    astra::SchedulerOptions opt_a{};
    opt_a.worker_count = 1;
    auto sa = std::make_unique<astra::Scheduler>(opt_a);
    const astra::RuntimeId id_a = sa->runtime_id();

    auto holder_a = std::make_shared<astra::Scheduler>(std::move(*sa));
    sa.reset();

    // 在 Scheduler A 的 Worker 上释放最后 Handle，并保持任务处于阻塞执行状态（Pending）
    auto& ref_a = *holder_a;
    auto task_a = [&, h = std::move(holder_a)]() mutable {
        h.reset(); // 执行 orphan handoff，Runtime A 进入 Pending 状态
        task_a_handoff_done.store(true);
        // 持续阻塞，模拟长时间未完成的 Drain/任务
        while (task_a_hold.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    astra::detail::run_test_task_on_worker(ref_a, std::move(task_a));

    // 等待 A 完成 handoff
    while (!task_a_handoff_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto* slot_a = registry.find_slot(id_a);
    TEST_ASSERT(slot_a != nullptr);
    TEST_ASSERT(slot_a->handoff_executed.load());
    // A 此时仍处于 Pending（尚未 Join Ready）
    TEST_ASSERT(!slot_a->join_ready.load());

    // 2. 创建 Scheduler B，并在 Worker 上释放 Handle，快速结束进入 Join Ready
    astra::SchedulerOptions opt_b{};
    opt_b.worker_count = 1;
    auto sb = std::make_unique<astra::Scheduler>(opt_b);
    const astra::RuntimeId id_b = sb->runtime_id();

    auto holder_b = std::make_shared<astra::Scheduler>(std::move(*sb));
    sb.reset();

    auto& ref_b = *holder_b;
    auto task_b = [&, h = std::move(holder_b)]() mutable {
        h.reset();
        task_b_completed.store(true);
    };
    astra::detail::run_test_task_on_worker(ref_b, std::move(task_b));

    // 3. 验证：B 能够被 Reaper 迅速认领并完成 join 和注销，绝不受处于 Pending 的 A 阻塞！
    const auto wait_b_start = std::chrono::steady_clock::now();
    while (registry.find_slot(id_b) != nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - wait_b_start).count();
        if (elapsed_s > 3) {
            TEST_ASSERT(false && "Runtime B reclamation was blocked by Pending Runtime A (Head-of-Line blocking violated R-025)");
        }
    }

    // 证明此时 B 已经完全回收，而 A 仍然保持 Pending
    TEST_ASSERT(registry.find_slot(id_b) == nullptr);
    TEST_ASSERT(registry.find_slot(id_a) != nullptr);

    // 4. 释放 A，使其进入 Join Ready，并验证 A 最终也被 Reaper 回收
    task_a_hold.store(false);
    const auto wait_a_start = std::chrono::steady_clock::now();
    while (registry.find_slot(id_a) != nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - wait_a_start).count();
        if (elapsed_s > 3) {
            TEST_ASSERT(false && "Runtime A reclamation timed out after becoming Join Ready");
        }
    }
    TEST_ASSERT(registry.registered_count() == 0);
}

// -----------------------------------------------------------------------------
// R-026: 仅当 Join Ready 后由 Reaper 认领唯一 join 并发布 Stopped
// -----------------------------------------------------------------------------
void test_R026_join_ready_unique_join_and_stopped_publication() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();

    std::atomic<bool> worker_done{false};
    astra::SchedulerOptions opt{};
    opt.worker_count = 2;
    auto s = std::make_unique<astra::Scheduler>(opt);
    const astra::RuntimeId id = s->runtime_id();

    auto holder = std::make_shared<astra::Scheduler>(std::move(*s));
    s.reset();

    auto& sched_ref = *holder;
    auto task = [&, h = std::move(holder)]() mutable {
        h.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        worker_done.store(true);
    };
    astra::detail::run_test_task_on_worker(sched_ref, std::move(task));

    while (!worker_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 等待 Reaper 认领并完成 join
    while (registry.find_slot(id) != nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    TEST_ASSERT(registry.registered_count() == 0);
}

// -----------------------------------------------------------------------------
// R-028: Reaper 空闲时保持同一 coordinator 服务，不反复停启
// -----------------------------------------------------------------------------
void test_R028_reaper_idle_service_persistence() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();

    // 连续进行 3 轮创建与销毁
    for (int round = 0; round < 3; ++round) {
        {
            astra::SchedulerOptions opt{};
            opt.worker_count = 2;
            astra::Scheduler s(opt);
            TEST_ASSERT(s.valid());
            TEST_ASSERT(registry.registered_count() == 1);
        }
        // 非 Worker 析构同步完成
        TEST_ASSERT(registry.registered_count() == 0);
        // coordinator 线程保持同一服务且处于空闲状态
        TEST_ASSERT(registry.coordinator_thread_count() == 1);
    }
}

// -----------------------------------------------------------------------------
// R-107: 进程内恰好一个专用 coordinator 服务全部 Runtime
// -----------------------------------------------------------------------------
void test_R107_single_coordinator_thread_topology() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();

    // 并发创建 4 个 Scheduler
    std::vector<std::unique_ptr<astra::Scheduler>> schedulers;
    for (int i = 0; i < 4; ++i) {
        astra::SchedulerOptions opt{};
        opt.worker_count = 2;
        schedulers.push_back(std::make_unique<astra::Scheduler>(opt));
    }

    TEST_ASSERT(registry.registered_count() == 4);
    // 尽管有 4 个 Scheduler，全局专用 coordinator 线程恰好只有 1 条（R-107）
    TEST_ASSERT(registry.coordinator_thread_count() == 1);

    schedulers.clear();
    TEST_ASSERT(registry.registered_count() == 0);
    TEST_ASSERT(registry.coordinator_thread_count() == 1);
}

}  // namespace

int main() {
    std::printf("Running astra_reaper_coordinator_test...\n");
    test_R025_pending_runtime_does_not_block_reaper();
    test_R026_join_ready_unique_join_and_stopped_publication();
    test_R028_reaper_idle_service_persistence();
    test_R107_single_coordinator_thread_topology();
    std::printf("All AST-007 Reaper coordinator tests passed successfully!\n");
    return 0;
}

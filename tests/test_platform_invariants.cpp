// AST-052 / R-107 / R-111 — 平台与单 implementation instance 进程不变量测试。
// 验证：多 Scheduler 不增加 Reaper coordinator 数量；进程共享 ID/Process
// Metrics/Finalization 域；Linux-only 编译护栏（export.hpp）在受支持平台通过。

#include "astra/finalization.hpp"
#include "astra/process_metrics.hpp"
#include "astra/scheduler.hpp"
#include "reaper_registry.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using astra::RuntimeId;

// 1. 三个并发 Scheduler：coordinator 恰好一个（R-107）。
void test_single_coordinator_with_multiple_schedulers() {
    astra::SchedulerOptions opts{};
    opts.worker_count = 2;
    astra::Scheduler s1(opts);
    astra::Scheduler s2(opts);
    astra::Scheduler s3(opts);

    assert(astra::detail::ReaperRegistry::instance().coordinator_thread_count() == 1);

    auto snap = astra::process_metrics_snapshot();
    assert(snap.service_state == astra::ProcessServiceState::Active);
    assert(snap.gauges.registered_runtimes == 3);
    assert(snap.counters.runtime_registrations == 3);

    // ID domain 共享：runtime_id 进程内唯一不重复。
    assert(s1.runtime_id() != s2.runtime_id());
    assert(s2.runtime_id() != s3.runtime_id());
    assert(s1.runtime_id() != s3.runtime_id());

    // 每个 Scheduler 独立执行任务（coordinator 不执行用户任务/不参与 steal）。
    auto h1 = s1.submit([] { return 1; });
    auto h2 = s2.submit([] { return 2; });
    auto h3 = s3.submit([] { return 3; });
    assert(h1.get() + h2.get() + h3.get() == 6);

    s1.shutdown();
    s2.shutdown();
    s3.shutdown();

    // 空闲保持（D-022）：全部回收后 coordinator 不停止不重建，仍恰好一个。
    assert(astra::detail::ReaperRegistry::instance().coordinator_thread_count() == 1);

    std::cout << "[PASS] test_single_coordinator_with_multiple_schedulers" << std::endl;
}

// 2. Startup rollback：启动失败的 Runtime 撤销注册，gauge 回落且不新增 coordinator。
void test_startup_rollback_keeps_single_coordinator() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    const std::size_t before = registry.registered_count();

    registry.inject_handoff_reservation_failure(true);
    bool threw = false;
    try {
        astra::SchedulerOptions opts{};
        opts.worker_count = 1;
        astra::Scheduler bad(opts);
        (void)bad;
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    assert(threw);
    registry.inject_handoff_reservation_failure(false);

    assert(registry.registered_count() == before);
    assert(registry.coordinator_thread_count() == 1);

    std::cout << "[PASS] test_startup_rollback_keeps_single_coordinator" << std::endl;
}

// 3. Finalization gate 进程唯一：begin 后关闭注册，新 Scheduler 被拒绝；
//    空集合收尾后 coordinator 退出（R-035/R-107，最后执行）。
void test_finalization_gate_is_process_unique() {
    auto control = astra::begin_finalization();
    bool rejected = false;
    try {
        astra::SchedulerOptions opts{};
        opts.worker_count = 1;
        astra::Scheduler late(opts);
        (void)late;
    } catch (const astra::scheduler_creation_rejected&) {
        rejected = true;
    }
    assert(rejected);

    assert(control.wait_for(std::chrono::milliseconds(5000)) ==
           astra::FinalizationWaitResult::Completed);

    auto snap = astra::process_metrics_snapshot();
    assert(snap.finalization_state == astra::ProcessFinalizationState::Finalized);
    assert(snap.gauges.registered_runtimes == 0);
    // Finalized 后保留终值且不重启服务。
    auto snap2 = astra::process_metrics_snapshot();
    assert(snap2.finalization_state == astra::ProcessFinalizationState::Finalized);

    std::cout << "[PASS] test_finalization_gate_is_process_unique" << std::endl;
}

}  // namespace

int main() {
    std::cout << "Running astra_platform_invariants_test..." << std::endl;

    test_single_coordinator_with_multiple_schedulers();
    test_startup_rollback_keeps_single_coordinator();
    test_finalization_gate_is_process_unique();

    std::cout << "All AST-052 platform invariant tests passed successfully!" << std::endl;
    return 0;
}

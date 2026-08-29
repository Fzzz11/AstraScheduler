// AST-044 / R-095 / D-148 — side-effect-free Process Metrics 测试。
// 验证 process_metrics_snapshot() 的固定 counter/gauge、状态与时长字段，
// 以及查询不初始化 Reaper/Finalization 服务、Finalized 后保留终值。

#include "astra/finalization.hpp"
#include "astra/process_metrics.hpp"
#include "astra/scheduler.hpp"
#include "reaper_registry.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

namespace {

// -----------------------------------------------------------------------------
// 1. 未创建 Scheduler 时查询零值且无线程副作用 (R-095 / D-148)
//    本用例必须在 main 中最先执行。
// -----------------------------------------------------------------------------
void test_R095_notstarted_zero_and_no_thread_side_effect() {
    auto snap = astra::process_metrics_snapshot();

    assert(snap.schema_version == 1);
    assert(snap.service_state == astra::ProcessServiceState::NotStarted);
    assert(snap.finalization_state == astra::ProcessFinalizationState::NotStarted);
    assert(snap.counters.runtime_registrations == 0);
    assert(snap.counters.runtime_handoffs == 0);
    assert(snap.counters.runtimes_joined == 0);
    assert(snap.counters.finalization_begin_calls == 0);
    assert(snap.counters.finalization_wait_timeouts == 0);
    assert(snap.counters.finalization_escalations == 0);
    assert(snap.gauges.registered_runtimes == 0);
    assert(snap.gauges.pending_runtimes == 0);
    assert(snap.gauges.join_ready_runtimes == 0);
    assert(snap.finalization_elapsed_ns == 0);
    assert(snap.finalization_completion_duration_ns == 0);
    assert(!snap.saturated);

    // 查询本身不得初始化 Reaper coordinator 线程（D-148 不可重启生命周期）。
    assert(astra::detail::ReaperRegistry::instance().coordinator_thread_count() == 0);

    std::cout << "[PASS] test_R095_notstarted_zero_and_no_thread_side_effect" << std::endl;
}

// -----------------------------------------------------------------------------
// 2. Active 阶段的 registration gauge 与累计 counter (R-095)
// -----------------------------------------------------------------------------
void test_R095_active_registration_and_handoff_join() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    auto owned = std::make_unique<astra::Scheduler>(opts);
    // 唯一 Handle 移入 holder：之后由 Worker 销毁最后 Handle 触发 orphan handoff。
    auto holder = std::make_shared<astra::Scheduler>(std::move(*owned));
    owned.reset();

    auto active = astra::process_metrics_snapshot();
    assert(active.service_state == astra::ProcessServiceState::Active);
    assert(active.finalization_state == astra::ProcessFinalizationState::NotStarted);
    assert(active.counters.runtime_registrations >= 1);
    assert(active.gauges.registered_runtimes >= 1);

    // Worker 线程销毁最后 Handle（R-021 / R-022）：异步 orphan handoff 交给 Reaper。
    auto& sched_ref = *holder;
    auto task = [h = std::move(holder)]() mutable {
        h.reset();
    };
    astra::detail::run_test_task_on_worker(sched_ref, std::move(task));

    // Reaper 完成 handoff 与 join 后 gauges 归零，累计 counter 增加。
    bool drained = false;
    for (int i = 0; i < 2000; ++i) {
        auto s = astra::process_metrics_snapshot();
        if (s.gauges.registered_runtimes == 0 && s.gauges.pending_runtimes == 0 &&
            s.gauges.join_ready_runtimes == 0 && s.counters.runtime_handoffs >= 1 &&
            s.counters.runtimes_joined >= 1) {
            drained = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    assert(drained);

    std::cout << "[PASS] test_R095_active_registration_and_handoff_join" << std::endl;
}

// -----------------------------------------------------------------------------
// 3. Finalizing：wait timeout / escalation counter 与 elapsed (R-095 / D-148)
// -----------------------------------------------------------------------------
void test_R095_finalizing_timeout_and_escalation() {
    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    astra::Scheduler sched(opts);

    std::atomic<bool> gate{true};
    auto h = sched.submit([&gate] {
        while (gate.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return 7;
    });

    auto control = astra::begin_finalization();
    auto fin = astra::process_metrics_snapshot();
    assert(fin.counters.finalization_begin_calls >= 1);
    assert(fin.service_state == astra::ProcessServiceState::Finalizing);
    assert(fin.finalization_state == astra::ProcessFinalizationState::Finalizing);

    // 阻塞任务尚未退出，限时等待必须真实返回 TimedOut 并累计 counter。
    assert(control.wait_for(5ms) == astra::FinalizationWaitResult::TimedOut);
    auto timed = astra::process_metrics_snapshot();
    assert(timed.counters.finalization_wait_timeouts >= 1);
    assert(timed.finalization_state == astra::ProcessFinalizationState::Finalizing);
    assert(timed.finalization_elapsed_ns > 0);
    assert(timed.finalization_completion_duration_ns == 0);

    // 升级为 Immediate 并累计 escalation counter。
    control.request_immediate();
    auto esc = astra::process_metrics_snapshot();
    assert(esc.counters.finalization_escalations >= 1);

    gate.store(false, std::memory_order_release);
    control.wait();
    h.wait();

    // Finalized：保留终值，重复查询稳定且不新增隐藏行为。
    auto final1 = astra::process_metrics_snapshot();
    assert(final1.service_state == astra::ProcessServiceState::Finalized);
    assert(final1.finalization_state == astra::ProcessFinalizationState::Finalized);
    assert(final1.gauges.registered_runtimes == 0);
    assert(final1.gauges.pending_runtimes == 0);
    assert(final1.gauges.join_ready_runtimes == 0);
    assert(final1.finalization_completion_duration_ns > 0);
    assert(final1.finalization_elapsed_ns == final1.finalization_completion_duration_ns);

    std::this_thread::sleep_for(5ms);
    auto final2 = astra::process_metrics_snapshot();
    assert(final2.counters.runtime_registrations == final1.counters.runtime_registrations);
    assert(final2.counters.runtime_handoffs == final1.counters.runtime_handoffs);
    assert(final2.counters.runtimes_joined == final1.counters.runtimes_joined);
    assert(final2.counters.finalization_begin_calls == final1.counters.finalization_begin_calls);
    assert(final2.counters.finalization_wait_timeouts == final1.counters.finalization_wait_timeouts);
    assert(final2.counters.finalization_escalations == final1.counters.finalization_escalations);
    assert(final2.finalization_completion_duration_ns == final1.finalization_completion_duration_ns);

    // Process Metrics 不聚合 Runtime task counters：schema 中不存在此类字段，
    // 此处仅验证 task 结果未被 process snapshot 观察污染。
    assert(h.get() == 7);

    std::cout << "[PASS] test_R095_finalizing_timeout_and_escalation" << std::endl;
}

// -----------------------------------------------------------------------------
// 4. 逐字段安全：快速重复快照不崩溃且 capture 时间单调 (R-095 / D-137 语义)
// -----------------------------------------------------------------------------
void test_R095_snapshot_field_safety() {
    for (int i = 0; i < 1000; ++i) {
        auto s = astra::process_metrics_snapshot();
        assert(s.capture_finished_at >= s.capture_started_at);
        assert(s.schema_version == 1);
    }
    std::cout << "[PASS] test_R095_snapshot_field_safety" << std::endl;
}

}  // namespace

int main() {
    std::cout << "Running astra_process_metrics_test..." << std::endl;

    test_R095_notstarted_zero_and_no_thread_side_effect();
    test_R095_active_registration_and_handoff_join();
    test_R095_finalizing_timeout_and_escalation();
    test_R095_snapshot_field_safety();

    std::cout << "All AST-044 process metrics tests passed successfully!" << std::endl;
    return 0;
}

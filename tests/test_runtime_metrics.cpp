#include "astra/coroutine.hpp"
#include "astra/graph.hpp"
#include "astra/metrics.hpp"
#include "astra/scheduler.hpp"
#include "astra/task_handle.hpp"
#include "astra/task_options.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// -----------------------------------------------------------------------------
// 1. MetricsLevel::Off 模式：enabled == false，全部指标为 0，元数据有效 (R-084 / D-135 / D-151)
// -----------------------------------------------------------------------------
void test_R084_metrics_level_off() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Off;
    astra::Scheduler sched(opts);

    auto h1 = sched.submit([] { return 42; });
    assert(h1.get() == 42);

    auto snap = sched.metrics_snapshot();
    assert(!snap.enabled);
    assert(!snap.saturated);
    assert(snap.schema_version == 1);
    assert(snap.runtime_id == sched.runtime_id());
    assert(snap.worker_count == 2);
    assert(snap.metrics_level == astra::MetricsLevel::Off);
    assert(snap.scheduler_state == astra::SchedulerState::Running);
    assert(snap.capture_finished_at >= snap.capture_started_at);

    // 验证所有 counter 为 0
    assert(snap.counters.submission_attempts == 0);
    assert(snap.counters.accepted_task_identities == 0);
    assert(snap.counters.first_starts == 0);
    assert(snap.counters.succeeded == 0);
    assert(snap.counters.failed == 0);
    assert(snap.counters.unobserved_failures == 0);

    // 验证所有 gauge 为 0
    assert(snap.gauges.waiting_tasks == 0);
    assert(snap.gauges.ready_tasks == 0);
    assert(snap.gauges.running_tasks == 0);
    assert(snap.gauges.suspended_tasks == 0);

    // D-151: 即使产生未观测失败，Off 模式下也不记录 unobserved_failures
    {
        auto h_fail = sched.submit([]() -> int {
            throw std::runtime_error("unobserved error in Off mode");
        });
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(10ms);

    auto snap2 = sched.metrics_snapshot();
    assert(!snap2.enabled);
    assert(snap2.counters.unobserved_failures == 0);

    std::cout << "[PASS] test_R084_metrics_level_off" << std::endl;
}

// -----------------------------------------------------------------------------
// 2. MetricsLevel::Basic 模式：基础计数与单调性 (R-084 / D-135 / D-136)
// -----------------------------------------------------------------------------
void test_R084_metrics_basic_counters_and_monotonicity() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Basic;
    astra::Scheduler sched(opts);

    auto snap0 = sched.metrics_snapshot();
    assert(snap0.enabled);
    assert(!snap0.saturated);
    assert(snap0.worker_count == 2);
    assert(snap0.metrics_level == astra::MetricsLevel::Basic);

    // 提交一批成功与失败任务
    std::vector<astra::TaskHandle<int>> handles;
    for (int i = 0; i < 10; ++i) {
        handles.push_back(sched.submit([i] { return i * 2; }));
    }

    for (int i = 0; i < 10; ++i) {
        assert(handles[i].get() == i * 2);
    }

    auto snap1 = sched.metrics_snapshot();
    assert(snap1.enabled);
    assert(snap1.counters.submission_attempts >= 10);
    assert(snap1.counters.accepted_task_identities >= 10);
    assert(snap1.counters.first_starts >= 10);
    assert(snap1.counters.succeeded >= 10);

    // 再执行一些任务，检验单调性
    auto h_fail = sched.submit([]() -> int {
        throw std::runtime_error("expected error");
    });
    try {
        h_fail.get();
        assert(false && "should have thrown");
    } catch (const std::runtime_error&) {}

    auto snap2 = sched.metrics_snapshot();
    assert(snap2.counters.submission_attempts >= snap1.counters.submission_attempts);
    assert(snap2.counters.accepted_task_identities >= snap1.counters.accepted_task_identities);
    assert(snap2.counters.first_starts >= snap1.counters.first_starts);
    assert(snap2.counters.succeeded >= snap1.counters.succeeded);
    assert(snap2.counters.failed >= 1);

    std::cout << "[PASS] test_R084_metrics_basic_counters_and_monotonicity" << std::endl;
}

// -----------------------------------------------------------------------------
// 3. Unobserved Failures 计数测试 (R-084 / D-151)
// -----------------------------------------------------------------------------
void test_R084_unobserved_failures() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Basic;
    astra::Scheduler sched(opts);

    {
        auto h_unobserved = sched.submit([]() -> int {
            throw std::runtime_error("unobserved failure");
        });
        // 等待任务在 worker 上执行并失败
        std::this_thread::sleep_for(30ms);
        // handle 在此处析构且未被 get()/exception() 观测
    }

    std::this_thread::sleep_for(10ms);

    auto snap = sched.metrics_snapshot();
    assert(snap.counters.unobserved_failures >= 1);
    assert(snap.counters.failed >= 1);

    std::cout << "[PASS] test_R084_unobserved_failures" << std::endl;
}

// -----------------------------------------------------------------------------
// 4. Deadline 指标统计测试 (R-084 / D-135)
// -----------------------------------------------------------------------------
void test_R084_deadline_metrics() {
    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    opts.metrics_level = astra::MetricsLevel::Basic;
    astra::Scheduler sched(opts);

    const auto now = std::chrono::steady_clock::now();

    // 1. Deadline Met 任务
    astra::TaskOptions opt_met;
    opt_met.deadline = astra::TaskDeadline::at(now + 2s);
    auto h_met = sched.submit(opt_met, [] { return 100; });
    assert(h_met.get() == 100);

    // 2. Deadline Missed 任务（提交时 deadline 已过期）
    astra::TaskOptions opt_missed;
    opt_missed.deadline = astra::TaskDeadline::at(now - 100ms);
    auto h_missed = sched.submit(opt_missed, [] { return 200; });
    assert(h_missed.get() == 200);

    auto snap = sched.metrics_snapshot();
    assert(snap.counters.deadline_admitted >= 2);
    assert(snap.counters.deadline_met >= 1);
    assert(snap.counters.deadline_missed >= 1);

    std::cout << "[PASS] test_R084_deadline_metrics" << std::endl;
}

// -----------------------------------------------------------------------------
// 5. 协程与定时器指标统计测试 (R-084 / D-135)
// -----------------------------------------------------------------------------
void test_R084_coroutine_and_timer_metrics() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Basic;
    astra::Scheduler sched(opts);

    auto coro_fn = []() -> astra::Task<int> {
        co_await astra::yield();
        co_await astra::sleep_for(10ms);
        co_return 777;
    };

    auto h = sched.spawn(coro_fn());
    assert(h.get() == 777);

    auto snap = sched.metrics_snapshot();
    assert(snap.counters.coroutine_suspends >= 1);
    assert(snap.counters.resume_segments >= 1);
    assert(snap.counters.explicit_yields >= 1);
    assert(snap.counters.timer_registrations >= 1);
    assert(snap.counters.timer_fires >= 1);
    assert(snap.gauges.suspended_tasks == 0);

    std::cout << "[PASS] test_R084_coroutine_and_timer_metrics" << std::endl;
}

// -----------------------------------------------------------------------------
// 6. 图调度指标统计测试 (R-084 / D-135)
// -----------------------------------------------------------------------------
void test_R084_graph_metrics() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Basic;
    astra::Scheduler sched(opts);

    astra::TaskGraph g;
    auto n1 = g.emplace([] {});
    auto n2 = g.emplace([] {});
    g.add_edge(n1, n2);

    auto run = sched.run(std::move(g).freeze());
    run.wait();
    auto report = run.get_report();
    assert(report.succeeded_nodes == 2);
    assert(report.failed_nodes == 0);
    assert(report.cancelled_nodes == 0);

    auto snap = sched.metrics_snapshot();
    assert(snap.counters.graph_admission_attempts >= 1);
    assert(snap.counters.graph_runs_accepted >= 1);
    assert(snap.counters.graph_nodes_terminal >= 2);
    assert(snap.gauges.active_graph_runs == 0);

    std::cout << "[PASS] test_R084_graph_metrics" << std::endl;
}

// -----------------------------------------------------------------------------
// 7. 关停生命周期与快照一致性测试 (R-084 / D-135)
// -----------------------------------------------------------------------------
void test_R084_shutdown_and_snapshot_metadata() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Basic;
    astra::Scheduler sched(opts);

    auto snap_running = sched.metrics_snapshot();
    assert(snap_running.scheduler_state == astra::SchedulerState::Running);

    sched.shutdown();

    auto snap_stopped = sched.metrics_snapshot();
    assert(snap_stopped.scheduler_state == astra::SchedulerState::Stopped);
    assert(snap_stopped.shutdown_mode == astra::ShutdownMode::Graceful);

    // 停机后再尝试提交，记录 rejected_lifecycle
    try {
        sched.submit([] {});
        assert(false && "submit after shutdown must throw");
    } catch (const astra::submission_rejected&) {}

    auto snap_rejected = sched.metrics_snapshot();
    assert(snap_rejected.counters.rejected_lifecycle >= 1);

    std::cout << "[PASS] test_R084_shutdown_and_snapshot_metadata" << std::endl;
}

}  // namespace

int main() {
    std::cout << "Starting AstraScheduler Runtime Metrics Tests (AST-042)..." << std::endl;

    test_R084_metrics_level_off();
    test_R084_metrics_basic_counters_and_monotonicity();
    test_R084_unobserved_failures();
    test_R084_deadline_metrics();
    test_R084_coroutine_and_timer_metrics();
    test_R084_graph_metrics();
    test_R084_shutdown_and_snapshot_metadata();

    std::cout << "All AST-042 Runtime Metrics Tests PASSED!" << std::endl;
    return 0;
}

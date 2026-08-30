#ifndef ASTRA_METRICS_HPP
#define ASTRA_METRICS_HPP

// AstraScheduler 运行时指标模型与快照定义（AST-042 / R-084 / D-135 / D-136 / D-151）。
// 仅公开低基数固定 schema，不包含 TaskId、NodeId 或任意字符串 label。
// 【通俗说明】这是"运行时指标"的定义文件：submit/task/队列/协程/定时器/
// 图/优先级/截止时间等每类事件各有一个计数器，Detailed 模式额外记录延迟
// 直方图（64 个 2 的幂分桶，省内存且足够看分布）。Scheduler::metrics_snapshot()
// 把所有分片原子计数累加成一份不可变快照。设计取舍：计数器是低基数固定
// 字段（不是任意标签），Off 模式零开销，Basic 只累计数，Detailed 才记分布。

#include <astra/export.hpp>
#include <astra/id.hpp>
#include <astra/scheduler_options.hpp>
#include <astra/status.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace astra {

// -----------------------------------------------------------------------------
// Log2Histogram (R-085 / D-137)
// 固定 64 个 base-2 纳秒 bucket 的饱和直方图
// -----------------------------------------------------------------------------
struct Log2Histogram {
    static constexpr std::size_t kBucketCount = 64;
    std::uint64_t count{0};
    std::uint64_t sum_ns{0};
    std::uint64_t max_ns{0};
    std::array<std::uint64_t, kBucketCount> buckets{};

    static constexpr std::size_t bucket_for_ns(std::uint64_t ns) noexcept {
        if (ns <= 1) return 0;
        const std::size_t k = 63 - __builtin_clzll(ns);
        return (k > 63) ? 63 : k;
    }
};

// -----------------------------------------------------------------------------
// Runtime Metrics Histograms (R-085 / D-137)
// Detailed 模式下的延迟直方图
// -----------------------------------------------------------------------------
struct RuntimeMetricsHistograms {
    Log2Histogram ready_queue_wait{};
    Log2Histogram execution_segment{};
    Log2Histogram task_wall_time{};
    Log2Histogram blocking_admission_wait{};
    Log2Histogram timer_wake_lateness{};
    Log2Histogram deadline_start_lateness{};
    Log2Histogram worker_park_duration{};
    Log2Histogram runtime_join_latency{};
    // R-096 / D-149：wait/await 时长（Detailed）
    Log2Histogram thread_wait_duration{};
    Log2Histogram helping_wait_duration{};
    Log2Histogram coroutine_await_duration{};
};

// -----------------------------------------------------------------------------
// Runtime Metrics Counters (R-084 / D-136)
// 包含固定低基数生命周期、准入、调度、协程/定时器、图、Deadline 累计 counter
// -----------------------------------------------------------------------------
struct RuntimeMetricsCounters {
    // 准入（Admission）
    std::uint64_t submission_attempts{0};
    std::uint64_t accepted_task_identities{0};
    std::uint64_t rejected_lifecycle{0};
    std::uint64_t rejected_capacity{0};
    std::uint64_t blocking_submit_waits{0};
    std::uint64_t blocking_submit_wakeups{0};

    // 执行与结果（Execution / Outcome）
    std::uint64_t first_starts{0};
    std::uint64_t resume_segments{0};
    std::uint64_t succeeded{0};
    std::uint64_t failed{0};
    std::uint64_t cancelled_before_start{0};
    std::uint64_t cancelled_cooperative{0};
    std::uint64_t unobserved_failures{0};

    // 调度行为（Scheduling）
    std::uint64_t global_claims{0};
    std::uint64_t local_claims{0};
    std::uint64_t steal_attempts{0};
    std::uint64_t steal_successes{0};
    std::uint64_t steal_failures{0};
    std::uint64_t worker_parks{0};
    std::uint64_t worker_wakes{0};
    std::uint64_t explicit_yields{0};

    // 协程与定时器（Coroutine / Timer）
    std::uint64_t coroutine_suspends{0};
    std::uint64_t timer_registrations{0};
    std::uint64_t timer_fires{0};
    std::uint64_t timer_cancellations{0};

    // 同步等待与 Await 诊断（R-096 / D-149）
    std::uint64_t task_wait_calls{0};
    std::uint64_t graph_wait_calls{0};
    std::uint64_t wait_for_timeouts{0};
    std::uint64_t same_runtime_helping_waits{0};
    std::uint64_t cross_runtime_helping_waits{0};
    std::uint64_t coroutine_await_registrations{0};
    std::uint64_t direct_self_wait_rejections{0};
    std::uint64_t helping_depth_rejections{0};

    // 任务图（Task Graph）
    std::uint64_t graph_admission_attempts{0};
    std::uint64_t graph_runs_accepted{0};
    std::uint64_t graph_runs_rejected{0};
    std::uint64_t graph_nodes_terminal{0};

    // 截止时间（Task Deadline）
    std::uint64_t deadline_admitted{0};
    std::uint64_t deadline_met{0};
    std::uint64_t deadline_missed{0};
    std::uint64_t deadline_cancelled_before_start{0};
};

// -----------------------------------------------------------------------------
// Runtime Metrics Gauges (R-084 / D-136)
// 当前状态投影 gauge 瞬时值
// -----------------------------------------------------------------------------
struct RuntimeMetricsGauges {
    std::uint64_t waiting_tasks{0};
    std::uint64_t ready_tasks{0};
    std::uint64_t running_tasks{0};
    std::uint64_t suspended_tasks{0};
    std::uint64_t external_pending_slots_used{0};
    std::uint64_t parked_workers{0};
    std::uint64_t active_timer_entries{0};
    std::uint64_t active_graph_runs{0};
};

// Scheduler::metrics_snapshot() 返回的不可变拷贝。不含 TaskId/字符串标签。
// Off 时 counters/histograms 为零且 enabled=false。saturated 表示计数触顶（R-084）。
struct RuntimeMetricsSnapshot {
    std::uint32_t schema_version{1};
    RuntimeId runtime_id{};
    std::chrono::steady_clock::time_point capture_started_at{};
    std::chrono::steady_clock::time_point capture_finished_at{};
    MetricsLevel metrics_level{MetricsLevel::Basic};
    std::size_t worker_count{0};
    SchedulerState scheduler_state{SchedulerState::Running};
    ShutdownMode shutdown_mode{ShutdownMode::Graceful};
    bool saturated{false};
    bool enabled{true};
    RuntimeMetricsCounters counters{};
    RuntimeMetricsGauges gauges{};
    RuntimeMetricsHistograms histograms{};
};

}  // namespace astra

#endif  // ASTRA_METRICS_HPP

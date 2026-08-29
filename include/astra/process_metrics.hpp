#ifndef ASTRA_PROCESS_METRICS_HPP
#define ASTRA_PROCESS_METRICS_HPP

// AstraScheduler 进程级 Process Metrics 快照（AST-044 / R-095 / D-148）。
// 只观察进程级 Reaper/Finalization 协调器生命周期：查询无副作用、不初始化服务、
// Finalized 后保留终值，不聚合每 Runtime task metrics（后者由 R-084/R-085 提供）。
// 低频控制面统计始终开启，不受任一 Scheduler 的 MetricsLevel 影响。

#include <astra/export.hpp>

#include <chrono>
#include <cstdint>

namespace astra {

// 进程级协调器服务状态（D-148 scope variants）。
enum class ProcessServiceState : std::uint8_t {
    NotStarted = 0,  // Reaper 从未初始化：零事实且查询不得创建线程/注册表
    Active = 1,      // 协调器存活且注册门禁开放
    Finalizing = 2,  // 注册已永久关闭，Runtime 回收进行中
    Finalized = 3,   // Finalization 完成：保留终态终值，绝不重启
};

// 进程级 Finalization 状态（D-148）。
enum class ProcessFinalizationState : std::uint8_t {
    NotStarted = 0,  // 尚未 begin_finalization
    Finalizing = 1,
    Finalized = 2,
};

// 固定 process 级累计 counter（R-095 / D-148）。
struct ProcessMetricsCounters {
    std::uint64_t runtime_registrations{0};
    std::uint64_t runtime_handoffs{0};
    std::uint64_t runtimes_joined{0};
    std::uint64_t finalization_begin_calls{0};
    std::uint64_t finalization_wait_timeouts{0};
    std::uint64_t finalization_escalations{0};
};

// 固定 process 级瞬时 gauge（R-095 / D-148）。
struct ProcessMetricsGauges {
    std::uint64_t registered_runtimes{0};
    std::uint64_t pending_runtimes{0};
    std::uint64_t join_ready_runtimes{0};
};

// 不可变逐字段 process 快照（R-095）。
// finalization_elapsed_ns：Finalizing 期间自 begin 起的流逝纳秒，其它阶段为
// Finalized 终值；finalization_completion_duration_ns 仅在 Finalized 后非零并冻结。
struct ProcessMetricsSnapshot {
    std::uint32_t schema_version{1};
    ProcessServiceState service_state{ProcessServiceState::NotStarted};
    ProcessFinalizationState finalization_state{ProcessFinalizationState::NotStarted};
    std::chrono::steady_clock::time_point capture_started_at{};
    std::chrono::steady_clock::time_point capture_finished_at{};
    std::chrono::steady_clock::time_point finalization_started_at{};
    std::uint64_t finalization_elapsed_ns{0};
    std::uint64_t finalization_completion_duration_ns{0};
    bool saturated{false};
    ProcessMetricsCounters counters{};
    ProcessMetricsGauges gauges{};
};

// 无参、side-effect-free 的进程级快照入口（R-095 / D-148）。
// 可在创建任何 Scheduler 或 begin_finalization 之前调用：返回 NotStarted/零，
// 绝不初始化 Reaper 线程、Runtime 注册或 FinalizationControl。
[[nodiscard]] ASTRA_EXPORT ProcessMetricsSnapshot process_metrics_snapshot() noexcept;

}  // namespace astra

#endif  // ASTRA_PROCESS_METRICS_HPP

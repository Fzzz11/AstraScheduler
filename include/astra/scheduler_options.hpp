#ifndef ASTRA_SCHEDULER_OPTIONS_HPP
#define ASTRA_SCHEDULER_OPTIONS_HPP

// AstraScheduler 配置选项（AST-004 / R-098 / D-078 / D-157）。
// 仅公开稳定语义 policy，并在 Scheduler startup 时冻结。
// Supported Configuration 仅 64-bit Linux（R-111，经 export.hpp 检查）。

#include <astra/export.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace astra {

// 推荐 Worker 数量：取 hardware_concurrency 提示，为 0 时 fallback 为 1（D-157）。
[[nodiscard]] ASTRA_EXPORT std::size_t recommended_worker_count() noexcept;

// External Pending 队列满时的背压策略（D-084 / D-157）。
enum class ExternalBackpressure : std::uint8_t {
    Reject,
    Block
};

// 运行时指标收集级别（D-135 / D-157）。
enum class MetricsLevel : std::uint8_t {
    Off,
    Basic,
    Detailed
};

// 前向声明追踪收集器（D-138 / D-157）。
class TraceCollector;

// Scheduler 构造配置（D-157）。
struct SchedulerOptions {
    std::size_t worker_count = recommended_worker_count();
    std::size_t external_pending_capacity = 65536;
    ExternalBackpressure external_backpressure = ExternalBackpressure::Reject;
    std::size_t max_helping_depth = 64;
    std::size_t local_burst_limit = 64;
    std::size_t steal_probe_limit = 8;
    MetricsLevel metrics_level = MetricsLevel::Basic;
    std::shared_ptr<TraceCollector> trace_collector{};
};

}  // namespace astra

#endif  // ASTRA_SCHEDULER_OPTIONS_HPP

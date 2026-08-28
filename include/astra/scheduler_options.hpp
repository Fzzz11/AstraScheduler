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

// Scheduler 构造配置（D-157）。仅公开稳定语义 policy，并在 startup 时冻结。
struct SchedulerOptions {
    // Worker 线程数量；默认取 hardware_concurrency 提示（为 0 时 fallback 为 1）（D-157 / D-078）。
    std::size_t worker_count = recommended_worker_count();
    // External Pending 队列的容量上限，达到上限时触发 external_backpressure 策略（D-157 / D-084）。
    std::size_t external_pending_capacity = 65536;
    // External Pending 队列满时的背压策略：拒绝提交或阻塞提交方（D-084 / D-157）。
    ExternalBackpressure external_backpressure = ExternalBackpressure::Reject;
    // 单个 Worker 在等待同 Runtime 任务时的最大协助（help）递归深度（D-157 / D-020）。
    std::size_t max_helping_depth = 64;
    // 单个 Worker 单次从本地队列连续取出的任务突发上限，用于平衡局部性与全局公平（D-157）。
    std::size_t local_burst_limit = 64;
    // Work-stealing 时单次探测（probe）其他 Worker 队列的尝试次数上限（D-157 / D-026）。
    std::size_t steal_probe_limit = 8;
    // 运行时指标收集级别：关闭 / 基础 / 详细（D-135 / D-157）。
    MetricsLevel metrics_level = MetricsLevel::Basic;
    // 可选的前向声明追踪收集器；为空表示不启用 trace 采集（D-138 / D-157）。
    std::shared_ptr<TraceCollector> trace_collector{};
};

}  // namespace astra

#endif  // ASTRA_SCHEDULER_OPTIONS_HPP

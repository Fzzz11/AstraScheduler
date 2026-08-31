#ifndef ASTRA_SRC_RUNTIME_RUNTIME_DIAGNOSTICS_HPP
#define ASTRA_SRC_RUNTIME_RUNTIME_DIAGNOSTICS_HPP

#include "runtime/runtime_metrics.hpp"

#include <astra/id.hpp>
#include <astra/trace.hpp>

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace astra {

struct TraceSlot;

namespace detail {

class TaskControlBlock;
struct GraphRunSharedState;

// Runtime 可观测性的单一入口：聚合 metrics，并独占 trace producer 附加状态。
// 调度与生命周期代码不再理解 TraceSlot 的选择和事件编码细节（R-127/R-130）。
class ASTRA_NO_EXPORT RuntimeDiagnostics final {
public:
    RuntimeDiagnostics(RuntimeId runtime_id, RuntimeMetrics& metrics) noexcept;

    void attach(
        const std::shared_ptr<TraceCollector>& collector,
        std::size_t worker_count);

    [[nodiscard]] bool tracing_enabled() const noexcept;

    void emit_wait_event(
        bool from_current_worker,
        std::size_t worker_index,
        TraceEventKind kind,
        TaskId source,
        TaskId target,
        GraphRunId graph_target,
        std::uint16_t reason) noexcept;

    RuntimeId runtime_id;
    RuntimeMetrics& metrics;

private:
    [[nodiscard]] TraceSlot* select_slot(
        bool from_current_worker,
        std::size_t worker_index) noexcept;

    std::shared_ptr<TraceCollector> trace_collector_;
    std::vector<TraceSlot*> worker_slots_;
    TraceSlot* external_slot_{nullptr};
};

[[nodiscard]] RuntimeDiagnostics* current_worker_diagnostics() noexcept;
void emit_wait_trace_event(
    RuntimeDiagnostics* diagnostics,
    TraceEventKind kind,
    TaskId source,
    TaskId target,
    GraphRunId graph_target,
    std::uint16_t reason) noexcept;

struct WaitDiagnosticsGuard final {
    RuntimeDiagnostics* diagnostics;
    TaskId source;
    TaskId target;
    GraphRunId graph_target;
    std::chrono::steady_clock::time_point begin;
    const std::optional<std::chrono::steady_clock::time_point>& deadline;
    const TaskControlBlock* task_state;
    const GraphRunSharedState* graph_state;
    bool helping;

    ~WaitDiagnosticsGuard() noexcept;
};

}  // namespace detail

}  // namespace astra

#endif  // ASTRA_SRC_RUNTIME_RUNTIME_DIAGNOSTICS_HPP

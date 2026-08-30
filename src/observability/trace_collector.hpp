#ifndef ASTRA_SRC_TRACE_COLLECTOR_HPP
#define ASTRA_SRC_TRACE_COLLECTOR_HPP

// TraceCollector 内部 buffer/generation 与 producer/emit seam
// （AST-045 / R-086 / D-138 / D-158）。仅由 Scheduler startup、Reaper 与测试使用；
// public API 见 <astra/trace.hpp>。

#include <astra/id.hpp>
#include <astra/trace.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace astra {

// 单代固定容量 ticket buffer：emit 仅做 ticket fetch_add + slot 写入，
// 满时 drop-newest 计 loss；无消费者同步（stop 在 quiesce 后统一 drain）。
struct ASTRA_NO_EXPORT TraceBuffer {
    explicit TraceBuffer(std::size_t capacity)
        : capacity(capacity), slots(capacity) {}

    std::size_t capacity;
    std::vector<TraceEvent> slots;
    std::atomic<std::uint64_t> next_ticket{0};
    std::atomic<std::uint64_t> dropped{0};
};

// 单一活动 capture generation：代际隔离锚点。上一代 late emitter 通过
// entry->generation 与 current 比对被拒绝写入新一代 buffer。
struct ASTRA_NO_EXPORT TraceGeneration {
    TraceOptions options{};
    TraceCategory mask{TraceCategory::Default};
    std::chrono::steady_clock::time_point origin{};
    std::atomic<bool> active{true};
    std::atomic<std::size_t> inflight{0};
};

// 槽位与某一代 buffer 的绑定；entry->generation 实现代际隔离检查。
struct ASTRA_NO_EXPORT TraceBufferEntry {
    std::shared_ptr<TraceGeneration> generation;
    std::shared_ptr<TraceBuffer> buffer;
};

// 单个 producer 槽位：由 Collector 持有至析构，地址稳定，可直接进入 emit 热路径。
struct ASTRA_NO_EXPORT TraceSlot {
    TraceProducerKind kind{TraceProducerKind::Worker};
    RuntimeId runtime_id{};
    std::uint32_t worker_index{0};
    std::uint64_t producer_id{0};
    std::size_t index{0};
    std::atomic<std::shared_ptr<TraceBufferEntry>> entry;
};

namespace detail {

// Scheduler startup 附加（D-158）：为该 Runtime 的全部 Worker 注册独立 producer，
// 并确保 external/control 与 Reaper 独立 producer 槽位存在；Collector 处于
// Recording 时在 startup barrier 前完成全部 buffer 预分配，失败抛出使 startup
// rollback。collector 为空则 no-op。worker_slots 按序输出每 Worker 槽位。
void trace_attach_runtime(const std::shared_ptr<TraceCollector>& collector,
                          RuntimeId runtime_id, std::size_t worker_count,
                          std::vector<TraceSlot*>* worker_slots = nullptr,
                          TraceSlot** external_slot = nullptr);

// 最小 producer/emit seam：测试与后续事件接线使用。
// emit 无分配、无 I/O、无用户 callback、无阻塞；Stopped/category disabled/
// 无 buffer 为 fast no-op 且不计 drop；buffer 满 drop-newest 计 loss。
[[nodiscard]] TraceSlot* trace_open_worker_producer(TraceCollector& collector,
                                                    RuntimeId runtime_id, std::uint32_t worker_index);
[[nodiscard]] TraceSlot* trace_open_external_producer(TraceCollector& collector);
[[nodiscard]] TraceSlot* trace_open_reaper_producer(TraceCollector& collector);

void trace_emit(TraceCollector& collector, TraceSlot* slot, TraceCategory category,
                std::uint16_t kind, RuntimeId runtime_id, std::uint32_t worker_id,
                std::uint64_t task_id) noexcept;

// 完整 identity/枚举 emit（R-087 / D-139）：category 由 kind→category 固定
// 映射推导，与 TraceOptions mask 一致；缺失字段为零值 sentinel。
struct ASTRA_NO_EXPORT TraceEmitDesc {
    TraceEventKind kind{TraceEventKind::Admission};
    RuntimeId runtime_id{};
    std::uint64_t task_sequence{0};
    RuntimeId target_runtime_id{};
    std::uint64_t target_task_sequence{0};
    std::uint64_t graph_run_sequence{0};
    std::uint32_t node_id{0};
    std::uint32_t worker_id{0};
    std::uint32_t segment_sequence{0};
    std::uint16_t priority{0};
    std::uint16_t source{0};
    std::uint16_t task_state{0};
    std::uint16_t outcome{0};
    std::uint16_t reason{0};
    std::uint16_t deadline_disposition{0};
};

void trace_emit_desc(TraceCollector& collector, TraceSlot* slot, const TraceEmitDesc& desc) noexcept;

}  // namespace detail

}  // namespace astra

#endif  // ASTRA_SRC_TRACE_COLLECTOR_HPP

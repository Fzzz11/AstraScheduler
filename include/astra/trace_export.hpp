#ifndef ASTRA_TRACE_EXPORT_HPP
#define ASTRA_TRACE_EXPORT_HPP

// AstraScheduler Chrome Trace 离线确定性导出（AST-047 / R-088 / R-109 / D-140）。
// 只有 Stopped TraceSnapshot 可导出；core 不接受文件路径、不在 Runtime 线程
// 写文件；导出失败不影响 Runtime 或原 snapshot，可重试到其他 ostream。

#include <astra/export.hpp>
#include <astra/trace.hpp>

#include <cstdint>
#include <iosfwd>

namespace astra {

// 导出结果（写入 JSON 顶层 metadata 的同一组事实）。
// trace_complete=true 仅表示 collector 未报告 drop/schema corruption；
// 不表示应用没有未追踪 Runtime 或关闭 category。
/** @brief Chrome Trace 导出的结果及 loss 摘要。 */
struct ChromeTraceExportResult {
    bool trace_complete{false};
    std::uint64_t recorded_events{0};
    std::uint64_t dropped_events{0};
    std::uint64_t schema_gaps{0};
};

// 将 Stopped snapshot 按 (timestamp_ns, producer_id, local_sequence) 确定 merge
// 后导出为 Chrome Trace Event JSON。损坏输入（未知 enum/category 不一致/identity
// 违规/空 snapshot）抛 std::invalid_argument，不生成伪造闭合事件；任意 loss 仍
// 输出语法有效 JSON 但 trace_complete=false。相同 snapshot 与 exporter 版本产生
// byte-for-byte 确定输出（pretty_print 只是用户选择的格式变体）。
/**
 * @brief 将已停止的 TraceSnapshot 确定性导出为 Chrome Trace JSON。
 * @param snapshot 要导出的 capture 快照。
 * @param out 接收 JSON 的输出流。
 * @param pretty_print 是否输出缩进格式。
 * @return 导出事件数、丢失数及完整性标记。
 */
[[nodiscard]] ASTRA_EXPORT ChromeTraceExportResult write_chrome_trace(
    const TraceSnapshot& snapshot, std::ostream& out, bool pretty_print = false);

}  // namespace astra

#endif  // ASTRA_TRACE_EXPORT_HPP

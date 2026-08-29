#include <astra/trace_export.hpp>

#include <astra/trace.hpp>

#include <cstdint>
#include <map>
#include <ostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace astra {

// ============================================================================
// Chrome Trace exporter（AST-047 / R-088 / D-140）
// ----------------------------------------------------------------------------
// - 仅接受 Stopped TraceSnapshot（active capture 无 snapshot 可导出）。
// - 按 (timestamp_ns, producer_id, local_sequence) 确定 merge；时间换算使用
//   整数除法/余数（ns → µs + 3 位小数），不使用浮点与 locale 依赖格式化。
// - Task execution segment 使用 ph:"X" duration event（dur 来自已记录的
//   start/resume 时间戳）；Admission/Ready→claim 与 Coroutine suspend→resume
//   使用 ph:"s"/"f" flow event，flow id 由稳定逻辑 identity 派生。
// - 绝不合成事件掩盖缺口：无法解析 start 的 segment end 降级为 instant 并计
//   schema gap → trace_complete=false；enum/category/identity 损坏直接抛出。
// - 输出只写入调用方提供的 ostream：无 path、无文件打开、无 logger 调用
//   （R-109：Trace/Logging 独立 sink，导出不递归进入日志系统）。
// ============================================================================

namespace {

constexpr const char* kExporterVersion = "1.0";
constexpr const char* kFormatName = "chrome_trace_event";

const char* trace_event_kind_name(std::uint16_t kind) {
    switch (static_cast<TraceEventKind>(kind)) {
        case TraceEventKind::Admission: return "admission";
        case TraceEventKind::Rejected: return "rejected";
        case TraceEventKind::TaskReady: return "task_ready";
        case TraceEventKind::TaskClaimed: return "task_claimed";
        case TraceEventKind::TaskFirstStart: return "task_first_start";
        case TraceEventKind::TaskSegmentEnd: return "task_segment_end";
        case TraceEventKind::TaskTerminal: return "task_terminal";
        case TraceEventKind::CancelRequested: return "cancel_requested";
        case TraceEventKind::LocalClaim: return "local_claim";
        case TraceEventKind::GlobalClaim: return "global_claim";
        case TraceEventKind::StealSuccess: return "steal_success";
        case TraceEventKind::StealAttempt: return "steal_attempt";
        case TraceEventKind::WorkerPark: return "worker_park";
        case TraceEventKind::WorkerWake: return "worker_wake";
        case TraceEventKind::CoroutineSuspend: return "coroutine_suspend";
        case TraceEventKind::CoroutineResume: return "coroutine_resume";
        case TraceEventKind::CoroutineYield: return "coroutine_yield";
        case TraceEventKind::TimerRegister: return "timer_register";
        case TraceEventKind::TimerFire: return "timer_fire";
        case TraceEventKind::TimerCancel: return "timer_cancel";
        case TraceEventKind::GraphAccepted: return "graph_accepted";
        case TraceEventKind::GraphTerminal: return "graph_terminal";
        case TraceEventKind::NodeDependencyRelease: return "node_dependency_release";
        case TraceEventKind::DeadlineMet: return "deadline_met";
        case TraceEventKind::DeadlineMissed: return "deadline_missed";
        case TraceEventKind::WaitBegin: return "wait_begin";
        case TraceEventKind::WaitEnd: return "wait_end";
        case TraceEventKind::AwaitArmed: return "await_armed";
        case TraceEventKind::AwaitTriggered: return "await_triggered";
        case TraceEventKind::AwaitResumed: return "await_resumed";
        case TraceEventKind::RuntimeHandoff: return "runtime_handoff";
        case TraceEventKind::RuntimeJoinReady: return "runtime_join_ready";
        case TraceEventKind::RuntimeJoined: return "runtime_joined";
        case TraceEventKind::FinalizationBegin: return "finalization_begin";
        case TraceEventKind::FinalizationEscalate: return "finalization_escalate";
        case TraceEventKind::CoordinatorExit: return "coordinator_exit";
        case TraceEventKind::FinalizationComplete: return "finalization_complete";
    }
    return nullptr;  // unknown kind：由校验阶段显式失败
}

const char* trace_category_name(TraceCategory category) {
    switch (category) {
        case TraceCategory::TaskLifecycle: return "task";
        case TraceCategory::QueueScheduling: return "queue";
        case TraceCategory::StealAttempt: return "steal_attempt";
        case TraceCategory::WaitAwait: return "wait_await";
        case TraceCategory::Coroutine: return "coroutine";
        case TraceCategory::Graph: return "graph";
        case TraceCategory::Timer: return "timer";
        case TraceCategory::Deadline: return "deadline";
        case TraceCategory::RuntimeLifecycle: return "runtime";
        case TraceCategory::Verbose: return "verbose";
        default: return nullptr;
    }
}

const char* producer_kind_name(TraceProducerKind kind) {
    switch (kind) {
        case TraceProducerKind::Worker: return "worker";
        case TraceProducerKind::ExternalControl: return "external_control";
        case TraceProducerKind::Reaper: return "reaper";
    }
    return "unknown";
}

// 所有数字经 std::to_string 输出（C locale、无分组），保证 byte 稳定。
void append_ns_as_us(std::string& out, std::uint64_t ns) {
    out += std::to_string(ns / 1000);
    out += '.';
    const std::uint64_t fraction = ns % 1000;
    if (fraction < 100) out += '0';
    if (fraction < 10) out += '0';
    out += std::to_string(fraction);
}

void append_hex64(std::string& out, std::uint64_t value) {
    static const char* kDigits = "0123456789abcdef";
    char buf[17];
    for (int i = 15; i >= 0; --i) {
        buf[i] = kDigits[value & 0xF];
        value >>= 4;
    }
    buf[16] = '\0';
    out += buf;
}

// JSON 字符串转义：即使内容来自固定 schema 也正确 escape（D-140 invariant）。
void append_json_string(std::string& out, const char* text) {
    out += '"';
    for (const char* p = text; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

bool is_flow_start_kind(std::uint16_t kind) {
    return kind == static_cast<std::uint16_t>(TraceEventKind::Admission) ||
           kind == static_cast<std::uint16_t>(TraceEventKind::TaskReady) ||
           kind == static_cast<std::uint16_t>(TraceEventKind::CoroutineSuspend) ||
           kind == static_cast<std::uint16_t>(TraceEventKind::AwaitArmed);
}

bool is_flow_finish_kind(std::uint16_t kind) {
    return kind == static_cast<std::uint16_t>(TraceEventKind::TaskClaimed) ||
           kind == static_cast<std::uint16_t>(TraceEventKind::LocalClaim) ||
           kind == static_cast<std::uint16_t>(TraceEventKind::GlobalClaim) ||
           kind == static_cast<std::uint16_t>(TraceEventKind::StealSuccess) ||
           kind == static_cast<std::uint16_t>(TraceEventKind::CoroutineResume) ||
           kind == static_cast<std::uint16_t>(TraceEventKind::AwaitTriggered) ||
           kind == static_cast<std::uint16_t>(TraceEventKind::AwaitResumed);
}

bool is_segment_end_kind(std::uint16_t kind) {
    return kind == static_cast<std::uint16_t>(TraceEventKind::TaskSegmentEnd) ||
           kind == static_cast<std::uint16_t>(TraceEventKind::TaskTerminal);
}

bool is_segment_start_kind(std::uint16_t kind) {
    return kind == static_cast<std::uint16_t>(TraceEventKind::TaskFirstStart) ||
           kind == static_cast<std::uint16_t>(TraceEventKind::CoroutineResume) ||
           kind == static_cast<std::uint16_t>(TraceEventKind::AwaitResumed);
}

}  // namespace

ChromeTraceExportResult write_chrome_trace(const TraceSnapshot& snapshot, std::ostream& out,
                                           bool pretty_print) {
    if (!snapshot) {
        throw std::invalid_argument("write_chrome_trace requires a stopped TraceSnapshot");
    }

    const std::vector<TraceEvent> events = trace_ordered_events(snapshot);

    // --- 校验：enum / identity / segment（损坏 snapshot 显式失败，不伪造闭合）---
    for (const TraceEvent& ev : events) {
        if (ev.schema_version != 1) {
            throw std::invalid_argument("trace event carries unsupported schema_version");
        }
        if (trace_event_kind_name(ev.kind) == nullptr) {
            throw std::invalid_argument("trace event carries unknown EventKind");
        }
        const auto category = category_for_kind(static_cast<TraceEventKind>(ev.kind));
        if (category == TraceCategory::None ||
            ev.category != static_cast<std::uint16_t>(static_cast<std::uint64_t>(category) & 0xFFFFu)) {
            throw std::invalid_argument("trace event category does not match its EventKind");
        }
        if ((ev.task_id != 0 || ev.graph_run_id != 0 || ev.segment_sequence != 0) && ev.runtime_id == 0) {
            throw std::invalid_argument("task-bearing trace event is missing RuntimeId");
        }
        if (ev.node_id != 0 && ev.graph_run_id == 0) {
            throw std::invalid_argument("trace event carries NodeId without GraphRunId");
        }
        if (ev.segment_sequence != 0 && ev.task_id == 0) {
            throw std::invalid_argument("trace event carries SegmentSequence without TaskId");
        }
    }

    // --- 单次确定性扫描：segment start 跟踪 / per-producer recorded 计数 ---
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, std::uint64_t> segment_start;
    std::map<std::uint64_t, std::uint64_t> recorded_by_producer;
    std::uint64_t schema_gaps = 0;

    std::string json;
    json += "{\"traceEvents\":[";
    const std::string nl = pretty_print ? "\n" : "";
    const std::string sep = pretty_print ? ",\n" : ",";
    if (pretty_print) {
        json += '\n';
    }

    bool first = true;
    for (const TraceEvent& ev : events) {
        if (!first) {
            json += sep;
        }
        first = false;

        recorded_by_producer[ev.producer_id] += 1;

        json += "{\"name\":";
        append_json_string(json, trace_event_kind_name(ev.kind));
        json += ",\"cat\":";
        append_json_string(json, trace_category_name(static_cast<TraceCategory>(
                                     static_cast<std::uint64_t>(ev.category) & 0xFFFFu)));
        json += ",\"ph\":\"";

        std::tuple<std::uint64_t, std::uint64_t, std::uint64_t> seg_key{
            ev.producer_id, ev.runtime_id, ev.task_id};
        if (is_segment_start_kind(ev.kind)) {
            segment_start[seg_key] = ev.timestamp_ns;
        }
        if (is_segment_end_kind(ev.kind)) {
            const auto it = segment_start.find(seg_key);
            if (it != segment_start.end() && ev.timestamp_ns >= it->second) {
                // duration event：dur 来自已记录的 start 时间戳，绝不合成。
                json += "X\",\"ts\":";
                append_ns_as_us(json, it->second);
                json += ",\"dur\":";
                append_ns_as_us(json, ev.timestamp_ns - it->second);
                segment_start.erase(it);
            } else {
                // 无法解析 start：降级为 instant 并计 schema gap（不得伪造 dur）。
                json += "i\",\"ts\":";
                append_ns_as_us(json, ev.timestamp_ns);
                ++schema_gaps;
            }
        } else if (is_flow_start_kind(ev.kind) && ev.task_id != 0) {
            json += "s\",\"ts\":";
            append_ns_as_us(json, ev.timestamp_ns);
            json += ",\"id\":\"0x";
            append_hex64(json, ev.runtime_id);
            append_hex64(json, ev.task_id);
            json += '"';
        } else if (is_flow_finish_kind(ev.kind) && ev.task_id != 0) {
            json += "f\",\"ts\":";
            append_ns_as_us(json, ev.timestamp_ns);
            json += ",\"id\":\"0x";
            append_hex64(json, ev.runtime_id);
            append_hex64(json, ev.task_id);
            json += '"';
        } else {
            json += "i\",\"ts\":";
            append_ns_as_us(json, ev.timestamp_ns);
        }

        json += ",\"pid\":1,\"tid\":";
        json += std::to_string(ev.producer_id);
        json += ",\"args\":{\"producer\":";
        json += std::to_string(ev.producer_id);
        json += ",\"sequence\":";
        json += std::to_string(ev.local_sequence);
        json += ",\"runtime\":";
        json += std::to_string(ev.runtime_id);
        if (ev.worker_id != 0) {
            json += ",\"worker\":";
            json += std::to_string(ev.worker_id);
        }
        if (ev.task_id != 0) {
            json += ",\"task\":";
            json += std::to_string(ev.task_id);
        }
        if (ev.graph_run_id != 0) {
            json += ",\"graph_run\":";
            json += std::to_string(ev.graph_run_id);
        }
        if (ev.node_id != 0) {
            json += ",\"node\":";
            json += std::to_string(ev.node_id);
        }
        if (ev.segment_sequence != 0) {
            json += ",\"segment\":";
            json += std::to_string(ev.segment_sequence);
        }
        json += "}}";
    }

    // --- 顶层 metadata：schema、options、identity 与 loss totals ---
    const std::uint64_t recorded_total = events.size();
    std::uint64_t dropped_total = 0;
    for (std::size_t i = 0; i < snapshot.producer_count(); ++i) {
        dropped_total += snapshot.producer(i).dropped_events;
    }
    const bool complete = dropped_total == 0 && schema_gaps == 0;

    json += sep;
    if (pretty_print) {
        json += '\n';
    }
    json += "\"metadata\":{\"format\":";
    append_json_string(json, kFormatName);
    json += ",\"exporter_version\":";
    append_json_string(json, kExporterVersion);
    json += ",\"schema_version\":";
    json += std::to_string(snapshot.schema_version());
    json += ",\"event_record_size\":";
    json += std::to_string(snapshot.event_record_size());
    json += ",\"trace_complete\":";
    json += complete ? "true" : "false";
    json += ",\"recorded_events\":";
    json += std::to_string(recorded_total);
    json += ",\"dropped_events\":";
    json += std::to_string(dropped_total);
    json += ",\"schema_gaps\":";
    json += std::to_string(schema_gaps);
    json += ",\"capture_origin_steady_ns\":";
    json += std::to_string(static_cast<std::uint64_t>(
        snapshot.origin().time_since_epoch().count()));
    json += ",\"categories_mask\":\"0x";
    append_hex64(json, static_cast<std::uint64_t>(snapshot.categories()) & 0xFFFFFFFFull);
    json += '"';
    json += ",\"producers\":[";
    for (std::size_t i = 0; i < snapshot.producer_count(); ++i) {
        if (i != 0) {
            json += ',';
        }
        const auto& report = snapshot.producer(i);
        json += "{\"producer_id\":";
        json += std::to_string(report.producer_id);
        json += ",\"kind\":";
        append_json_string(json, producer_kind_name(report.kind));
        json += ",\"capacity\":";
        json += std::to_string(report.capacity);
        json += ",\"recorded\":";
        const auto rit = recorded_by_producer.find(report.producer_id);
        json += std::to_string(rit == recorded_by_producer.end() ? 0ull : rit->second);
        json += ",\"dropped\":";
        json += std::to_string(report.dropped_events);
        json += '}';
    }
    json += "]}";

    if (pretty_print) {
        json += "\n}";
    } else {
        json += '}';
    }

    // 仅写入调用方 ostream：无 path、无文件 I/O、无 logger（R-088/R-109）。
    out.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!out) {
        throw std::runtime_error("write_chrome_trace failed writing to output stream");
    }

    ChromeTraceExportResult result;
    result.trace_complete = complete;
    result.recorded_events = recorded_total;
    result.dropped_events = dropped_total;
    result.schema_gaps = schema_gaps;
    return result;
}

}  // namespace astra

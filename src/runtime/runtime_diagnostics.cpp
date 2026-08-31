#include "runtime/runtime_diagnostics.hpp"

#include "graph/graph_shared_state.hpp"
#include "observability/trace_collector.hpp"
#include "runtime/runtime_registry.hpp"
#include "task/task_control_block.hpp"

#include <astra/task_handle.hpp>

#include <atomic>
#include <chrono>
#include <optional>

namespace astra::detail {

extern thread_local RuntimeId t_current_worker_runtime_id;
extern thread_local void* t_current_worker_impl;
extern thread_local std::size_t t_current_worker_index;

namespace {
RuntimeDiagnostics* find_runtime_diagnostics(RuntimeId id) noexcept {
    return find_runtime_instance(id);
}
}  // namespace

RuntimeDiagnostics::RuntimeDiagnostics(
    RuntimeId runtime_id,
    RuntimeMetrics& metrics) noexcept
    : runtime_id(runtime_id), metrics(metrics) {}

void RuntimeDiagnostics::attach(
    const std::shared_ptr<TraceCollector>& collector,
    std::size_t worker_count) {
    trace_collector_ = collector;
    trace_attach_runtime(
        collector,
        runtime_id,
        worker_count,
        &worker_slots_,
        &external_slot_);
}

bool RuntimeDiagnostics::tracing_enabled() const noexcept {
    return static_cast<bool>(trace_collector_);
}

TraceSlot* RuntimeDiagnostics::select_slot(
    bool from_current_worker,
    std::size_t worker_index) noexcept {
    if (!trace_collector_) {
        return nullptr;
    }
    if (from_current_worker && worker_index < worker_slots_.size()) {
        return worker_slots_[worker_index];
    }
    return external_slot_;
}

void RuntimeDiagnostics::emit_wait_event(
    bool from_current_worker,
    std::size_t worker_index,
    TraceEventKind kind,
    TaskId source,
    TaskId target,
    GraphRunId graph_target,
    std::uint16_t reason) noexcept {
    TraceSlot* slot = select_slot(from_current_worker, worker_index);
    if (!slot || !trace_collector_) {
        return;
    }
    TraceEmitDesc description{};
    description.kind = kind;
    description.runtime_id = runtime_id;
    description.task_sequence = source.valid() ? source.sequence() : 0;
    description.target_runtime_id =
        target.valid() ? RuntimeId{target.runtime_id().value()} : RuntimeId{};
    description.target_task_sequence = target.valid() ? target.sequence() : 0;
    description.graph_run_sequence = graph_target.valid() ? graph_target.sequence() : 0;
    description.reason = reason;
    trace_emit_desc(*trace_collector_, slot, description);
}
void record_metrics_submission_attempt(RuntimeId id) noexcept {
    auto* impl = find_runtime_diagnostics(id);
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    detail::RuntimeMetrics::saturating_inc(impl->metrics.shard_for_current().submission_attempts);
}

void record_metrics_first_start(TaskId id, std::optional<DeadlineDisposition> dl_disp) noexcept {
    auto* impl = find_runtime_diagnostics(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    detail::RuntimeMetrics::saturating_inc(shard.first_starts);
    RuntimeMetrics::saturating_dec(impl->metrics.ready_tasks);
    impl->metrics.running_tasks.fetch_add(1, std::memory_order_relaxed);
    if (dl_disp.has_value()) {
        if (*dl_disp == DeadlineDisposition::Met) {
            detail::RuntimeMetrics::saturating_inc(shard.deadline_met);
        } else if (*dl_disp == DeadlineDisposition::Missed) {
            detail::RuntimeMetrics::saturating_inc(shard.deadline_missed);
        }
    }
}

void record_metrics_succeeded(TaskId id) noexcept {
    auto* impl = find_runtime_diagnostics(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    detail::RuntimeMetrics::saturating_inc(shard.succeeded);
    RuntimeMetrics::saturating_dec(impl->metrics.running_tasks);
}

void record_metrics_failed(TaskId id) noexcept {
    auto* impl = find_runtime_diagnostics(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    detail::RuntimeMetrics::saturating_inc(shard.failed);
    RuntimeMetrics::saturating_dec(impl->metrics.running_tasks);
}

void record_metrics_cancelled_cooperative(TaskId id) noexcept {
    auto* impl = find_runtime_diagnostics(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    detail::RuntimeMetrics::saturating_inc(shard.cancelled_cooperative);
    RuntimeMetrics::saturating_dec(impl->metrics.running_tasks);
}

void record_metrics_cancelled_before_start(TaskId id, bool has_deadline) noexcept {
    auto* impl = find_runtime_diagnostics(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    detail::RuntimeMetrics::saturating_inc(shard.cancelled_before_start);
    RuntimeMetrics::saturating_dec(impl->metrics.ready_tasks);
    if (has_deadline) {
        detail::RuntimeMetrics::saturating_inc(shard.deadline_cancelled_before_start);
    }
}

// ============================================================================
// Wait/Await 诊断辅助（AST-048 / R-096 / D-149）。
// 全部 noexcept、无分配；Metrics Off / Trace 未附加时为零成本 fast path。
// ============================================================================

// wait/await 诊断 trace 事件：source 侧取当前 worker runtime（external caller
// 归属 target runtime 的 external/control lane），target 侧携带逻辑 identity。
RuntimeDiagnostics* current_worker_diagnostics() noexcept {
    if (!t_current_worker_impl || !t_current_worker_runtime_id.valid()) {
        return nullptr;
    }
    return find_runtime_instance(t_current_worker_runtime_id);
}

void emit_wait_trace_event(RuntimeDiagnostics* diagnostics, TraceEventKind kind, TaskId source,
                           TaskId target, GraphRunId graph_target,
                           std::uint16_t reason) noexcept {
    if (!diagnostics) {
        return;
    }
    diagnostics->emit_wait_event(
        current_worker_diagnostics() == diagnostics,
        t_current_worker_index,
        kind,
        source,
        target,
        graph_target,
        reason);
}

// WaitEnd 的 scope-exit 配对：duration histogram、timeout 计数与 trace 配对。
WaitDiagnosticsGuard::~WaitDiagnosticsGuard() noexcept {
    const bool completed =
        task_state ? task_state->is_completed()
                   : graph_state->run_state.load(std::memory_order_acquire) != GraphRunState::Running;
    const auto now = std::chrono::steady_clock::now();
    const bool timed_out = !completed && deadline.has_value() && now >= *deadline;
    const auto duration_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - begin).count());
    if (diagnostics && diagnostics->metrics.level != MetricsLevel::Off) {
        auto& shard = diagnostics->metrics.shard_for_current();
        if (diagnostics->metrics.level == MetricsLevel::Detailed) {
            (helping ? shard.helping_wait_duration : shard.thread_wait_duration)
                .record(duration_ns);
        }
        if (timed_out) {
            RuntimeMetrics::saturating_inc(shard.wait_for_timeouts);
        }
    }
    emit_wait_trace_event(
        diagnostics,
        TraceEventKind::WaitEnd,
        source,
        target,
        graph_target,
        completed ? 1u : (timed_out ? 2u : 0u));
}

void record_metrics_unobserved_failure(TaskId id) noexcept {
    auto* impl = find_runtime_diagnostics(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    detail::RuntimeMetrics::saturating_inc(shard.unobserved_failures);
    // R-060：仅在活动 Trace 可用时尽力发出 unobserved failure 诊断事件。
    emit_wait_trace_event(impl, TraceEventKind::UnobservedFailure, id, TaskId{}, GraphRunId{}, 0);
}

// --- Coroutine await 诊断入口（TaskHandleAwaiter 调用，R-096 / D-149）---

void record_wait_call(TaskId target, bool timed_out) noexcept {
    RuntimeDiagnostics* impl = t_current_worker_impl
                                   ? current_worker_diagnostics()
                                   : find_runtime_diagnostics(target.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    const bool worker = t_current_worker_impl != nullptr;
    if (worker) {
        if (target.runtime_id() == impl->runtime_id) {
            detail::RuntimeMetrics::saturating_inc(shard.same_runtime_helping_waits);
        } else {
            detail::RuntimeMetrics::saturating_inc(shard.cross_runtime_helping_waits);
        }
    } else {
        detail::RuntimeMetrics::saturating_inc(shard.task_wait_calls);
    }
    if (timed_out) {
        detail::RuntimeMetrics::saturating_inc(shard.wait_for_timeouts);
    }
    // 即时已完成等待记录零/最小 bucket（D-149）
    if (impl->metrics.level == MetricsLevel::Detailed) {
        (worker ? shard.helping_wait_duration : shard.thread_wait_duration).record(0);
    }
}

void record_self_wait_rejection(TaskId target) noexcept {
    if (auto* impl = find_runtime_diagnostics(target.runtime_id());
        impl && impl->metrics.level != MetricsLevel::Off) {
        detail::RuntimeMetrics::saturating_inc(
            impl->metrics.shard_for_current().direct_self_wait_rejections);
    }
}

void record_await_registration(TaskId source, TaskId target) noexcept {
    if (auto* impl = find_runtime_diagnostics(target.runtime_id());
        impl && impl->metrics.level != MetricsLevel::Off) {
        detail::RuntimeMetrics::saturating_inc(
            impl->metrics.shard_for_current().coroutine_await_registrations);
    }
    RuntimeDiagnostics* src_impl = t_current_worker_impl
                                       ? current_worker_diagnostics()
                                       : find_runtime_diagnostics(target.runtime_id());
    emit_wait_trace_event(src_impl, TraceEventKind::AwaitArmed, source, target, GraphRunId{}, 0);
}

void record_await_triggered(TaskId source, TaskId target, bool cancelled) noexcept {
    RuntimeDiagnostics* src_impl = t_current_worker_impl
                                       ? current_worker_diagnostics()
                                       : find_runtime_diagnostics(target.runtime_id());
    emit_wait_trace_event(src_impl, TraceEventKind::AwaitTriggered, source, target, GraphRunId{},
                          cancelled ? 2u : 1u);
}

void record_await_resumed(TaskId source, TaskId target, std::uint64_t duration_ns) noexcept {
    if (auto* impl = find_runtime_diagnostics(target.runtime_id());
        impl && impl->metrics.level == MetricsLevel::Detailed) {
        impl->metrics.shard_for_current().coroutine_await_duration.record(duration_ns);
    }
    RuntimeDiagnostics* src_impl = t_current_worker_impl
                                       ? current_worker_diagnostics()
                                       : find_runtime_diagnostics(target.runtime_id());
    emit_wait_trace_event(src_impl, TraceEventKind::AwaitResumed, source, target, GraphRunId{}, 0);
}

void record_metrics_suspended(TaskId id) noexcept {
    auto* impl = find_runtime_diagnostics(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    detail::RuntimeMetrics::saturating_inc(shard.coroutine_suspends);
    RuntimeMetrics::saturating_dec(impl->metrics.running_tasks);
    impl->metrics.suspended_tasks.fetch_add(1, std::memory_order_relaxed);
}

void record_metrics_resumed(TaskId id) noexcept {
    auto* impl = find_runtime_diagnostics(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    RuntimeMetrics::saturating_dec(impl->metrics.suspended_tasks);
    impl->metrics.ready_tasks.fetch_add(1, std::memory_order_relaxed);
}

void record_metrics_resume_segment(TaskId id) noexcept {
    auto* impl = find_runtime_diagnostics(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    detail::RuntimeMetrics::saturating_inc(shard.resume_segments);
    RuntimeMetrics::saturating_dec(impl->metrics.ready_tasks);
    impl->metrics.running_tasks.fetch_add(1, std::memory_order_relaxed);
}

void record_metrics_explicit_yield() noexcept {
    if (t_current_worker_impl != nullptr) {
        auto* impl = current_worker_diagnostics();
        if (impl && impl->metrics.level != MetricsLevel::Off) {
            detail::RuntimeMetrics::saturating_inc(impl->metrics.shard_for_current().explicit_yields);
        }
    }
}

void record_metrics_graph_node_terminal(RuntimeId id) noexcept {
    auto* impl = find_runtime_diagnostics(id);
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    detail::RuntimeMetrics::saturating_inc(impl->metrics.shard_for_current().graph_nodes_terminal);
}

void record_metrics_graph_run_completed(RuntimeId id) noexcept {
    auto* impl = find_runtime_diagnostics(id);
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    RuntimeMetrics::saturating_dec(impl->metrics.active_graph_runs);
}

void record_metrics_ready_queue_wait(TaskId id, std::uint64_t duration_ns) noexcept {
    auto* impl = find_runtime_diagnostics(id.runtime_id());
    if (!impl || impl->metrics.level != MetricsLevel::Detailed) return;
    impl->metrics.shard_for_current().ready_queue_wait.record(duration_ns);
}

void record_metrics_execution_segment(TaskId id, std::uint64_t duration_ns) noexcept {
    auto* impl = find_runtime_diagnostics(id.runtime_id());
    if (!impl || impl->metrics.level != MetricsLevel::Detailed) return;
    impl->metrics.shard_for_current().execution_segment.record(duration_ns);
}

void record_metrics_task_wall_time(TaskId id, std::uint64_t duration_ns) noexcept {
    auto* impl = find_runtime_diagnostics(id.runtime_id());
    if (!impl || impl->metrics.level != MetricsLevel::Detailed) return;
    impl->metrics.shard_for_current().task_wall_time.record(duration_ns);
}

void record_metrics_blocking_admission_wait(RuntimeId id, std::uint64_t duration_ns) noexcept {
    auto* impl = find_runtime_diagnostics(id);
    if (!impl || impl->metrics.level != MetricsLevel::Detailed) return;
    impl->metrics.shard_for_current().blocking_admission_wait.record(duration_ns);
}

void record_metrics_timer_wake_lateness(RuntimeId id, std::uint64_t duration_ns) noexcept {
    auto* impl = find_runtime_diagnostics(id);
    if (!impl || impl->metrics.level != MetricsLevel::Detailed) return;
    impl->metrics.shard_for_current().timer_wake_lateness.record(duration_ns);
}

void record_metrics_deadline_start_lateness(TaskId id, std::uint64_t duration_ns) noexcept {
    auto* impl = find_runtime_diagnostics(id.runtime_id());
    if (!impl || impl->metrics.level != MetricsLevel::Detailed) return;
    impl->metrics.shard_for_current().deadline_start_lateness.record(duration_ns);
}

void record_metrics_worker_park_duration(RuntimeId id, std::uint64_t duration_ns) noexcept {
    auto* impl = find_runtime_diagnostics(id);
    if (!impl || impl->metrics.level != MetricsLevel::Detailed) return;
    impl->metrics.shard_for_current().worker_park_duration.record(duration_ns);
}

void record_metrics_runtime_join_latency(RuntimeId id, std::uint64_t duration_ns) noexcept {
    auto* impl = find_runtime_diagnostics(id);
    if (!impl || impl->metrics.level != MetricsLevel::Detailed) return;
    impl->metrics.shard_for_current().runtime_join_latency.record(duration_ns);
}


}  // namespace astra::detail

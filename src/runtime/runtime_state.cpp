#include "runtime/runtime_state.hpp"

#include <utility>

namespace astra::detail {

RuntimeState::RuntimeState(
    RuntimeId id,
    SchedulerOptions scheduler_options,
    SchedulerCapabilities scheduler_capabilities)
    : runtime_id(id),
      options(std::move(scheduler_options)),
      capabilities(scheduler_capabilities),
      packed_status(pack(SchedulerState::Running, ShutdownMode::None)),
      diagnostics(id, metrics),
      admission(
          options.external_pending_capacity,
          options.external_backpressure,
          packed_status,
          metrics),
      timers(metrics),
      ready_queues(options.worker_count, metrics) {
    metrics.init(options.metrics_level, options.worker_count, runtime_id);
}

void RuntimeState::release_external_slot_after_claim(
    const ReadyQueues::QueuedTask& task) noexcept {
    if (task.is_external) {
        admission.release(1);
    }
}

void RuntimeState::process_due_timers() {
    auto due_items = timers.collect_due(std::chrono::steady_clock::now());
    for (auto& item : due_items) {
        if (item.handshake && item.resume_action) {
            item.handshake->trigger(item.resume_action);
        }
    }
}

std::optional<std::chrono::steady_clock::time_point> RuntimeState::earliest_wake_time() {
    return timers.earliest_wake_time();
}

bool RuntimeState::has_timers() {
    return !timers.empty();
}

}  // namespace astra::detail

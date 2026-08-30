#include "admission_controller.hpp"

#include <chrono>

namespace astra::detail {

AdmissionController::AdmissionController(
    std::size_t capacity,
    ExternalBackpressure backpressure,
    const std::atomic<std::uint16_t>& packed_status,
    RuntimeMetrics& metrics)
    : capacity_(capacity),
      backpressure_(backpressure),
      packed_status_(packed_status),
      metrics_(metrics) {}

SchedulerStatus AdmissionController::unpack(std::uint16_t val) noexcept {
    const auto state = static_cast<SchedulerState>((val >> 8) & 0xFF);
    const auto mode = static_cast<ShutdownMode>(val & 0xFF);
    return SchedulerStatus{state, mode};
}

AdmissionDecision AdmissionController::acquire(std::size_t count, bool block, bool is_internal) {
    if (count == 0) {
        const auto st = unpack(packed_status_.load(std::memory_order_acquire));
        if (st.state == SchedulerState::Stopped) {
            if (metrics_.level != MetricsLevel::Off) {
                RuntimeMetrics::saturating_inc(metrics_.shard_for_current().rejected_lifecycle);
            }
            return AdmissionDecision::Stopped;
        }
        if (st.state == SchedulerState::Stopping &&
            (!is_internal || st.shutdown_mode != ShutdownMode::Graceful)) {
            if (metrics_.level != MetricsLevel::Off) {
                RuntimeMetrics::saturating_inc(metrics_.shard_for_current().rejected_lifecycle);
            }
            return AdmissionDecision::Stopping;
        }
        if (metrics_.level != MetricsLevel::Off) {
            RuntimeMetrics::saturating_add(metrics_.shard_for_current().accepted_task_identities, count);
            metrics_.ready_tasks.fetch_add(count, std::memory_order_relaxed);
        }
        return AdmissionDecision::Success;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        const auto st = unpack(packed_status_.load(std::memory_order_acquire));
        if (st.state == SchedulerState::Stopped) {
            if (metrics_.level != MetricsLevel::Off) {
                RuntimeMetrics::saturating_inc(metrics_.shard_for_current().rejected_lifecycle);
            }
            return AdmissionDecision::Stopped;
        }
        if (st.state == SchedulerState::Stopping) {
            if (is_internal && st.shutdown_mode == ShutdownMode::Graceful) {
                if (metrics_.level != MetricsLevel::Off) {
                    RuntimeMetrics::saturating_add(metrics_.shard_for_current().accepted_task_identities, count);
                    metrics_.ready_tasks.fetch_add(count, std::memory_order_relaxed);
                }
                return AdmissionDecision::Success;
            }
            if (metrics_.level != MetricsLevel::Off) {
                RuntimeMetrics::saturating_inc(metrics_.shard_for_current().rejected_lifecycle);
            }
            return AdmissionDecision::Stopping;
        }

        if (is_internal) {
            if (metrics_.level != MetricsLevel::Off) {
                RuntimeMetrics::saturating_add(metrics_.shard_for_current().accepted_task_identities, count);
                metrics_.ready_tasks.fetch_add(count, std::memory_order_relaxed);
            }
            return AdmissionDecision::Success;
        }

        if (count > capacity_) {
            if (metrics_.level != MetricsLevel::Off) {
                RuntimeMetrics::saturating_inc(metrics_.shard_for_current().rejected_capacity);
            }
            return AdmissionDecision::CapacityExhausted;
        }

        if (pending_ + count <= capacity_) {
            pending_ += count;
            if (metrics_.level != MetricsLevel::Off) {
                RuntimeMetrics::saturating_add(metrics_.shard_for_current().accepted_task_identities, count);
                metrics_.ready_tasks.fetch_add(count, std::memory_order_relaxed);
            }
            return AdmissionDecision::Success;
        }

        if (!block || backpressure_ != ExternalBackpressure::Block) {
            if (metrics_.level != MetricsLevel::Off) {
                RuntimeMetrics::saturating_inc(metrics_.shard_for_current().rejected_capacity);
            }
            return AdmissionDecision::CapacityExhausted;
        }

        if (metrics_.level != MetricsLevel::Off) {
            RuntimeMetrics::saturating_inc(metrics_.shard_for_current().blocking_submit_waits);
        }
        const auto t_wait_start = std::chrono::steady_clock::now();
        slot_cv_.wait(lock, [this, count] {
            const auto current_st = unpack(packed_status_.load(std::memory_order_acquire));
            return current_st.state != SchedulerState::Running ||
                   (pending_ + count <= capacity_);
        });
        const auto t_wait_end = std::chrono::steady_clock::now();
        if (metrics_.level != MetricsLevel::Off) {
            RuntimeMetrics::saturating_inc(metrics_.shard_for_current().blocking_submit_wakeups);
            if (metrics_.level == MetricsLevel::Detailed && t_wait_end >= t_wait_start) {
                metrics_.shard_for_current().blocking_admission_wait.record(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t_wait_end - t_wait_start).count());
            }
        }
    }
}

void AdmissionController::release(std::size_t count) {
    if (count == 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_ >= count) {
            pending_ -= count;
        } else {
            pending_ = 0;
        }
    }
    slot_cv_.notify_all();
}

void AdmissionController::wake_all() {
    slot_cv_.notify_all();
}

std::size_t AdmissionController::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_;
}

}  // namespace astra::detail

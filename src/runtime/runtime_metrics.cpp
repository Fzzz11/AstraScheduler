#include "runtime_metrics.hpp"

#include <astra/id.hpp>

namespace astra::detail {

extern thread_local std::size_t t_current_worker_index;

void RuntimeMetrics::init(MetricsLevel lvl, std::size_t worker_count) {
    level = lvl;
    if (level != MetricsLevel::Off) {
        control_shard = std::make_unique<WorkerShard>();
        worker_shards.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            worker_shards.push_back(std::make_unique<WorkerShard>());
        }
    }
}

RuntimeMetrics::WorkerShard& RuntimeMetrics::shard_for_current() noexcept {
    const std::size_t w_idx = t_current_worker_index;
    if (w_idx < worker_shards.size() && worker_shards[w_idx]) {
        return *worker_shards[w_idx];
    }
    if (control_shard) {
        return *control_shard;
    }
    static WorkerShard fallback_shard;
    return fallback_shard;
}

void RuntimeMetrics::saturating_inc(std::atomic<std::uint64_t>& counter) noexcept {
    std::uint64_t cur = counter.load(std::memory_order_relaxed);
    while (cur < std::numeric_limits<std::uint64_t>::max()) {
        if (counter.compare_exchange_weak(cur, cur + 1, std::memory_order_relaxed)) {
            return;
        }
    }
}

void RuntimeMetrics::saturating_add(std::atomic<std::uint64_t>& counter, std::uint64_t val) noexcept {
    if (val == 0) {
        return;
    }
    std::uint64_t cur = counter.load(std::memory_order_relaxed);
    while (true) {
        std::uint64_t next = (std::numeric_limits<std::uint64_t>::max() - cur < val)
                                 ? std::numeric_limits<std::uint64_t>::max()
                                 : cur + val;
        if (cur == std::numeric_limits<std::uint64_t>::max() ||
            counter.compare_exchange_weak(cur, next, std::memory_order_relaxed)) {
            return;
        }
    }
}

namespace {

void add_to(std::uint64_t& dst, std::uint64_t src, bool& any_saturated) {
    if (src == std::numeric_limits<std::uint64_t>::max() ||
        std::numeric_limits<std::uint64_t>::max() - dst < src) {
        any_saturated = true;
        dst = std::numeric_limits<std::uint64_t>::max();
    } else {
        dst += src;
    }
}

}  // namespace

void RuntimeMetrics::fill_counters_and_histograms(
    RuntimeMetricsSnapshot& snapshot,
    bool& any_saturated) const {
    auto accumulate_shard = [&](const WorkerShard& shard) {
        add_to(snapshot.counters.submission_attempts, shard.submission_attempts.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.accepted_task_identities, shard.accepted_task_identities.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.rejected_lifecycle, shard.rejected_lifecycle.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.rejected_capacity, shard.rejected_capacity.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.blocking_submit_waits, shard.blocking_submit_waits.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.blocking_submit_wakeups, shard.blocking_submit_wakeups.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.first_starts, shard.first_starts.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.resume_segments, shard.resume_segments.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.succeeded, shard.succeeded.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.failed, shard.failed.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.cancelled_before_start, shard.cancelled_before_start.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.cancelled_cooperative, shard.cancelled_cooperative.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.unobserved_failures, shard.unobserved_failures.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.global_claims, shard.global_claims.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.local_claims, shard.local_claims.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.steal_attempts, shard.steal_attempts.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.steal_successes, shard.steal_successes.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.steal_failures, shard.steal_failures.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.worker_parks, shard.worker_parks.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.worker_wakes, shard.worker_wakes.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.explicit_yields, shard.explicit_yields.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.coroutine_suspends, shard.coroutine_suspends.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.timer_registrations, shard.timer_registrations.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.timer_fires, shard.timer_fires.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.timer_cancellations, shard.timer_cancellations.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.graph_admission_attempts, shard.graph_admission_attempts.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.graph_runs_accepted, shard.graph_runs_accepted.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.graph_runs_rejected, shard.graph_runs_rejected.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.graph_nodes_terminal, shard.graph_nodes_terminal.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.deadline_admitted, shard.deadline_admitted.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.deadline_met, shard.deadline_met.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.deadline_missed, shard.deadline_missed.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.deadline_cancelled_before_start, shard.deadline_cancelled_before_start.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.task_wait_calls, shard.task_wait_calls.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.graph_wait_calls, shard.graph_wait_calls.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.wait_for_timeouts, shard.wait_for_timeouts.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.same_runtime_helping_waits, shard.same_runtime_helping_waits.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.cross_runtime_helping_waits, shard.cross_runtime_helping_waits.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.coroutine_await_registrations, shard.coroutine_await_registrations.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.direct_self_wait_rejections, shard.direct_self_wait_rejections.load(std::memory_order_relaxed), any_saturated);
        add_to(snapshot.counters.helping_depth_rejections, shard.helping_depth_rejections.load(std::memory_order_relaxed), any_saturated);
    };

    if (control_shard) {
        accumulate_shard(*control_shard);
    }
    for (const auto& shard_ptr : worker_shards) {
        if (shard_ptr) {
            accumulate_shard(*shard_ptr);
        }
    }

    if (level != MetricsLevel::Detailed) {
        return;
    }

    auto accumulate_hist = [&](Log2Histogram& dst, const WorkerShard::ShardedHistogram& src) {
        add_to(dst.count, src.count.load(std::memory_order_relaxed), any_saturated);
        add_to(dst.sum_ns, src.sum_ns.load(std::memory_order_relaxed), any_saturated);
        const std::uint64_t src_max = src.max_ns.load(std::memory_order_relaxed);
        if (src_max > dst.max_ns) {
            dst.max_ns = src_max;
        }
        for (std::size_t i = 0; i < Log2Histogram::kBucketCount; ++i) {
            add_to(dst.buckets[i], src.buckets[i].load(std::memory_order_relaxed), any_saturated);
        }
    };

    auto accumulate_histograms = [&](const WorkerShard& shard) {
        accumulate_hist(snapshot.histograms.ready_queue_wait, shard.ready_queue_wait);
        accumulate_hist(snapshot.histograms.execution_segment, shard.execution_segment);
        accumulate_hist(snapshot.histograms.task_wall_time, shard.task_wall_time);
        accumulate_hist(snapshot.histograms.blocking_admission_wait, shard.blocking_admission_wait);
        accumulate_hist(snapshot.histograms.timer_wake_lateness, shard.timer_wake_lateness);
        accumulate_hist(snapshot.histograms.deadline_start_lateness, shard.deadline_start_lateness);
        accumulate_hist(snapshot.histograms.worker_park_duration, shard.worker_park_duration);
        accumulate_hist(snapshot.histograms.runtime_join_latency, shard.runtime_join_latency);
        accumulate_hist(snapshot.histograms.thread_wait_duration, shard.thread_wait_duration);
        accumulate_hist(snapshot.histograms.helping_wait_duration, shard.helping_wait_duration);
        accumulate_hist(snapshot.histograms.coroutine_await_duration, shard.coroutine_await_duration);
    };

    if (control_shard) {
        accumulate_histograms(*control_shard);
    }
    for (const auto& shard_ptr : worker_shards) {
        if (shard_ptr) {
            accumulate_histograms(*shard_ptr);
        }
    }
}

void RuntimeMetrics::fill_task_gauges(RuntimeMetricsSnapshot& snapshot) const {
    snapshot.gauges.waiting_tasks = waiting_tasks.load(std::memory_order_relaxed);
    snapshot.gauges.ready_tasks = ready_tasks.load(std::memory_order_relaxed);
    snapshot.gauges.running_tasks = running_tasks.load(std::memory_order_relaxed);
    snapshot.gauges.suspended_tasks = suspended_tasks.load(std::memory_order_relaxed);
    snapshot.gauges.active_graph_runs = active_graph_runs.load(std::memory_order_relaxed);
}

}  // namespace astra::detail

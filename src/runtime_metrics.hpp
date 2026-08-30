#ifndef ASTRA_SRC_RUNTIME_METRICS_HPP
#define ASTRA_SRC_RUNTIME_METRICS_HPP

#include <astra/metrics.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace astra::detail {

class RuntimeMetrics {
public:
    MetricsLevel level{MetricsLevel::Basic};

    struct WorkerShard {
        alignas(64) std::atomic<std::uint64_t> submission_attempts{0};
        std::atomic<std::uint64_t> accepted_task_identities{0};
        std::atomic<std::uint64_t> rejected_lifecycle{0};
        std::atomic<std::uint64_t> rejected_capacity{0};
        std::atomic<std::uint64_t> blocking_submit_waits{0};
        std::atomic<std::uint64_t> blocking_submit_wakeups{0};

        std::atomic<std::uint64_t> first_starts{0};
        std::atomic<std::uint64_t> resume_segments{0};
        std::atomic<std::uint64_t> succeeded{0};
        std::atomic<std::uint64_t> failed{0};
        std::atomic<std::uint64_t> cancelled_before_start{0};
        std::atomic<std::uint64_t> cancelled_cooperative{0};
        std::atomic<std::uint64_t> unobserved_failures{0};

        std::atomic<std::uint64_t> global_claims{0};
        std::atomic<std::uint64_t> local_claims{0};
        std::atomic<std::uint64_t> steal_attempts{0};
        std::atomic<std::uint64_t> steal_successes{0};
        std::atomic<std::uint64_t> steal_failures{0};
        std::atomic<std::uint64_t> worker_parks{0};
        std::atomic<std::uint64_t> worker_wakes{0};
        std::atomic<std::uint64_t> explicit_yields{0};

        std::atomic<std::uint64_t> coroutine_suspends{0};
        std::atomic<std::uint64_t> timer_registrations{0};
        std::atomic<std::uint64_t> timer_fires{0};
        std::atomic<std::uint64_t> timer_cancellations{0};

        std::atomic<std::uint64_t> graph_admission_attempts{0};
        std::atomic<std::uint64_t> graph_runs_accepted{0};
        std::atomic<std::uint64_t> graph_runs_rejected{0};
        std::atomic<std::uint64_t> graph_nodes_terminal{0};

        std::atomic<std::uint64_t> deadline_admitted{0};
        std::atomic<std::uint64_t> deadline_met{0};
        std::atomic<std::uint64_t> deadline_missed{0};
        std::atomic<std::uint64_t> deadline_cancelled_before_start{0};

        std::atomic<std::uint64_t> task_wait_calls{0};
        std::atomic<std::uint64_t> graph_wait_calls{0};
        std::atomic<std::uint64_t> wait_for_timeouts{0};
        std::atomic<std::uint64_t> same_runtime_helping_waits{0};
        std::atomic<std::uint64_t> cross_runtime_helping_waits{0};
        std::atomic<std::uint64_t> coroutine_await_registrations{0};
        std::atomic<std::uint64_t> direct_self_wait_rejections{0};
        std::atomic<std::uint64_t> helping_depth_rejections{0};

        struct ShardedHistogram {
            std::atomic<std::uint64_t> count{0};
            std::atomic<std::uint64_t> sum_ns{0};
            std::atomic<std::uint64_t> max_ns{0};
            std::array<std::atomic<std::uint64_t>, Log2Histogram::kBucketCount> buckets{};

            ShardedHistogram() {
                for (auto& b : buckets) {
                    b.store(0, std::memory_order_relaxed);
                }
            }

            void record(std::uint64_t ns) noexcept {
                RuntimeMetrics::saturating_inc(count);
                RuntimeMetrics::saturating_add(sum_ns, ns);
                std::uint64_t cur_max = max_ns.load(std::memory_order_relaxed);
                while (ns > cur_max && !max_ns.compare_exchange_weak(cur_max, ns, std::memory_order_relaxed)) {}
                const std::size_t b = Log2Histogram::bucket_for_ns(ns);
                RuntimeMetrics::saturating_inc(buckets[b]);
            }
        };

        ShardedHistogram ready_queue_wait;
        ShardedHistogram execution_segment;
        ShardedHistogram task_wall_time;
        ShardedHistogram blocking_admission_wait;
        ShardedHistogram timer_wake_lateness;
        ShardedHistogram deadline_start_lateness;
        ShardedHistogram worker_park_duration;
        ShardedHistogram runtime_join_latency;
        ShardedHistogram thread_wait_duration;
        ShardedHistogram helping_wait_duration;
        ShardedHistogram coroutine_await_duration;
    };

    std::vector<std::unique_ptr<WorkerShard>> worker_shards;
    std::unique_ptr<WorkerShard> control_shard;

    std::atomic<std::uint64_t> waiting_tasks{0};
    std::atomic<std::uint64_t> ready_tasks{0};
    std::atomic<std::uint64_t> running_tasks{0};
    std::atomic<std::uint64_t> suspended_tasks{0};
    std::atomic<std::uint64_t> active_graph_runs{0};

    void init(MetricsLevel lvl, std::size_t worker_count);
    [[nodiscard]] WorkerShard& shard_for_current() noexcept;
    void fill_counters_and_histograms(RuntimeMetricsSnapshot& snapshot, bool& any_saturated) const;
    void fill_task_gauges(RuntimeMetricsSnapshot& snapshot) const;

    static void saturating_inc(std::atomic<std::uint64_t>& counter) noexcept;
    static void saturating_add(std::atomic<std::uint64_t>& counter, std::uint64_t val) noexcept;
};

}  // namespace astra::detail

#endif

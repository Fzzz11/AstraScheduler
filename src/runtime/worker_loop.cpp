// Worker 的 claim/steal/park/execute/drain 完整循环（R-127 / D-177）。
// 实现仅依赖 RuntimeState，并在单独翻译单元中编译。

#include "worker_loop.hpp"
#include "lifecycle/reaper_registry.hpp"
#include "ready_queues.hpp"
#include "runtime_metrics.hpp"
#include "runtime_state.hpp"

#include <astra/id.hpp>
#include <astra/status.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace astra::detail {

namespace {
constexpr int kParkSpinIterations = 16;
}

extern thread_local RuntimeId t_current_worker_runtime_id;
extern thread_local void* t_current_worker_impl;
extern thread_local std::size_t t_current_worker_index;
extern thread_local TaskId t_current_executing_task_id;
extern thread_local std::size_t t_current_helping_depth;

void generate_steal_victims(
    std::size_t self_index,
    std::size_t worker_count,
    std::size_t probe_limit,
    std::uint64_t& rng_state,
    std::vector<std::size_t>& out_victims);

void run_worker_loop(
    RuntimeState& runtime,
    void* owner_impl,
    std::size_t worker_index) {
    using QueuedTask = ReadyQueues::QueuedTask;

    t_current_worker_runtime_id = runtime.runtime_id;
    t_current_worker_impl = owner_impl;
    t_current_worker_index = worker_index;
    t_current_helping_depth = 0;
    t_current_executing_task_id = TaskId{};

    // 等待 startup 栅栏完成或中止。
    {
        std::unique_lock<std::mutex> lock(runtime.lifecycle_mutex);
        ++runtime.workers_ready;
        runtime.startup_cv.notify_all();
        runtime.startup_cv.wait(lock, [&runtime] {
            return runtime.startup_done || runtime.startup_failed || runtime.stop_requested;
        });
        if (runtime.startup_failed || (!runtime.startup_done && runtime.stop_requested)) {
            t_current_worker_runtime_id = RuntimeId{0};
            t_current_worker_impl = nullptr;
            if (runtime.active_workers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                ReaperRegistry::instance().notify_join_ready(runtime.runtime_id);
            }
            return;
        }
    }

    std::size_t consecutive_local_count = 0;
    std::size_t local_calendar_index = 0;
    std::size_t global_calendar_index = 0;
    std::size_t steal_calendar_index = 0;
    std::array<std::size_t, 4> global_deadline_bursts{0, 0, 0, 0};
    std::uint64_t random_state =
        (static_cast<std::uint64_t>(runtime.runtime_id.value()) << 32) ^
        static_cast<std::uint64_t>(worker_index + 1) ^
        0x9E3779B97F4A7C15ULL;
    std::vector<std::size_t> victims;

    while (true) {
        runtime.process_due_timers();

        QueuedTask task;
        bool found_task = false;

        if (consecutive_local_count < runtime.options.local_burst_limit &&
            runtime.ready_queues.claim_local(worker_index, local_calendar_index, task)) {
            found_task = true;
            ++consecutive_local_count;
        }

        if (!found_task) {
            std::unique_lock<std::mutex> lock(runtime.lifecycle_mutex);
            if (runtime.ready_queues.claim_global(
                    global_calendar_index, global_deadline_bursts, task)) {
                found_task = true;
                consecutive_local_count = 0;
                runtime.release_external_slot_after_claim(task);
            } else if (consecutive_local_count >= runtime.options.local_burst_limit) {
                consecutive_local_count = 0;
                lock.unlock();
                if (runtime.ready_queues.claim_local(
                        worker_index, local_calendar_index, task)) {
                    found_task = true;
                    ++consecutive_local_count;
                }
            }
        }

        if (!found_task) {
            generate_steal_victims(
                worker_index,
                runtime.options.worker_count,
                runtime.options.steal_probe_limit,
                random_state,
                victims);
            for (std::size_t victim : victims) {
                if (runtime.metrics.level != MetricsLevel::Off) {
                    RuntimeMetrics::saturating_inc(
                        runtime.metrics.shard_for_current().steal_attempts);
                }
                if (runtime.ready_queues.steal(
                        victim, steal_calendar_index, task)) {
                    if (runtime.metrics.level != MetricsLevel::Off) {
                        RuntimeMetrics::saturating_inc(
                            runtime.metrics.shard_for_current().steal_successes);
                    }
                    found_task = true;
                    consecutive_local_count = 0;
                    break;
                }
                if (runtime.metrics.level != MetricsLevel::Off) {
                    RuntimeMetrics::saturating_inc(
                        runtime.metrics.shard_for_current().steal_failures);
                }
            }
        }

        if (!found_task) {
            runtime.process_due_timers();

            for (int spin = 0; spin < kParkSpinIterations; ++spin) {
#if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
#else
                std::this_thread::yield();
#endif
            }

            runtime.parked_workers.fetch_add(1, std::memory_order_seq_cst);
            const std::uint64_t observed_epoch =
                runtime.work_epoch.load(std::memory_order_acquire);

            {
                std::unique_lock<std::mutex> lock(runtime.lifecycle_mutex);
                const auto next_wake = runtime.earliest_wake_time();
                auto predicate = [&runtime, worker_index, observed_epoch] {
                    if (runtime.stop_requested) {
                        const auto status = RuntimeState::unpack(
                            runtime.packed_status.load(std::memory_order_acquire));
                        if (status.state == SchedulerState::Stopped ||
                            status.shutdown_mode == ShutdownMode::Immediate) {
                            return true;
                        }
                        if (runtime.ready_queues.any_queued_work() ||
                            (runtime.ready_queues.claimed_count() == 0 && !runtime.has_timers())) {
                            return true;
                        }
                    } else {
                        if (runtime.work_epoch.load(std::memory_order_acquire) != observed_epoch ||
                            runtime.ready_queues.any_queued_work() ||
                            !runtime.ready_queues.local_empty(worker_index)) {
                            return true;
                        }
                    }
                    return false;
                };

                if (runtime.metrics.level != MetricsLevel::Off) {
                    RuntimeMetrics::saturating_inc(
                        runtime.metrics.shard_for_current().worker_parks);
                }
                const auto park_started_at = std::chrono::steady_clock::now();
                if (next_wake.has_value()) {
                    runtime.work_cv.wait_until(lock, *next_wake, predicate);
                } else {
                    runtime.work_cv.wait(lock, predicate);
                }
                const auto park_finished_at = std::chrono::steady_clock::now();
                if (runtime.metrics.level != MetricsLevel::Off) {
                    RuntimeMetrics::saturating_inc(
                        runtime.metrics.shard_for_current().worker_wakes);
                    if (runtime.metrics.level == MetricsLevel::Detailed &&
                        park_finished_at >= park_started_at) {
                        runtime.metrics.shard_for_current().worker_park_duration.record(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                park_finished_at - park_started_at).count());
                    }
                }
                runtime.parked_workers.fetch_sub(1, std::memory_order_seq_cst);

                runtime.process_due_timers();

                if (runtime.ready_queues.claim_global(
                        global_calendar_index, global_deadline_bursts, task)) {
                    found_task = true;
                    consecutive_local_count = 0;
                    runtime.release_external_slot_after_claim(task);
                } else if (runtime.ready_queues.claim_local(
                               worker_index, local_calendar_index, task)) {
                    found_task = true;
                    ++consecutive_local_count;
                } else {
                    lock.unlock();
                    generate_steal_victims(
                        worker_index,
                        runtime.options.worker_count,
                        runtime.options.steal_probe_limit,
                        random_state,
                        victims);
                    for (std::size_t victim : victims) {
                        if (runtime.metrics.level != MetricsLevel::Off) {
                            RuntimeMetrics::saturating_inc(
                                runtime.metrics.shard_for_current().steal_attempts);
                        }
                        if (runtime.ready_queues.steal(
                                victim, steal_calendar_index, task)) {
                            if (runtime.metrics.level != MetricsLevel::Off) {
                                RuntimeMetrics::saturating_inc(
                                    runtime.metrics.shard_for_current().steal_successes);
                            }
                            found_task = true;
                            consecutive_local_count = 0;
                            break;
                        }
                        if (runtime.metrics.level != MetricsLevel::Off) {
                            RuntimeMetrics::saturating_inc(
                                runtime.metrics.shard_for_current().steal_failures);
                        }
                    }
                    lock.lock();
                }

                if (!found_task && runtime.stop_requested) {
                    const auto status = RuntimeState::unpack(
                        runtime.packed_status.load(std::memory_order_acquire));
                    if (status.state == SchedulerState::Stopped) {
                        break;
                    }
                    if (status.shutdown_mode == ShutdownMode::Immediate) {
                        const bool has_resumes =
                            runtime.ready_queues.any_resume_work_after_immediate_cleanup();
                        if (!has_resumes && runtime.ready_queues.claimed_count() == 0) {
                            runtime.work_cv.notify_all();
                            break;
                        }
                    } else {
                        const bool has_tasks = runtime.ready_queues.any_queued_work();
                        const bool has_timers = runtime.has_timers();
                        if (!has_tasks && runtime.ready_queues.claimed_count() == 0 && !has_timers) {
                            runtime.work_cv.notify_all();
                            break;
                        }
                    }
                }
            }
        }

        if (found_task && task.invoker) {
            const auto status = runtime.get_status();
            if (status.shutdown_mode == ShutdownMode::Immediate &&
                !task.invoker->is_resume_segment()) {
                task.invoker->cancel_pre_start();
            } else {
                task.invoker->execute();
            }
            runtime.ready_queues.complete_claim();
            runtime.work_cv.notify_all();
        }
    }

    t_current_worker_runtime_id = RuntimeId{0};
    t_current_worker_impl = nullptr;
    t_current_helping_depth = 0;
    t_current_executing_task_id = TaskId{};
    if (runtime.active_workers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        ReaperRegistry::instance().notify_join_ready(runtime.runtime_id);
    }
}

}  // namespace astra::detail

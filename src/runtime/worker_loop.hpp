#ifndef ASTRA_SRC_WORKER_LOOP_HPP
#define ASTRA_SRC_WORKER_LOOP_HPP

// Worker 的 claim/steal/park/execute/drain 完整循环（R-127 / D-177）。
// Runtime 是内部 duck-typed port；模板让私有 Scheduler::Impl 无需泄露到
// public header，同时把并发协议从 scheduler facade 的实现中移出。

#include "lifecycle/reaper_registry.hpp"
#include "runtime_metrics.hpp"

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

template <typename Runtime>
void run_worker_loop(Runtime& runtime, std::size_t worker_index) {
    using QueuedTask = typename Runtime::QueuedTask;

    t_current_worker_runtime_id = runtime.runtime_id;
    t_current_worker_impl = &runtime;
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

    auto& local_deque = *runtime.local_deques[worker_index];
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
            local_deque.pop_back_weighted(local_calendar_index, task)) {
            found_task = true;
            ++consecutive_local_count;
            std::lock_guard<std::mutex> lock(runtime.lifecycle_mutex);
            ++runtime.active_task_count;
        }

        if (!found_task) {
            std::unique_lock<std::mutex> lock(runtime.lifecycle_mutex);
            if (runtime.pop_global_weighted(
                    global_calendar_index, global_deadline_bursts, task)) {
                ++runtime.active_task_count;
                found_task = true;
                consecutive_local_count = 0;
                if (task.is_external) {
                    runtime.admission.release(1);
                }
            } else if (consecutive_local_count >= runtime.options.local_burst_limit) {
                consecutive_local_count = 0;
                lock.unlock();
                if (local_deque.pop_back_weighted(local_calendar_index, task)) {
                    found_task = true;
                    ++consecutive_local_count;
                    std::lock_guard<std::mutex> count_lock(runtime.lifecycle_mutex);
                    ++runtime.active_task_count;
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
                if (victim < runtime.local_deques.size() &&
                    runtime.local_deques[victim]->steal_front_weighted(
                        steal_calendar_index, task)) {
                    if (runtime.metrics.level != MetricsLevel::Off) {
                        RuntimeMetrics::saturating_inc(
                            runtime.metrics.shard_for_current().steal_successes);
                    }
                    found_task = true;
                    consecutive_local_count = 0;
                    std::lock_guard<std::mutex> lock(runtime.lifecycle_mutex);
                    ++runtime.active_task_count;
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

            for (int spin = 0; spin < 16; ++spin) {
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
                auto predicate = [&runtime, &local_deque, observed_epoch] {
                    if (runtime.stop_requested) {
                        const auto status = Runtime::unpack(
                            runtime.packed_status.load(std::memory_order_acquire));
                        if (status.state == SchedulerState::Stopped ||
                            status.shutdown_mode == ShutdownMode::Immediate) {
                            return true;
                        }
                        if (!runtime.global_queues_empty() || !local_deque.empty() ||
                            (runtime.active_task_count == 0 && !runtime.has_timers())) {
                            return true;
                        }
                        for (const auto& deque : runtime.local_deques) {
                            if (deque && !deque->empty()) {
                                return true;
                            }
                        }
                    } else {
                        if (runtime.work_epoch.load(std::memory_order_acquire) != observed_epoch ||
                            !runtime.global_queues_empty() || !local_deque.empty()) {
                            return true;
                        }
                        for (const auto& deque : runtime.local_deques) {
                            if (deque && !deque->empty()) {
                                return true;
                            }
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

                if (runtime.pop_global_weighted(
                        global_calendar_index, global_deadline_bursts, task)) {
                    ++runtime.active_task_count;
                    found_task = true;
                    consecutive_local_count = 0;
                    if (task.is_external) {
                        runtime.admission.release(1);
                    }
                } else if (local_deque.pop_back_weighted(local_calendar_index, task)) {
                    ++runtime.active_task_count;
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
                        if (victim < runtime.local_deques.size() &&
                            runtime.local_deques[victim]->steal_front_weighted(
                                steal_calendar_index, task)) {
                            if (runtime.metrics.level != MetricsLevel::Off) {
                                RuntimeMetrics::saturating_inc(
                                    runtime.metrics.shard_for_current().steal_successes);
                            }
                            found_task = true;
                            consecutive_local_count = 0;
                            std::lock_guard<std::mutex> count_lock(runtime.lifecycle_mutex);
                            ++runtime.active_task_count;
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
                    const auto status = Runtime::unpack(
                        runtime.packed_status.load(std::memory_order_acquire));
                    if (status.state == SchedulerState::Stopped) {
                        break;
                    }
                    if (status.shutdown_mode == ShutdownMode::Immediate) {
                        bool has_resumes = false;
                        for (const auto& queue : runtime.global_injection_queues) {
                            for (const auto& queued : queue) {
                                if (queued.invoker && queued.invoker->is_resume_segment()) {
                                    has_resumes = true;
                                    break;
                                }
                            }
                            if (has_resumes) {
                                break;
                            }
                        }
                        if (!has_resumes && !local_deque.empty()) {
                            has_resumes = true;
                        }
                        if (!has_resumes) {
                            for (const auto& deque : runtime.local_deques) {
                                if (deque && !deque->empty()) {
                                    has_resumes = true;
                                    break;
                                }
                            }
                        }
                        if (!has_resumes && runtime.active_task_count == 0) {
                            runtime.work_cv.notify_all();
                            break;
                        }
                    } else {
                        bool has_tasks =
                            !runtime.global_queues_empty() || !local_deque.empty();
                        if (!has_tasks) {
                            for (const auto& deque : runtime.local_deques) {
                                if (deque && !deque->empty()) {
                                    has_tasks = true;
                                    break;
                                }
                            }
                        }
                        const bool has_timers = runtime.has_timers();
                        if (!has_tasks && runtime.active_task_count == 0 && !has_timers) {
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
            {
                std::lock_guard<std::mutex> lock(runtime.lifecycle_mutex);
                --runtime.active_task_count;
            }
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

#endif  // ASTRA_SRC_WORKER_LOOP_HPP

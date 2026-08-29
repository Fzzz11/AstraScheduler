#include <astra/coroutine.hpp>
#include <astra/scheduler.hpp>
#include "chase_lev_deque.hpp"
#include "graph_shared_state.hpp"
#include "reaper_registry.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace astra {

std::size_t recommended_worker_count() noexcept {
    const unsigned int count = std::thread::hardware_concurrency();
    return count == 0 ? 1u : static_cast<std::size_t>(count);
}

namespace {

void validate_options(const SchedulerOptions& options) {
    if (options.worker_count == 0) {
        throw std::invalid_argument("SchedulerOptions::worker_count must be greater than 0");
    }
    if (options.external_pending_capacity == 0) {
        throw std::invalid_argument("SchedulerOptions::external_pending_capacity must be greater than 0");
    }
    if (options.max_helping_depth == 0) {
        throw std::invalid_argument("SchedulerOptions::max_helping_depth must be greater than 0");
    }
    if (options.local_burst_limit == 0) {
        throw std::invalid_argument("SchedulerOptions::local_burst_limit must be greater than 0");
    }
    if (options.steal_probe_limit == 0) {
        throw std::invalid_argument("SchedulerOptions::steal_probe_limit must be greater than 0");
    }
    if (options.external_backpressure != ExternalBackpressure::Reject &&
        options.external_backpressure != ExternalBackpressure::Block) {
        throw std::invalid_argument("SchedulerOptions::external_backpressure contains unknown enum value");
    }
    if (options.metrics_level != MetricsLevel::Off &&
        options.metrics_level != MetricsLevel::Basic &&
        options.metrics_level != MetricsLevel::Detailed) {
        throw std::invalid_argument("SchedulerOptions::metrics_level contains unknown enum value");
    }
}

RuntimeId allocate_runtime_id() {
    static std::atomic<std::uint64_t> global_runtime_sequence{0};
    std::uint64_t current = global_runtime_sequence.load(std::memory_order_relaxed);
    while (true) {
        if (current == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("RuntimeId sequence exhausted");
        }
        if (global_runtime_sequence.compare_exchange_weak(
                current, current + 1, std::memory_order_relaxed)) {
            return RuntimeId{current + 1};
        }
    }
}

}  // namespace

namespace {
void register_runtime_impl(Scheduler::Impl* impl);
void unregister_runtime_impl(Scheduler::Impl* impl);
Scheduler::Impl* find_runtime_impl(RuntimeId id);
}  // namespace

namespace detail {
extern thread_local RuntimeId t_current_worker_runtime_id;
extern thread_local void* t_current_worker_impl;
extern thread_local std::size_t t_current_worker_index;
extern thread_local TaskId t_current_executing_task_id;
extern thread_local std::size_t t_current_helping_depth;
}  // namespace detail

struct ASTRA_NO_EXPORT Scheduler::Impl : public std::enable_shared_from_this<Scheduler::Impl> {
    RuntimeId runtime_id;
    SchedulerOptions options;
    SchedulerCapabilities capabilities;
    // 单字原子状态，保证 status() 线性化读取成对快照，不发生跨维度撕裂（D-160）。
    std::atomic<std::uint16_t> packed_status;

    // 运行时指标 Tracker（R-084 / D-135 / D-136）
    struct MetricsTracker {
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

            // Detailed 延迟直方图 (R-085 / D-137)
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
                    MetricsTracker::saturating_inc(count);
                    MetricsTracker::saturating_add(sum_ns, ns);
                    std::uint64_t cur_max = max_ns.load(std::memory_order_relaxed);
                    while (ns > cur_max && !max_ns.compare_exchange_weak(cur_max, ns, std::memory_order_relaxed)) {}
                    const std::size_t b = Log2Histogram::bucket_for_ns(ns);
                    MetricsTracker::saturating_inc(buckets[b]);
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
        };

        std::vector<std::unique_ptr<WorkerShard>> worker_shards;
        std::unique_ptr<WorkerShard> control_shard;

        std::atomic<std::uint64_t> waiting_tasks{0};
        std::atomic<std::uint64_t> ready_tasks{0};
        std::atomic<std::uint64_t> running_tasks{0};
        std::atomic<std::uint64_t> suspended_tasks{0};
        std::atomic<std::uint64_t> active_graph_runs{0};

        void init(MetricsLevel lvl, std::size_t worker_count) {
            level = lvl;
            if (level != MetricsLevel::Off) {
                control_shard = std::make_unique<WorkerShard>();
                worker_shards.reserve(worker_count);
                for (std::size_t i = 0; i < worker_count; ++i) {
                    worker_shards.push_back(std::make_unique<WorkerShard>());
                }
            }
        }

        [[nodiscard]] WorkerShard& shard_for_current() noexcept {
            const std::size_t w_idx = detail::t_current_worker_index;
            if (w_idx < worker_shards.size() && worker_shards[w_idx]) {
                return *worker_shards[w_idx];
            }
            if (control_shard) {
                return *control_shard;
            }
            static WorkerShard fallback_shard;
            return fallback_shard;
        }

        static void saturating_inc(std::atomic<std::uint64_t>& counter) noexcept {
            std::uint64_t cur = counter.load(std::memory_order_relaxed);
            while (cur < std::numeric_limits<std::uint64_t>::max()) {
                if (counter.compare_exchange_weak(cur, cur + 1, std::memory_order_relaxed)) {
                    return;
                }
            }
        }

        static void saturating_add(std::atomic<std::uint64_t>& counter, std::uint64_t val) noexcept {
            if (val == 0) return;
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
    } metrics;

    // Worker 同步与生命周期控制
    std::mutex lifecycle_mutex;
    std::condition_variable startup_cv;
    std::condition_variable work_cv;
    std::condition_variable slot_cv;
    bool startup_done{false};
    bool startup_failed{false};
    bool stop_requested{false};
    bool handoff_dispatched{false};
    std::size_t workers_ready{0};
    std::size_t external_pending_count{0};
    std::size_t active_task_count{0};
    std::atomic<std::size_t> active_workers{0};
    std::atomic<std::size_t> parked_workers{0};
    std::atomic<std::uint64_t> work_epoch{0};
    std::vector<std::thread> worker_threads;

    static constexpr std::size_t kPriorityCalendarLength = 15;
    static constexpr std::array<Priority, kPriorityCalendarLength> kPriorityCalendar = {
        Priority::Critical, // 0
        Priority::High,     // 1
        Priority::Critical, // 2
        Priority::Normal,   // 3
        Priority::Critical, // 4
        Priority::High,     // 5
        Priority::Critical, // 6
        Priority::Low,      // 7
        Priority::Critical, // 8
        Priority::High,     // 9
        Priority::Critical, // 10
        Priority::Normal,   // 11
        Priority::Critical, // 12
        Priority::High,     // 13
        Priority::Critical  // 14
    };

    static constexpr std::array<Priority, 4> kFallbackPriorityOrder = {
        Priority::Critical, Priority::High, Priority::Normal, Priority::Low
    };

    struct QueuedTask {
        std::unique_ptr<detail::TaskInvokerBase> invoker;
        bool is_external{false};
    };

    struct EdfEntry {
        TaskDeadline deadline;
        std::uint64_t admission_seq{0};
        QueuedTask task;

        bool operator>(const EdfEntry& other) const noexcept {
            if (deadline != other.deadline) {
                return deadline > other.deadline;
            }
            return admission_seq > other.admission_seq;
        }

        bool operator<(const EdfEntry& other) const noexcept {
            if (deadline != other.deadline) {
                return deadline < other.deadline;
            }
            return admission_seq < other.admission_seq;
        }
    };

    std::atomic<std::uint64_t> global_admission_seq{0};
    std::array<std::vector<EdfEntry>, 4> global_edf_heaps;
    std::array<std::deque<QueuedTask>, 4> global_injection_queues;

    bool pop_from_global_band_locked(std::size_t band_idx, std::size_t& deadline_burst, QueuedTask& out) {
        auto& edf_heap = global_edf_heaps[band_idx];
        auto& fifo_queue = global_injection_queues[band_idx];

        if (edf_heap.empty() && fifo_queue.empty()) {
            return false;
        }

        if (metrics.level != MetricsLevel::Off) {
            MetricsTracker::saturating_inc(metrics.shard_for_current().global_claims);
        }

        if (!edf_heap.empty() && !fifo_queue.empty()) {
            if (deadline_burst < 8) {
                std::pop_heap(edf_heap.begin(), edf_heap.end(), std::greater<EdfEntry>{});
                out = std::move(edf_heap.back().task);
                edf_heap.pop_back();
                ++deadline_burst;
                return true;
            } else {
                out = std::move(fifo_queue.front());
                fifo_queue.pop_front();
                deadline_burst = 0;
                return true;
            }
        } else if (!edf_heap.empty()) {
            std::pop_heap(edf_heap.begin(), edf_heap.end(), std::greater<EdfEntry>{});
            out = std::move(edf_heap.back().task);
            edf_heap.pop_back();
            ++deadline_burst;
            return true;
        } else {
            out = std::move(fifo_queue.front());
            fifo_queue.pop_front();
            deadline_burst = 0;
            return true;
        }
    }

    bool pop_global_weighted(std::size_t& calendar_idx, std::array<std::size_t, 4>& deadline_bursts, QueuedTask& out) {
        const Priority target_p = kPriorityCalendar[calendar_idx % kPriorityCalendarLength];
        const std::size_t target_idx = static_cast<std::size_t>(target_p);

        if (pop_from_global_band_locked(target_idx, deadline_bursts[target_idx], out)) {
            calendar_idx = (calendar_idx + 1) % kPriorityCalendarLength;
            return true;
        }

        for (Priority p : kFallbackPriorityOrder) {
            if (p == target_p) continue;
            const std::size_t p_idx = static_cast<std::size_t>(p);
            if (pop_from_global_band_locked(p_idx, deadline_bursts[p_idx], out)) {
                calendar_idx = (calendar_idx + 1) % kPriorityCalendarLength;
                return true;
            }
        }
        return false;
    }

    bool global_queues_empty() const noexcept {
        for (std::size_t i = 0; i < 4; ++i) {
            if (!global_edf_heaps[i].empty() || !global_injection_queues[i].empty()) {
                return false;
            }
        }
        return true;
    }

    struct LockedLocalDeque {
        mutable std::mutex mutex;
        std::array<std::deque<QueuedTask>, 4> bands;
        Impl* impl_ptr{nullptr};

        void push_back(QueuedTask task, Priority priority) {
            std::lock_guard<std::mutex> lock(mutex);
            bands[static_cast<std::size_t>(priority)].push_back(std::move(task));
        }

        bool pop_back_weighted(std::size_t& calendar_idx, QueuedTask& out) {
            std::lock_guard<std::mutex> lock(mutex);
            const Priority target_p = kPriorityCalendar[calendar_idx % kPriorityCalendarLength];
            auto& target_q = bands[static_cast<std::size_t>(target_p)];
            if (!target_q.empty()) {
                out = std::move(target_q.back());
                target_q.pop_back();
                calendar_idx = (calendar_idx + 1) % kPriorityCalendarLength;
                if (impl_ptr && impl_ptr->metrics.level != MetricsLevel::Off) {
                    MetricsTracker::saturating_inc(impl_ptr->metrics.shard_for_current().local_claims);
                }
                return true;
            }

            for (Priority p : kFallbackPriorityOrder) {
                if (p == target_p) continue;
                auto& q = bands[static_cast<std::size_t>(p)];
                if (!q.empty()) {
                    out = std::move(q.back());
                    q.pop_back();
                    calendar_idx = (calendar_idx + 1) % kPriorityCalendarLength;
                    if (impl_ptr && impl_ptr->metrics.level != MetricsLevel::Off) {
                        MetricsTracker::saturating_inc(impl_ptr->metrics.shard_for_current().local_claims);
                    }
                    return true;
                }
            }
            return false;
        }

        bool steal_front_weighted(std::size_t& calendar_idx, QueuedTask& out) {
            std::lock_guard<std::mutex> lock(mutex);
            const Priority target_p = kPriorityCalendar[calendar_idx % kPriorityCalendarLength];
            auto& target_q = bands[static_cast<std::size_t>(target_p)];
            if (!target_q.empty()) {
                out = std::move(target_q.front());
                target_q.pop_front();
                calendar_idx = (calendar_idx + 1) % kPriorityCalendarLength;
                return true;
            }

            for (Priority p : kFallbackPriorityOrder) {
                if (p == target_p) continue;
                auto& q = bands[static_cast<std::size_t>(p)];
                if (!q.empty()) {
                    out = std::move(q.front());
                    q.pop_front();
                    calendar_idx = (calendar_idx + 1) % kPriorityCalendarLength;
                    return true;
                }
            }
            return false;
        }

        bool empty() const {
            std::lock_guard<std::mutex> lock(mutex);
            return bands[0].empty() && bands[1].empty() && bands[2].empty() && bands[3].empty();
        }

        std::size_t size() const {
            std::lock_guard<std::mutex> lock(mutex);
            return bands[0].size() + bands[1].size() + bands[2].size() + bands[3].size();
        }
    };
    std::vector<std::unique_ptr<LockedLocalDeque>> local_deques;

    Impl(RuntimeId id, SchedulerOptions opts, SchedulerCapabilities caps)
        : runtime_id(id),
          options(std::move(opts)),
          capabilities(caps),
          packed_status(pack(SchedulerState::Running, ShutdownMode::None)) {
        
        metrics.init(options.metrics_level, options.worker_count);
        register_runtime_impl(this);

        local_deques.reserve(options.worker_count);
        for (std::size_t i = 0; i < options.worker_count; ++i) {
            auto deque = std::make_unique<LockedLocalDeque>();
            deque->impl_ptr = this;
            local_deques.push_back(std::move(deque));
        }
        
        // 1. Reaper 注册与能力预留（R-023, R-024, R-097）
        auto& registry = detail::ReaperRegistry::instance();
        if (!registry.is_registration_open()) {
            unregister_runtime_impl(this);
            throw scheduler_creation_rejected(SchedulerCreationError::FinalizationStarted);
        }
        if (!registry.register_runtime(
                runtime_id,
                [this] {
                    this->request_shutdown_mode(ShutdownMode::Graceful);
                    {
                        std::lock_guard<std::mutex> lock(this->lifecycle_mutex);
                        this->stop_requested = true;
                    }
                    this->work_cv.notify_all();
                },
                [this] {
                    this->request_shutdown_mode(ShutdownMode::Immediate);
                    {
                        std::lock_guard<std::mutex> lock(this->lifecycle_mutex);
                        this->stop_requested = true;
                        this->cancel_all_unstarted_tasks_locked();
                    }
                    this->slot_cv.notify_all();
                    this->work_cv.notify_all();
                },
                [this] {
                    this->reaper_cleanup_and_join();
                })) {
            unregister_runtime_impl(this);
            if (registry.should_fail_reservation()) {
                throw std::bad_alloc();
            }
            throw scheduler_creation_rejected(SchedulerCreationError::FinalizationStarted);
        }

        // 2. 创建 Worker 并通过启动栅栏进行同步强事务管理（R-097, D-155）
        const std::size_t count = options.worker_count;
        active_workers.store(count, std::memory_order_relaxed);
        worker_threads.reserve(count);

        try {
            for (std::size_t i = 0; i < count; ++i) {
                // 检查故障注入（模拟第 k 个 worker 线程创建失败）
                if (registry.worker_creation_failure_index() == i + 1) {
                    throw std::system_error(
                        std::make_error_code(std::errc::resource_unavailable_try_again),
                        "Injected worker thread creation failure");
                }
                worker_threads.emplace_back(&Impl::worker_thread_entry, this, i);
            }

            // 等待全部 Worker 就绪到达 startup 栅栏
            {
                std::unique_lock<std::mutex> lock(lifecycle_mutex);
                startup_cv.wait(lock, [this, count] {
                    return workers_ready == count;
                });

                // 3. 在发布 Running 前再次检查 Finalization 状态（D-156 竞态全序）
                if (!registry.is_registration_open()) {
                    // Finalization close 赢得竞态：回滚已创建 Worker 并拒绝创建
                    startup_failed = true;
                    stop_requested = true;
                    startup_cv.notify_all();
                    work_cv.notify_all();
                    throw scheduler_creation_rejected(SchedulerCreationError::FinalizationStarted);
                }

                // 4. 一次性发布 Running（R-097）并释放 Worker 启动栅栏
                packed_status.store(pack(SchedulerState::Running, ShutdownMode::None), std::memory_order_release);
                startup_done = true;
                startup_cv.notify_all();
            }
        } catch (...) {
            // 回滚事务：停止并 join 全部已创建的 Worker，撤销 Reaper 注册，保证 0 活跃 Worker
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex);
                startup_failed = true;
                stop_requested = true;
            }
            startup_cv.notify_all();
            work_cv.notify_all();

            reaper_cleanup_and_join();
            registry.unregister_runtime(runtime_id);
            unregister_runtime_impl(this);
            throw;
        }
    }

    ~Impl() {
        unregister_runtime_impl(this);
        // 非 Worker 正常析构（若尚未经过 Worker handoff 移交且未处于 Stopped 状态）
        if (!handoff_dispatched) {
            if (get_status().state != SchedulerState::Stopped) {
                shutdown_sync(ShutdownMode::Graceful);
            }
        }
    }

    std::mutex shutdown_mutex;
    std::condition_variable shutdown_done_cv;
    bool shutdown_in_progress{false};

    // 同步关闭并在全部 Worker 退出并 join 后发布 Stopped（R-010 / R-012 / R-016 / R-019）
    void shutdown_sync(ShutdownMode mode) {
        if (get_status().state == SchedulerState::Stopped) {
            return;
        }

        request_shutdown_mode(mode);
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex);
            stop_requested = true;
        }
        work_cv.notify_all();

        // 共享 Shutdown Completion（R-016 / D-013）：Leader-Waiter 模式
        std::unique_lock<std::mutex> s_lock(shutdown_mutex);
        if (get_status().state == SchedulerState::Stopped) {
            return;
        }

        if (!shutdown_in_progress) {
            shutdown_in_progress = true;
            s_lock.unlock();

            const auto t_join_start = std::chrono::steady_clock::now();
            // Leader 负责 join 全部 worker 线程（恰好 join 一次）
            for (auto& t : worker_threads) {
                if (t.joinable()) {
                    t.join();
                }
            }
            worker_threads.clear();
            const auto t_join_end = std::chrono::steady_clock::now();
            if (metrics.level == MetricsLevel::Detailed && t_join_end >= t_join_start) {
                metrics.shard_for_current().runtime_join_latency.record(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t_join_end - t_join_start).count());
            }

            const auto current_mode = get_status().shutdown_mode;
            packed_status.store(pack(SchedulerState::Stopped, current_mode), std::memory_order_release);
            slot_cv.notify_all();
            detail::ReaperRegistry::instance().unregister_runtime(runtime_id);

            s_lock.lock();
            shutdown_in_progress = false;
            shutdown_done_cv.notify_all();
        } else {
            // Waiter 等待 Leader 完成 join 并发布 Stopped
            shutdown_done_cv.wait(s_lock, [this] {
                return get_status().state == SchedulerState::Stopped;
            });
        }
    }

    void cancel_all_unstarted_tasks_locked() noexcept {
        for (auto& heap : global_edf_heaps) {
            std::vector<EdfEntry> remaining_edf;
            for (auto& entry : heap) {
                if (entry.task.invoker && entry.task.invoker->is_resume_segment()) {
                    remaining_edf.push_back(std::move(entry));
                } else {
                    if (entry.task.invoker) {
                        entry.task.invoker->cancel_pre_start();
                    }
                    if (entry.task.is_external && external_pending_count > 0) {
                        --external_pending_count;
                        slot_cv.notify_one();
                    }
                }
            }
            heap = std::move(remaining_edf);
            std::make_heap(heap.begin(), heap.end(), std::greater<EdfEntry>{});
        }

        for (auto& q : global_injection_queues) {
            std::deque<QueuedTask> remaining_global;
            while (!q.empty()) {
                auto task = std::move(q.front());
                q.pop_front();
                if (task.invoker && task.invoker->is_resume_segment()) {
                    // R-075 / D-154: 保留已启动的 resume segment，允许其恢复执行合作取消或自然完成
                    remaining_global.push_back(std::move(task));
                } else {
                    if (task.invoker) {
                        task.invoker->cancel_pre_start();
                    }
                    if (task.is_external && external_pending_count > 0) {
                        --external_pending_count;
                        slot_cv.notify_one();
                    }
                }
            }
            q = std::move(remaining_global);
        }

        for (auto& d : local_deques) {
            if (d) {
                std::lock_guard<std::mutex> lk(d->mutex);
                for (auto& q : d->bands) {
                    std::deque<QueuedTask> remaining_local;
                    while (!q.empty()) {
                        auto task = std::move(q.front());
                        q.pop_front();
                        if (task.invoker && task.invoker->is_resume_segment()) {
                            remaining_local.push_back(std::move(task));
                        } else {
                            if (task.invoker) {
                                task.invoker->cancel_pre_start();
                            }
                        }
                    }
                    q = std::move(remaining_local);
                }
            }
        }
    }

    // 状态转换与模式保持（R-014 / R-015 / R-022）
    void request_shutdown_mode(ShutdownMode requested_mode) noexcept {
        work_epoch.fetch_add(1, std::memory_order_release);
        uint16_t current = packed_status.load(std::memory_order_acquire);
        while (true) {
            auto st = unpack(current);
            if (st.state == SchedulerState::Running) {
                uint16_t next = pack(SchedulerState::Stopping, requested_mode);
                if (packed_status.compare_exchange_weak(current, next, std::memory_order_acq_rel)) {
                    if (requested_mode == ShutdownMode::Immediate) {
                        {
                            std::lock_guard<std::mutex> lock(lifecycle_mutex);
                            cancel_all_unstarted_tasks_locked();
                        }
                        cancel_all_timers();
                    }
                    slot_cv.notify_all();
                    work_cv.notify_all();
                    break;
                }
            } else if (st.state == SchedulerState::Stopping) {
                // Immediate 升级（D-012 / D-014 / R-014）
                if (requested_mode == ShutdownMode::Immediate && st.shutdown_mode != ShutdownMode::Immediate) {
                    uint16_t next = pack(SchedulerState::Stopping, ShutdownMode::Immediate);
                    if (packed_status.compare_exchange_weak(current, next, std::memory_order_acq_rel)) {
                        {
                            std::lock_guard<std::mutex> lock(lifecycle_mutex);
                            cancel_all_unstarted_tasks_locked();
                        }
                        cancel_all_timers();
                        slot_cv.notify_all();
                        work_cv.notify_all();
                        break;
                    }
                } else {
                    break;
                }
            } else {
                // 已处于 Stopped，保持现有模式（R-019 / R-022）
                break;
            }
        }
    }

    // R-021, R-022: Worker 释放最后 Handle 时触发异步 orphan handoff
    void execute_worker_orphan_handoff(std::shared_ptr<Impl> self) noexcept {
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex);
            handoff_dispatched = true;
            stop_requested = true;
        }
        // R-022: 请求 Graceful Shutdown，保留当前模式
        request_shutdown_mode(ShutdownMode::Graceful);
        work_cv.notify_all();

        // 移交强引用所有权给 Reaper
        detail::ReaperRegistry::instance().execute_worker_handoff(
            runtime_id,
            std::static_pointer_cast<void>(self),
            [self]() {
                self->reaper_cleanup_and_join();
            }
        );
    }

    // 由 Reaper 线程（非目标 Worker 线程）执行最终 join 与清理
    void reaper_cleanup_and_join() noexcept {
        std::unique_lock<std::mutex> s_lock(shutdown_mutex);
        if (get_status().state == SchedulerState::Stopped) {
            return;
        }

        if (!shutdown_in_progress) {
            shutdown_in_progress = true;
            s_lock.unlock();

            const auto t_join_start = std::chrono::steady_clock::now();
            for (auto& t : worker_threads) {
                if (t.joinable()) {
                    t.join();
                }
            }
            worker_threads.clear();
            const auto t_join_end = std::chrono::steady_clock::now();
            if (metrics.level == MetricsLevel::Detailed && t_join_end >= t_join_start) {
                metrics.shard_for_current().runtime_join_latency.record(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t_join_end - t_join_start).count());
            }

            const auto current_mode = get_status().shutdown_mode;
            packed_status.store(pack(SchedulerState::Stopped, current_mode), std::memory_order_release);
            slot_cv.notify_all();

            s_lock.lock();
            shutdown_in_progress = false;
            shutdown_done_cv.notify_all();
        } else {
            shutdown_done_cv.wait(s_lock, [this] {
                return get_status().state == SchedulerState::Stopped;
            });
        }
    }

    std::atomic<std::uint64_t> next_graph_run_sequence{0};

    GraphRunId allocate_graph_run_id() noexcept {
        return GraphRunId{runtime_id, ++next_graph_run_sequence};
    }

    detail::AdmissionDecision acquire_admission_slot(bool block, bool is_internal) {
        return acquire_admission_slots(1, block, is_internal);
    }

    detail::AdmissionDecision acquire_admission_slots(std::size_t count, bool block, bool is_internal) {
        if (count == 0) {
            const auto st = unpack(packed_status.load(std::memory_order_acquire));
            if (st.state == SchedulerState::Stopped) {
                if (metrics.level != MetricsLevel::Off) {
                    MetricsTracker::saturating_inc(metrics.shard_for_current().rejected_lifecycle);
                }
                return detail::AdmissionDecision::Stopped;
            }
            if (st.state == SchedulerState::Stopping &&
                (!is_internal || st.shutdown_mode != ShutdownMode::Graceful)) {
                if (metrics.level != MetricsLevel::Off) {
                    MetricsTracker::saturating_inc(metrics.shard_for_current().rejected_lifecycle);
                }
                return detail::AdmissionDecision::Stopping;
            }
            if (metrics.level != MetricsLevel::Off) {
                MetricsTracker::saturating_add(metrics.shard_for_current().accepted_task_identities, count);
                metrics.ready_tasks.fetch_add(count, std::memory_order_relaxed);
            }
            return detail::AdmissionDecision::Success;
        }

        std::unique_lock<std::mutex> lock(lifecycle_mutex);
        while (true) {
            const auto st = unpack(packed_status.load(std::memory_order_acquire));
            if (st.state == SchedulerState::Stopped) {
                if (metrics.level != MetricsLevel::Off) {
                    MetricsTracker::saturating_inc(metrics.shard_for_current().rejected_lifecycle);
                }
                return detail::AdmissionDecision::Stopped;
            }
            if (st.state == SchedulerState::Stopping) {
                // R-006 / D-002: Graceful Stopping 接受授权的 Internal Submission
                if (is_internal && st.shutdown_mode == ShutdownMode::Graceful) {
                    if (metrics.level != MetricsLevel::Off) {
                        MetricsTracker::saturating_add(metrics.shard_for_current().accepted_task_identities, count);
                        metrics.ready_tasks.fetch_add(count, std::memory_order_relaxed);
                    }
                    return detail::AdmissionDecision::Success;
                }
                if (metrics.level != MetricsLevel::Off) {
                    MetricsTracker::saturating_inc(metrics.shard_for_current().rejected_lifecycle);
                }
                return detail::AdmissionDecision::Stopping;
            }

            if (is_internal) {
                if (metrics.level != MetricsLevel::Off) {
                    MetricsTracker::saturating_add(metrics.shard_for_current().accepted_task_identities, count);
                    metrics.ready_tasks.fetch_add(count, std::memory_order_relaxed);
                }
                return detail::AdmissionDecision::Success;
            }

            // R-070 / D-106: 若 count > external_pending_capacity，即使 policy 为 Block 也立即以 CapacityExhausted 拒绝
            if (count > options.external_pending_capacity) {
                if (metrics.level != MetricsLevel::Off) {
                    MetricsTracker::saturating_inc(metrics.shard_for_current().rejected_capacity);
                }
                return detail::AdmissionDecision::CapacityExhausted;
            }

            if (external_pending_count + count <= options.external_pending_capacity) {
                external_pending_count += count;
                if (metrics.level != MetricsLevel::Off) {
                    MetricsTracker::saturating_add(metrics.shard_for_current().accepted_task_identities, count);
                    metrics.ready_tasks.fetch_add(count, std::memory_order_relaxed);
                }
                return detail::AdmissionDecision::Success;
            }

            if (!block || options.external_backpressure != ExternalBackpressure::Block) {
                if (metrics.level != MetricsLevel::Off) {
                    MetricsTracker::saturating_inc(metrics.shard_for_current().rejected_capacity);
                }
                return detail::AdmissionDecision::CapacityExhausted;
            }

            if (metrics.level != MetricsLevel::Off) {
                MetricsTracker::saturating_inc(metrics.shard_for_current().blocking_submit_waits);
            }
            const auto t_wait_start = std::chrono::steady_clock::now();
            // Ordinary thread waiting on slot_cv (R-061 / D-086 / D-106)
            slot_cv.wait(lock, [this, count] {
                const auto current_st = unpack(packed_status.load(std::memory_order_acquire));
                return current_st.state != SchedulerState::Running ||
                       (external_pending_count + count <= options.external_pending_capacity);
            });
            const auto t_wait_end = std::chrono::steady_clock::now();
            if (metrics.level != MetricsLevel::Off) {
                MetricsTracker::saturating_inc(metrics.shard_for_current().blocking_submit_wakeups);
                if (metrics.level == MetricsLevel::Detailed && t_wait_end >= t_wait_start) {
                    metrics.shard_for_current().blocking_admission_wait.record(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(t_wait_end - t_wait_start).count());
                }
            }
        }
    }

    void release_external_slot() {
        release_external_slots(1);
    }

    void release_external_slots(std::size_t count) {
        if (count == 0) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex);
            if (external_pending_count >= count) {
                external_pending_count -= count;
            } else {
                external_pending_count = 0;
            }
        }
        slot_cv.notify_all();
    }

    struct TimerEntry {
        std::uint64_t timer_id{0};
        std::chrono::steady_clock::time_point wake_time{};
        std::uint64_t sequence{0};
        std::shared_ptr<AwaitHandshake> handshake{nullptr};
        std::function<void()> resume_action{nullptr};
        std::size_t heap_index{0};
    };

    std::atomic<std::uint64_t> next_timer_id{1};
    std::atomic<std::uint64_t> next_timer_sequence{1};
    std::mutex timer_mutex;
    std::vector<std::shared_ptr<TimerEntry>> timer_heap;
    std::unordered_map<std::uint64_t, std::shared_ptr<TimerEntry>> timer_map;

    static bool compare_timer_entry(const TimerEntry& a, const TimerEntry& b) noexcept {
        if (a.wake_time != b.wake_time) {
            return a.wake_time < b.wake_time;
        }
        return a.sequence < b.sequence;
    }

    void timer_heap_bubble_up(std::size_t idx) {
        while (idx > 0) {
            std::size_t parent = (idx - 1) / 2;
            if (compare_timer_entry(*timer_heap[idx], *timer_heap[parent])) {
                std::swap(timer_heap[idx], timer_heap[parent]);
                timer_heap[idx]->heap_index = idx;
                timer_heap[parent]->heap_index = parent;
                idx = parent;
            } else {
                break;
            }
        }
    }

    void timer_heap_bubble_down(std::size_t idx) {
        const std::size_t n = timer_heap.size();
        while (true) {
            std::size_t smallest = idx;
            std::size_t left = 2 * idx + 1;
            std::size_t right = 2 * idx + 2;
            if (left < n && compare_timer_entry(*timer_heap[left], *timer_heap[smallest])) {
                smallest = left;
            }
            if (right < n && compare_timer_entry(*timer_heap[right], *timer_heap[smallest])) {
                smallest = right;
            }
            if (smallest != idx) {
                std::swap(timer_heap[idx], timer_heap[smallest]);
                timer_heap[idx]->heap_index = idx;
                timer_heap[smallest]->heap_index = smallest;
                idx = smallest;
            } else {
                break;
            }
        }
    }

    std::uint64_t register_timer(std::chrono::steady_clock::time_point wake_time,
                                 std::shared_ptr<AwaitHandshake> handshake,
                                 std::function<void()> resume_action) {
        const std::uint64_t tid = next_timer_id.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t seq = next_timer_sequence.fetch_add(1, std::memory_order_relaxed);
        auto entry = std::make_shared<TimerEntry>();
        entry->timer_id = tid;
        entry->wake_time = wake_time;
        entry->sequence = seq;
        entry->handshake = std::move(handshake);
        entry->resume_action = std::move(resume_action);

        if (metrics.level != MetricsLevel::Off) {
            MetricsTracker::saturating_inc(metrics.shard_for_current().timer_registrations);
        }

        bool became_earliest = false;
        {
            std::lock_guard<std::mutex> lock(timer_mutex);
            entry->heap_index = timer_heap.size();
            timer_heap.push_back(entry);
            timer_map[tid] = entry;
            timer_heap_bubble_up(entry->heap_index);
            if (timer_heap.front()->timer_id == tid) {
                became_earliest = true;
            }
        }
        if (became_earliest) {
            work_epoch.fetch_add(1, std::memory_order_release);
            work_cv.notify_one();
        }
        return tid;
    }

    void cancel_timer(std::uint64_t tid) {
        std::lock_guard<std::mutex> lock(timer_mutex);
        auto it = timer_map.find(tid);
        if (it == timer_map.end()) {
            return;
        }
        auto entry = std::move(it->second);
        timer_map.erase(it);
        if (metrics.level != MetricsLevel::Off) {
            MetricsTracker::saturating_inc(metrics.shard_for_current().timer_cancellations);
        }
        const std::size_t idx = entry->heap_index;
        const std::size_t last_idx = timer_heap.size() - 1;
        if (idx == last_idx) {
            timer_heap.pop_back();
        } else {
            timer_heap[idx] = std::move(timer_heap.back());
            timer_heap.pop_back();
            timer_heap[idx]->heap_index = idx;
            timer_heap_bubble_down(idx);
            timer_heap_bubble_up(idx);
        }
    }

    void process_due_timers() {
        const auto now = std::chrono::steady_clock::now();
        struct DueItem {
            std::shared_ptr<AwaitHandshake> handshake;
            std::function<void()> resume_action;
            std::chrono::steady_clock::time_point wake_time;
        };
        std::vector<DueItem> due_items;
        {
            std::lock_guard<std::mutex> lock(timer_mutex);
            while (!timer_heap.empty() && timer_heap.front()->wake_time <= now) {
                auto entry = std::move(timer_heap.front());
                timer_map.erase(entry->timer_id);
                if (timer_heap.size() == 1) {
                    timer_heap.pop_back();
                } else {
                    timer_heap[0] = std::move(timer_heap.back());
                    timer_heap.pop_back();
                    timer_heap[0]->heap_index = 0;
                    timer_heap_bubble_down(0);
                }
                due_items.push_back({std::move(entry->handshake), std::move(entry->resume_action), entry->wake_time});
            }
        }
        if (metrics.level != MetricsLevel::Off && !due_items.empty()) {
            MetricsTracker::saturating_add(metrics.shard_for_current().timer_fires, due_items.size());
            if (metrics.level == MetricsLevel::Detailed) {
                for (const auto& item : due_items) {
                    if (now >= item.wake_time) {
                        const auto lateness = std::chrono::duration_cast<std::chrono::nanoseconds>(now - item.wake_time).count();
                        metrics.shard_for_current().timer_wake_lateness.record(lateness);
                    }
                }
            }
        }
        for (auto& item : due_items) {
            if (item.handshake && item.resume_action) {
                item.handshake->trigger(item.resume_action);
            }
        }
    }

    void cancel_all_timers() {
        std::vector<std::pair<std::shared_ptr<AwaitHandshake>, std::function<void()>>> all_items;
        {
            std::lock_guard<std::mutex> lock(timer_mutex);
            while (!timer_heap.empty()) {
                auto entry = std::move(timer_heap.front());
                timer_map.erase(entry->timer_id);
                if (timer_heap.size() == 1) {
                    timer_heap.pop_back();
                } else {
                    timer_heap[0] = std::move(timer_heap.back());
                    timer_heap.pop_back();
                    timer_heap[0]->heap_index = 0;
                    timer_heap_bubble_down(0);
                }
                all_items.push_back({std::move(entry->handshake), std::move(entry->resume_action)});
            }
        }
        if (metrics.level != MetricsLevel::Off && !all_items.empty()) {
            MetricsTracker::saturating_add(metrics.shard_for_current().timer_cancellations, all_items.size());
        }
        for (auto& [hs, act] : all_items) {
            if (hs && act) {
                hs->trigger_cancel(act);
            }
        }
    }

    std::optional<std::chrono::steady_clock::time_point> earliest_wake_time() {
        std::lock_guard<std::mutex> lock(timer_mutex);
        if (timer_heap.empty()) {
            return std::nullopt;
        }
        return timer_heap.front()->wake_time;
    }

    bool has_timers() {
        std::lock_guard<std::mutex> lock(timer_mutex);
        return !timer_heap.empty();
    }

    void worker_main(std::size_t worker_index);

    void post_task_internal(std::unique_ptr<detail::TaskInvokerBase> task, bool is_external) {
        if (!task) return;
        const Priority p = task->priority();
        const auto dl = task->deadline();
        const bool is_resume = task->is_resume_segment();
        const std::size_t band_idx = static_cast<std::size_t>(p);

        // R-083 / D-133: 带 Deadline 且从未 Running 的任务进入对应 Priority band 的 Global EDF min-heap
        if (dl.has_value() && !is_resume) {
            if (metrics.level != MetricsLevel::Off) {
                MetricsTracker::saturating_inc(metrics.shard_for_current().deadline_admitted);
            }
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex);
                const std::uint64_t seq = ++global_admission_seq;
                global_edf_heaps[band_idx].push_back(EdfEntry{*dl, seq, {std::move(task), is_external}});
                std::push_heap(global_edf_heaps[band_idx].begin(),
                               global_edf_heaps[band_idx].end(),
                               std::greater<EdfEntry>{});
            }
            work_epoch.fetch_add(1, std::memory_order_release);
            work_cv.notify_one();
            return;
        }

        if (!is_external && detail::t_current_worker_impl == this &&
            detail::t_current_worker_runtime_id == runtime_id &&
            detail::t_current_worker_index < local_deques.size()) {
            local_deques[detail::t_current_worker_index]->push_back({std::move(task), false}, p);
        } else {
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex);
                global_injection_queues[band_idx].push_back({std::move(task), is_external});
            }
        }
        work_epoch.fetch_add(1, std::memory_order_release);
        work_cv.notify_one();
    }

    static constexpr std::uint16_t pack(SchedulerState state, ShutdownMode mode) noexcept {
        return static_cast<std::uint16_t>((static_cast<std::uint8_t>(state) << 8) |
                                          static_cast<std::uint8_t>(mode));
    }

    static constexpr SchedulerStatus unpack(std::uint16_t val) noexcept {
        const auto state = static_cast<SchedulerState>((val >> 8) & 0xFF);
        const auto mode = static_cast<ShutdownMode>(val & 0xFF);
        return SchedulerStatus{state, mode};
    }

    SchedulerStatus get_status() const noexcept {
        const std::uint16_t val = packed_status.load(std::memory_order_acquire);
        return unpack(val);
    }

    static void worker_thread_entry(void* arg, std::size_t index) noexcept {
        if (arg != nullptr) {
            static_cast<Impl*>(arg)->worker_main(index);
        }
    }
};

namespace {
std::mutex g_runtime_registry_mutex;
std::unordered_map<std::uint64_t, Scheduler::Impl*> g_runtime_registry;

void register_runtime_impl(Scheduler::Impl* impl) {
    std::lock_guard<std::mutex> lock(g_runtime_registry_mutex);
    g_runtime_registry[impl->runtime_id.value()] = impl;
}

void unregister_runtime_impl(Scheduler::Impl* impl) {
    std::lock_guard<std::mutex> lock(g_runtime_registry_mutex);
    g_runtime_registry.erase(impl->runtime_id.value());
}

Scheduler::Impl* find_runtime_impl(RuntimeId id) {
    std::lock_guard<std::mutex> lock(g_runtime_registry_mutex);
    auto it = g_runtime_registry.find(id.value());
    if (it != g_runtime_registry.end()) {
        return it->second;
    }
    return nullptr;
}
}  // namespace

namespace detail {

thread_local RuntimeId t_current_worker_runtime_id{0};
thread_local void* t_current_worker_impl{nullptr};
thread_local std::size_t t_current_worker_index{0};
thread_local TaskId t_current_executing_task_id{};
thread_local Priority t_current_executing_task_priority{Priority::Normal};
thread_local GraphRunId t_current_executing_graph_run_id{};
thread_local std::size_t t_current_helping_depth{0};

void record_metrics_submission_attempt(RuntimeId id) noexcept {
    auto* impl = find_runtime_impl(id);
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    Scheduler::Impl::MetricsTracker::saturating_inc(impl->metrics.shard_for_current().submission_attempts);
}

void record_metrics_first_start(TaskId id, std::optional<DeadlineDisposition> dl_disp) noexcept {
    auto* impl = find_runtime_impl(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    Scheduler::Impl::MetricsTracker::saturating_inc(shard.first_starts);
    if (impl->metrics.ready_tasks.load(std::memory_order_relaxed) > 0) {
        impl->metrics.ready_tasks.fetch_sub(1, std::memory_order_relaxed);
    }
    impl->metrics.running_tasks.fetch_add(1, std::memory_order_relaxed);
    if (dl_disp.has_value()) {
        if (*dl_disp == DeadlineDisposition::Met) {
            Scheduler::Impl::MetricsTracker::saturating_inc(shard.deadline_met);
        } else if (*dl_disp == DeadlineDisposition::Missed) {
            Scheduler::Impl::MetricsTracker::saturating_inc(shard.deadline_missed);
        }
    }
}

void record_metrics_succeeded(TaskId id) noexcept {
    auto* impl = find_runtime_impl(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    Scheduler::Impl::MetricsTracker::saturating_inc(shard.succeeded);
    if (impl->metrics.running_tasks.load(std::memory_order_relaxed) > 0) {
        impl->metrics.running_tasks.fetch_sub(1, std::memory_order_relaxed);
    }
}

void record_metrics_failed(TaskId id) noexcept {
    auto* impl = find_runtime_impl(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    Scheduler::Impl::MetricsTracker::saturating_inc(shard.failed);
    if (impl->metrics.running_tasks.load(std::memory_order_relaxed) > 0) {
        impl->metrics.running_tasks.fetch_sub(1, std::memory_order_relaxed);
    }
}

void record_metrics_cancelled_cooperative(TaskId id) noexcept {
    auto* impl = find_runtime_impl(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    Scheduler::Impl::MetricsTracker::saturating_inc(shard.cancelled_cooperative);
    if (impl->metrics.running_tasks.load(std::memory_order_relaxed) > 0) {
        impl->metrics.running_tasks.fetch_sub(1, std::memory_order_relaxed);
    }
}

void record_metrics_cancelled_before_start(TaskId id, bool has_deadline) noexcept {
    auto* impl = find_runtime_impl(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    Scheduler::Impl::MetricsTracker::saturating_inc(shard.cancelled_before_start);
    if (impl->metrics.ready_tasks.load(std::memory_order_relaxed) > 0) {
        impl->metrics.ready_tasks.fetch_sub(1, std::memory_order_relaxed);
    }
    if (has_deadline) {
        Scheduler::Impl::MetricsTracker::saturating_inc(shard.deadline_cancelled_before_start);
    }
}

void record_metrics_unobserved_failure(TaskId id) noexcept {
    auto* impl = find_runtime_impl(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    Scheduler::Impl::MetricsTracker::saturating_inc(shard.unobserved_failures);
}

void record_metrics_suspended(TaskId id) noexcept {
    auto* impl = find_runtime_impl(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    Scheduler::Impl::MetricsTracker::saturating_inc(shard.coroutine_suspends);
    if (impl->metrics.running_tasks.load(std::memory_order_relaxed) > 0) {
        impl->metrics.running_tasks.fetch_sub(1, std::memory_order_relaxed);
    }
    impl->metrics.suspended_tasks.fetch_add(1, std::memory_order_relaxed);
}

void record_metrics_resumed(TaskId id) noexcept {
    auto* impl = find_runtime_impl(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    if (impl->metrics.suspended_tasks.load(std::memory_order_relaxed) > 0) {
        impl->metrics.suspended_tasks.fetch_sub(1, std::memory_order_relaxed);
    }
    impl->metrics.ready_tasks.fetch_add(1, std::memory_order_relaxed);
}

void record_metrics_resume_segment(TaskId id) noexcept {
    auto* impl = find_runtime_impl(id.runtime_id());
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    auto& shard = impl->metrics.shard_for_current();
    Scheduler::Impl::MetricsTracker::saturating_inc(shard.resume_segments);
    if (impl->metrics.ready_tasks.load(std::memory_order_relaxed) > 0) {
        impl->metrics.ready_tasks.fetch_sub(1, std::memory_order_relaxed);
    }
    impl->metrics.running_tasks.fetch_add(1, std::memory_order_relaxed);
}

void record_metrics_explicit_yield() noexcept {
    if (t_current_worker_impl != nullptr) {
        auto* impl = static_cast<Scheduler::Impl*>(t_current_worker_impl);
        if (impl->metrics.level != MetricsLevel::Off) {
            Scheduler::Impl::MetricsTracker::saturating_inc(impl->metrics.shard_for_current().explicit_yields);
        }
    }
}

void record_metrics_graph_node_terminal(RuntimeId id) noexcept {
    auto* impl = find_runtime_impl(id);
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    Scheduler::Impl::MetricsTracker::saturating_inc(impl->metrics.shard_for_current().graph_nodes_terminal);
}

void record_metrics_graph_run_completed(RuntimeId id) noexcept {
    auto* impl = find_runtime_impl(id);
    if (!impl || impl->metrics.level == MetricsLevel::Off) return;
    if (impl->metrics.active_graph_runs.load(std::memory_order_relaxed) > 0) {
        impl->metrics.active_graph_runs.fetch_sub(1, std::memory_order_relaxed);
    }
}

void record_metrics_ready_queue_wait(TaskId id, std::uint64_t duration_ns) noexcept {
    auto* impl = find_runtime_impl(id.runtime_id());
    if (!impl || impl->metrics.level != MetricsLevel::Detailed) return;
    impl->metrics.shard_for_current().ready_queue_wait.record(duration_ns);
}

void record_metrics_execution_segment(TaskId id, std::uint64_t duration_ns) noexcept {
    auto* impl = find_runtime_impl(id.runtime_id());
    if (!impl || impl->metrics.level != MetricsLevel::Detailed) return;
    impl->metrics.shard_for_current().execution_segment.record(duration_ns);
}

void record_metrics_task_wall_time(TaskId id, std::uint64_t duration_ns) noexcept {
    auto* impl = find_runtime_impl(id.runtime_id());
    if (!impl || impl->metrics.level != MetricsLevel::Detailed) return;
    impl->metrics.shard_for_current().task_wall_time.record(duration_ns);
}

void record_metrics_blocking_admission_wait(RuntimeId id, std::uint64_t duration_ns) noexcept {
    auto* impl = find_runtime_impl(id);
    if (!impl || impl->metrics.level != MetricsLevel::Detailed) return;
    impl->metrics.shard_for_current().blocking_admission_wait.record(duration_ns);
}

void record_metrics_timer_wake_lateness(RuntimeId id, std::uint64_t duration_ns) noexcept {
    auto* impl = find_runtime_impl(id);
    if (!impl || impl->metrics.level != MetricsLevel::Detailed) return;
    impl->metrics.shard_for_current().timer_wake_lateness.record(duration_ns);
}

void record_metrics_deadline_start_lateness(TaskId id, std::uint64_t duration_ns) noexcept {
    auto* impl = find_runtime_impl(id.runtime_id());
    if (!impl || impl->metrics.level != MetricsLevel::Detailed) return;
    impl->metrics.shard_for_current().deadline_start_lateness.record(duration_ns);
}

void record_metrics_worker_park_duration(RuntimeId id, std::uint64_t duration_ns) noexcept {
    auto* impl = find_runtime_impl(id);
    if (!impl || impl->metrics.level != MetricsLevel::Detailed) return;
    impl->metrics.shard_for_current().worker_park_duration.record(duration_ns);
}

void record_metrics_runtime_join_latency(RuntimeId id, std::uint64_t duration_ns) noexcept {
    auto* impl = find_runtime_impl(id);
    if (!impl || impl->metrics.level != MetricsLevel::Detailed) return;
    impl->metrics.shard_for_current().runtime_join_latency.record(duration_ns);
}

TaskExecutionContextGuard::TaskExecutionContextGuard(TaskId new_id, Priority new_priority) noexcept
    : prev_id(t_current_executing_task_id), prev_priority(t_current_executing_task_priority) {
    t_current_executing_task_id = new_id;
    t_current_executing_task_priority = new_priority;
}

TaskExecutionContextGuard::~TaskExecutionContextGuard() noexcept {
    t_current_executing_task_id = prev_id;
    t_current_executing_task_priority = prev_priority;
}

TaskId current_executing_task_id() noexcept {
    return t_current_executing_task_id;
}

Priority current_executing_task_priority() noexcept {
    return t_current_executing_task_priority;
}

GraphRunId current_executing_graph_run_id() noexcept {
    return t_current_executing_graph_run_id;
}

struct GraphNodeExecutionContextGuard {
    GraphRunId prev_id;
    Priority prev_priority;
    explicit GraphNodeExecutionContextGuard(GraphRunId new_id, Priority new_priority = Priority::Normal) noexcept
        : prev_id(t_current_executing_graph_run_id), prev_priority(t_current_executing_task_priority) {
        t_current_executing_graph_run_id = new_id;
        t_current_executing_task_priority = new_priority;
    }
    ~GraphNodeExecutionContextGuard() noexcept {
        t_current_executing_graph_run_id = prev_id;
        t_current_executing_task_priority = prev_priority;
    }
};

struct GraphCoroutineResumeWrapper final : TaskInvokerBase {
    std::unique_ptr<TaskInvokerBase> inner;
    std::shared_ptr<GraphRunSharedState> graph_state;
    std::shared_ptr<TaskSharedState<void>> task_state;
    NodeId node_id;
    std::function<void(NodeId)> trigger_fn;

    GraphCoroutineResumeWrapper(
        std::unique_ptr<TaskInvokerBase> in,
        std::shared_ptr<GraphRunSharedState> gs,
        std::shared_ptr<TaskSharedState<void>> ts,
        NodeId nid,
        std::function<void(NodeId)> tfn)
        : inner(std::move(in)), graph_state(std::move(gs)), task_state(std::move(ts)),
          node_id(nid), trigger_fn(std::move(tfn)) {}

    void execute() override {
        const Priority p = task_state ? task_state->priority() : Priority::Normal;
        GraphNodeExecutionContextGuard node_guard(graph_state->id, p);
        if (inner) {
            inner->execute();
        }
        if (task_state && task_state->is_completed()) {
            TaskState outcome = task_state->state();
            std::exception_ptr ex = (outcome == TaskState::Failed) ? task_state->exception() : nullptr;
            graph_state->mark_node_terminal(node_id.value(), outcome, ex);
            if (trigger_fn) {
                trigger_fn(node_id);
            }
        }
    }

    void cancel_pre_start() noexcept override {
        if (inner) {
            inner->cancel_pre_start();
        }
    }

    [[nodiscard]] bool is_resume_segment() const noexcept override {
        return true;
    }

    [[nodiscard]] Priority priority() const noexcept override {
        return task_state ? task_state->priority() : Priority::Normal;
    }
};

inline std::uint64_t next_random(std::uint64_t& state) noexcept {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1DULL;
}

void generate_steal_victims(
    std::size_t self_index,
    std::size_t worker_count,
    std::size_t probe_limit,
    std::uint64_t& rng_state,
    std::vector<std::size_t>& out_victims) {
    out_victims.clear();
    if (worker_count <= 1 || probe_limit == 0) {
        return;
    }

    const std::size_t num_candidates = worker_count - 1;
    const std::size_t k = std::min(probe_limit, num_candidates);
    out_victims.reserve(k);

    std::vector<std::size_t> candidates;
    candidates.reserve(num_candidates);
    for (std::size_t i = 0; i < worker_count; ++i) {
        if (i != self_index) {
            candidates.push_back(i);
        }
    }

    for (std::size_t i = 0; i < k; ++i) {
        std::size_t j = i + static_cast<std::size_t>(next_random(rng_state) % (num_candidates - i));
        std::swap(candidates[i], candidates[j]);
        out_victims.push_back(candidates[i]);
    }
}

void perform_caller_wait(
    const TaskSharedStateBase& target,
    std::optional<std::chrono::steady_clock::time_point> deadline) {
    // 1. Direct Self-Wait 必须在副作用前抛 std::logic_error（R-052 / D-049 / D-065）
    if (t_current_worker_impl != nullptr &&
        t_current_executing_task_id != TaskId{} &&
        t_current_executing_task_id == target.id()) {
        throw std::logic_error("direct self-wait detected on TaskHandle");
    }

    // 2. 已完成即时返回
    if (target.is_completed()) {
        return;
    }

    // 3. 非正/已过期 deadline 即时返回
    if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline) {
        return;
    }

    // 4. 非 Worker 线程：执行无界/有界条件变量等待（D-047）
    if (t_current_worker_impl == nullptr) {
        std::unique_lock<std::mutex> lock(target.mutex());
        if (deadline.has_value()) {
            target.cv().wait_until(lock, *deadline, [&target] {
                return target.is_completed();
            });
        } else {
            target.cv().wait(lock, [&target] {
                return target.is_completed();
            });
        }
        return;
    }

    // 5. 同/源 Runtime Worker 线程：执行 Helping Wait（R-052 / D-048 / D-051）
    auto* impl = static_cast<Scheduler::Impl*>(t_current_worker_impl);

    // R-059 / D-078 / D-079: 检查 Helping depth 超限
    if (t_current_helping_depth >= impl->options.max_helping_depth) {
        throw helping_depth_exceeded{};
    }

    struct DepthGuard {
        std::size_t& depth;
        explicit DepthGuard(std::size_t& d) noexcept : depth(d) { ++depth; }
        ~DepthGuard() noexcept { --depth; }
    } guard(t_current_helping_depth);

    thread_local std::size_t t_help_local_cal = 0;
    thread_local std::size_t t_help_global_cal = 0;
    thread_local std::size_t t_help_steal_cal = 0;
    thread_local std::array<std::size_t, 4> t_help_deadline_bursts{0, 0, 0, 0};

    while (!target.is_completed()) {
        if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline) {
            break;
        }

        Scheduler::Impl::QueuedTask task;
        bool found_task = false;

        if (t_current_worker_index < impl->local_deques.size()) {
            if (impl->local_deques[t_current_worker_index]->pop_back_weighted(t_help_local_cal, task)) {
                found_task = true;
                std::lock_guard<std::mutex> lock(impl->lifecycle_mutex);
                ++impl->active_task_count;
            }
        }

        if (!found_task) {
            std::unique_lock<std::mutex> lock(impl->lifecycle_mutex);
            const auto st = Scheduler::Impl::unpack(impl->packed_status.load(std::memory_order_acquire));
            // R-059 / D-080: Immediate 模式下不得 first-start 新 Task
            if (st.state != SchedulerState::Stopped &&
                st.shutdown_mode != ShutdownMode::Immediate &&
                impl->pop_global_weighted(t_help_global_cal, t_help_deadline_bursts, task)) {
                ++impl->active_task_count;
                found_task = true;
                if (task.is_external && impl->external_pending_count > 0) {
                    --impl->external_pending_count;
                    impl->slot_cv.notify_one();
                }
            }
        }

        // 尝试从其他 Worker 的 Local Deque 执行 bounded non-repeating Steal Round (R-064)
        if (!found_task) {
            std::vector<std::size_t> victims;
            static thread_local std::uint64_t s_help_rng = 0x854329415849ULL;
            generate_steal_victims(t_current_worker_index, impl->options.worker_count, impl->options.steal_probe_limit, s_help_rng, victims);
            for (std::size_t v : victims) {
                if (impl->metrics.level != MetricsLevel::Off) {
                    Scheduler::Impl::MetricsTracker::saturating_inc(impl->metrics.shard_for_current().steal_attempts);
                }
                if (v < impl->local_deques.size() && impl->local_deques[v]->steal_front_weighted(t_help_steal_cal, task)) {
                    if (impl->metrics.level != MetricsLevel::Off) {
                        Scheduler::Impl::MetricsTracker::saturating_inc(impl->metrics.shard_for_current().steal_successes);
                    }
                    found_task = true;
                    std::lock_guard<std::mutex> lock(impl->lifecycle_mutex);
                    ++impl->active_task_count;
                    break;
                } else {
                    if (impl->metrics.level != MetricsLevel::Off) {
                        Scheduler::Impl::MetricsTracker::saturating_inc(impl->metrics.shard_for_current().steal_failures);
                    }
                }
            }
        }

        if (found_task && task.invoker) {
            task.invoker->execute();
            {
                std::lock_guard<std::mutex> lock(impl->lifecycle_mutex);
                --impl->active_task_count;
            }
            impl->work_cv.notify_all();
        } else {
            // 没有可窃取/帮助的任务，等待目标完成通知
            std::unique_lock<std::mutex> lock(target.mutex());
            if (target.is_completed()) {
                break;
            }
            if (deadline.has_value()) {
                target.cv().wait_until(lock, *deadline, [&target] {
                    return target.is_completed();
                });
            } else {
                target.cv().wait_for(lock, std::chrono::milliseconds(2), [&target] {
                    return target.is_completed();
                });
            }
        }
    }
}

void perform_graph_caller_wait(
    const GraphRunSharedState& target,
    std::optional<std::chrono::steady_clock::time_point> deadline) {
    // 1. Direct Self-Run 检查（R-072 / D-113）
    if (t_current_worker_impl != nullptr &&
        t_current_executing_graph_run_id != GraphRunId{} &&
        t_current_executing_graph_run_id == target.id) {
        throw std::logic_error("cannot wait on own GraphRun inside its node execution");
    }

    // 2. 已完成即时返回
    if (target.run_state.load(std::memory_order_acquire) != GraphRunState::Running) {
        return;
    }

    // 3. 非正/已过期 deadline 即时返回
    if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline) {
        return;
    }

    // 4. 非 Worker 线程：执行无界/有界条件变量等待
    if (t_current_worker_impl == nullptr) {
        std::unique_lock<std::mutex> lock(target.mutex);
        if (deadline.has_value()) {
            target.cv.wait_until(lock, *deadline, [&target] {
                return target.run_state.load(std::memory_order_acquire) != GraphRunState::Running;
            });
        } else {
            target.cv.wait(lock, [&target] {
                return target.run_state.load(std::memory_order_acquire) != GraphRunState::Running;
            });
        }
        return;
    }

    // 5. 同/源 Runtime Worker 线程：执行 Helping Wait（R-072 / D-113）
    auto* impl = static_cast<Scheduler::Impl*>(t_current_worker_impl);

    if (t_current_helping_depth >= impl->options.max_helping_depth) {
        throw helping_depth_exceeded{};
    }

    struct DepthGuard {
        std::size_t& depth;
        explicit DepthGuard(std::size_t& d) noexcept : depth(d) { ++depth; }
        ~DepthGuard() noexcept { --depth; }
    } guard(t_current_helping_depth);

    thread_local std::size_t t_graph_help_local_cal = 0;
    thread_local std::size_t t_graph_help_global_cal = 0;
    thread_local std::size_t t_graph_help_steal_cal = 0;
    thread_local std::array<std::size_t, 4> t_graph_help_deadline_bursts{0, 0, 0, 0};

    while (target.run_state.load(std::memory_order_acquire) == GraphRunState::Running) {
        if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline) {
            break;
        }

        Scheduler::Impl::QueuedTask task;
        bool found_task = false;

        if (t_current_worker_index < impl->local_deques.size()) {
            if (impl->local_deques[t_current_worker_index]->pop_back_weighted(t_graph_help_local_cal, task)) {
                found_task = true;
                std::lock_guard<std::mutex> lock(impl->lifecycle_mutex);
                ++impl->active_task_count;
            }
        }

        if (!found_task) {
            std::unique_lock<std::mutex> lock(impl->lifecycle_mutex);
            const auto st = Scheduler::Impl::unpack(impl->packed_status.load(std::memory_order_acquire));
            if (st.state != SchedulerState::Stopped &&
                st.shutdown_mode != ShutdownMode::Immediate &&
                impl->pop_global_weighted(t_graph_help_global_cal, t_graph_help_deadline_bursts, task)) {
                ++impl->active_task_count;
                found_task = true;
                if (task.is_external && impl->external_pending_count > 0) {
                    --impl->external_pending_count;
                    impl->slot_cv.notify_one();
                }
            }
        }

        if (!found_task) {
            std::vector<std::size_t> victims;
            static thread_local std::uint64_t s_help_rng = 0x854329415849ULL;
            generate_steal_victims(t_current_worker_index, impl->options.worker_count, impl->options.steal_probe_limit, s_help_rng, victims);
            for (std::size_t v : victims) {
                if (impl->metrics.level != MetricsLevel::Off) {
                    Scheduler::Impl::MetricsTracker::saturating_inc(impl->metrics.shard_for_current().steal_attempts);
                }
                if (v < impl->local_deques.size() && impl->local_deques[v]->steal_front_weighted(t_graph_help_steal_cal, task)) {
                    if (impl->metrics.level != MetricsLevel::Off) {
                        Scheduler::Impl::MetricsTracker::saturating_inc(impl->metrics.shard_for_current().steal_successes);
                    }
                    found_task = true;
                    std::lock_guard<std::mutex> lock(impl->lifecycle_mutex);
                    ++impl->active_task_count;
                    break;
                } else {
                    if (impl->metrics.level != MetricsLevel::Off) {
                        Scheduler::Impl::MetricsTracker::saturating_inc(impl->metrics.shard_for_current().steal_failures);
                    }
                }
            }
        }

        if (found_task && task.invoker) {
            task.invoker->execute();
            {
                std::lock_guard<std::mutex> lock(impl->lifecycle_mutex);
                --impl->active_task_count;
            }
            impl->work_cv.notify_all();
        } else {
            std::unique_lock<std::mutex> lock(target.mutex);
            if (target.run_state.load(std::memory_order_acquire) != GraphRunState::Running) {
                break;
            }
            if (deadline.has_value()) {
                target.cv.wait_until(lock, *deadline, [&target] {
                    return target.run_state.load(std::memory_order_acquire) != GraphRunState::Running;
                });
            } else {
                target.cv.wait_for(lock, std::chrono::milliseconds(2), [&target] {
                    return target.run_state.load(std::memory_order_acquire) != GraphRunState::Running;
                });
            }
        }
    }
}

RuntimeId current_worker_runtime_id() noexcept {
    return t_current_worker_runtime_id;
}

namespace {
class FunctionTaskInvoker : public TaskInvokerBase {
public:
    explicit FunctionTaskInvoker(std::function<void()> fn) : fn_(std::move(fn)) {}
    void execute() override {
        if (fn_) {
            fn_();
        }
    }
    void cancel_pre_start() noexcept override {}
private:
    std::function<void()> fn_;
};
}  // namespace

void run_test_task_on_worker(Scheduler& s, std::function<void()> task) {
    if (s.impl_) {
        s.impl_->post_task_internal(std::make_unique<FunctionTaskInvoker>(std::move(task)), false /* not external */);
    }
}

std::size_t global_injection_queue_size(const Scheduler& s) {
    if (s.impl_) {
        std::lock_guard<std::mutex> lock(s.impl_->lifecycle_mutex);
        std::size_t total = 0;
        for (const auto& q : s.impl_->global_injection_queues) {
            total += q.size();
        }
        return total;
    }
    return 0;
}

std::size_t external_pending_count(const Scheduler& s) {
    if (s.impl_) {
        std::lock_guard<std::mutex> lock(s.impl_->lifecycle_mutex);
        return s.impl_->external_pending_count;
    }
    return 0;
}

std::size_t parked_workers_count(const Scheduler& s) {
    if (s.impl_) {
        return s.impl_->parked_workers.load(std::memory_order_acquire);
    }
    return 0;
}

std::uint64_t current_work_epoch(const Scheduler& s) {
    if (s.impl_) {
        return s.impl_->work_epoch.load(std::memory_order_acquire);
    }
    return 0;
}

}  // namespace detail

void Scheduler::Impl::worker_main(std::size_t worker_index) {
    detail::t_current_worker_runtime_id = runtime_id;
    detail::t_current_worker_impl = this;
    detail::t_current_worker_index = worker_index;
    detail::t_current_helping_depth = 0;
    detail::t_current_executing_task_id = TaskId{};

    // 等待 startup 栅栏完成或中止
    {
        std::unique_lock<std::mutex> lock(lifecycle_mutex);
        ++workers_ready;
        startup_cv.notify_all();
        startup_cv.wait(lock, [this] {
            return startup_done || startup_failed || stop_requested;
        });
        if (startup_failed || (!startup_done && stop_requested)) {
            detail::t_current_worker_runtime_id = RuntimeId{0};
            detail::t_current_worker_impl = nullptr;
            if (active_workers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                detail::ReaperRegistry::instance().notify_join_ready(runtime_id);
            }
            return;
        }
    }

    auto& my_local_deque = *local_deques[worker_index];
    std::size_t consecutive_local_count = 0;
    std::size_t local_calendar_idx = 0;
    std::size_t global_calendar_idx = 0;
    std::size_t steal_calendar_idx = 0;
    std::array<std::size_t, 4> global_deadline_bursts{0, 0, 0, 0};
    std::uint64_t rng_state = (static_cast<std::uint64_t>(runtime_id.value()) << 32) ^
                              static_cast<std::uint64_t>(worker_index + 1) ^
                              0x9E3779B97F4A7C15ULL;
    std::vector<std::size_t> victims;

    // 运行期工作循环（执行内部/测试任务，直至收到 stop_requested 且 Drain Closure 排空）
    while (true) {
        process_due_timers();

        QueuedTask task;
        bool found_task = false;

        // 1. 优先尝试从本 Worker 的 Local Deque (LIFO) 获取任务（受 local_burst_limit 上限限制）
        if (consecutive_local_count < options.local_burst_limit) {
            if (my_local_deque.pop_back_weighted(local_calendar_idx, task)) {
                found_task = true;
                ++consecutive_local_count;
                {
                    std::lock_guard<std::mutex> lock(lifecycle_mutex);
                    ++active_task_count;
                }
            }
        }

        // 2. 若 Local 为空或防饥饿阈值触发，探测 Global Injection Queue (FIFO)
        if (!found_task) {
            std::unique_lock<std::mutex> lock(lifecycle_mutex);
            if (pop_global_weighted(global_calendar_idx, global_deadline_bursts, task)) {
                ++active_task_count;
                found_task = true;
                consecutive_local_count = 0;
                if (task.is_external && external_pending_count > 0) {
                    --external_pending_count;
                    slot_cv.notify_one();
                }
            } else if (consecutive_local_count >= options.local_burst_limit) {
                consecutive_local_count = 0;
                lock.unlock();
                if (my_local_deque.pop_back_weighted(local_calendar_idx, task)) {
                    found_task = true;
                    ++consecutive_local_count;
                    std::lock_guard<std::mutex> lk(lifecycle_mutex);
                    ++active_task_count;
                }
            }
        }

        // 3. 若仍无任务，执行 bounded non-repeating Steal Round (R-064)
        if (!found_task) {
            detail::generate_steal_victims(worker_index, options.worker_count, options.steal_probe_limit, rng_state, victims);
            for (std::size_t v : victims) {
                if (metrics.level != MetricsLevel::Off) {
                    MetricsTracker::saturating_inc(metrics.shard_for_current().steal_attempts);
                }
                if (v < local_deques.size() && local_deques[v]->steal_front_weighted(steal_calendar_idx, task)) {
                    if (metrics.level != MetricsLevel::Off) {
                        MetricsTracker::saturating_inc(metrics.shard_for_current().steal_successes);
                    }
                    found_task = true;
                    consecutive_local_count = 0;
                    std::lock_guard<std::mutex> lock(lifecycle_mutex);
                    ++active_task_count;
                    break;
                } else {
                    if (metrics.level != MetricsLevel::Off) {
                        MetricsTracker::saturating_inc(metrics.shard_for_current().steal_failures);
                    }
                }
            }
        }

        // 4. 若仍未获取到任务，进入 Park Handshake 流程 (R-065)
        if (!found_task) {
            process_due_timers();

            // (a) Active backoff: 少量自旋/yield 减少即时睡眠开销
            for (int spin = 0; spin < 16; ++spin) {
#if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
#else
                std::this_thread::yield();
#endif
            }

            // (b) 登记休眠意图并记录当前 work epoch
            parked_workers.fetch_add(1, std::memory_order_seq_cst);
            const std::uint64_t observed_epoch = work_epoch.load(std::memory_order_acquire);

            {
                std::unique_lock<std::mutex> lock(lifecycle_mutex);
                const auto next_wake = earliest_wake_time();
                auto predicate = [this, &my_local_deque, observed_epoch] {
                    if (stop_requested) {
                        const auto st = unpack(packed_status.load(std::memory_order_acquire));
                        if (st.state == SchedulerState::Stopped || st.shutdown_mode == ShutdownMode::Immediate) {
                            return true;
                        }
                        if (!global_queues_empty() || !my_local_deque.empty() || (active_task_count == 0 && !has_timers())) {
                            return true;
                        }
                        for (const auto& d : local_deques) {
                            if (d && !d->empty()) return true;
                        }
                    } else {
                        if (work_epoch.load(std::memory_order_acquire) != observed_epoch) {
                            return true;
                        }
                        if (!global_queues_empty() || !my_local_deque.empty()) {
                            return true;
                        }
                        for (const auto& d : local_deques) {
                            if (d && !d->empty()) return true;
                        }
                    }
                    return false;
                };

                if (metrics.level != MetricsLevel::Off) {
                    MetricsTracker::saturating_inc(metrics.shard_for_current().worker_parks);
                }
                const auto t_park_start = std::chrono::steady_clock::now();
                if (next_wake.has_value()) {
                    work_cv.wait_until(lock, *next_wake, predicate);
                } else {
                    work_cv.wait(lock, predicate);
                }
                const auto t_park_end = std::chrono::steady_clock::now();
                if (metrics.level != MetricsLevel::Off) {
                    MetricsTracker::saturating_inc(metrics.shard_for_current().worker_wakes);
                    if (metrics.level == MetricsLevel::Detailed && t_park_end >= t_park_start) {
                        metrics.shard_for_current().worker_park_duration.record(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(t_park_end - t_park_start).count());
                    }
                }
                parked_workers.fetch_sub(1, std::memory_order_seq_cst);

                process_due_timers();

                if (pop_global_weighted(global_calendar_idx, global_deadline_bursts, task)) {
                    ++active_task_count;
                    found_task = true;
                    consecutive_local_count = 0;
                    if (task.is_external && external_pending_count > 0) {
                        --external_pending_count;
                        slot_cv.notify_one();
                    }
                } else if (my_local_deque.pop_back_weighted(local_calendar_idx, task)) {
                    ++active_task_count;
                    found_task = true;
                    ++consecutive_local_count;
                } else {
                    // 唤醒后尝试窃取一轮
                    lock.unlock();
                    detail::generate_steal_victims(worker_index, options.worker_count, options.steal_probe_limit, rng_state, victims);
                    for (std::size_t v : victims) {
                        if (metrics.level != MetricsLevel::Off) {
                            MetricsTracker::saturating_inc(metrics.shard_for_current().steal_attempts);
                        }
                        if (v < local_deques.size() && local_deques[v]->steal_front_weighted(steal_calendar_idx, task)) {
                            if (metrics.level != MetricsLevel::Off) {
                                MetricsTracker::saturating_inc(metrics.shard_for_current().steal_successes);
                            }
                            found_task = true;
                            consecutive_local_count = 0;
                            std::lock_guard<std::mutex> lk(lifecycle_mutex);
                            ++active_task_count;
                            break;
                        } else {
                            if (metrics.level != MetricsLevel::Off) {
                                MetricsTracker::saturating_inc(metrics.shard_for_current().steal_failures);
                            }
                        }
                    }
                    lock.lock();
                }

                if (!found_task && stop_requested) {
                    const auto st = unpack(packed_status.load(std::memory_order_acquire));
                    if (st.state == SchedulerState::Stopped) {
                        break;
                    }
                    if (st.shutdown_mode == ShutdownMode::Immediate) {
                        bool any_resumes = false;
                        for (const auto& q : global_injection_queues) {
                            for (const auto& entry : q) {
                                if (entry.invoker && entry.invoker->is_resume_segment()) {
                                    any_resumes = true;
                                    break;
                                }
                            }
                            if (any_resumes) break;
                        }
                        if (!any_resumes && !my_local_deque.empty()) {
                            any_resumes = true;
                        }
                        if (!any_resumes) {
                            for (const auto& d : local_deques) {
                                if (d && !d->empty()) {
                                    any_resumes = true;
                                    break;
                                }
                            }
                        }
                        if (!any_resumes && active_task_count == 0) {
                            work_cv.notify_all();
                            break;
                        }
                    } else {
                        bool any_tasks = !global_queues_empty() || !my_local_deque.empty();
                        if (!any_tasks) {
                            for (const auto& d : local_deques) {
                                if (d && !d->empty()) {
                                    any_tasks = true;
                                    break;
                                }
                            }
                        }
                        bool any_timers = has_timers();
                        if (!any_tasks && active_task_count == 0 && !any_timers) {
                            work_cv.notify_all();
                            break;
                        }
                    }
                }
            }
        }

        // 5. 执行获取到的任务
        if (found_task && task.invoker) {
            const auto st = get_status();
            if (st.shutdown_mode == ShutdownMode::Immediate && !task.invoker->is_resume_segment()) {
                task.invoker->cancel_pre_start();
            } else {
                task.invoker->execute();
            }
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex);
                --active_task_count;
            }
            work_cv.notify_all();
        }
    }

    detail::t_current_worker_runtime_id = RuntimeId{0};
    detail::t_current_worker_impl = nullptr;
    detail::t_current_helping_depth = 0;
    detail::t_current_executing_task_id = TaskId{};
    if (active_workers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        detail::ReaperRegistry::instance().notify_join_ready(runtime_id);
    }
}

detail::AdmissionDecision Scheduler::acquire_admission(bool block, bool is_internal) const {
    if (!impl_) {
        throw std::logic_error("operating on empty/moved-from Scheduler");
    }
    return impl_->acquire_admission_slot(block, is_internal);
}

void Scheduler::rollback_external_slot() const {
    if (impl_) {
        impl_->release_external_slot();
    }
}

void Scheduler::post_task_invoker(std::unique_ptr<detail::TaskInvokerBase> invoker, bool is_external) const {
    if (!impl_) {
        throw std::logic_error("operating on empty/moved-from Scheduler");
    }
    impl_->post_task_internal(std::move(invoker), is_external);
}

std::uint64_t Scheduler::register_timer(std::chrono::steady_clock::time_point wake_time,
                                        std::shared_ptr<AwaitHandshake> handshake,
                                        std::function<void()> resume_action) const {
    if (!impl_) {
        throw std::logic_error("operating on empty/moved-from Scheduler");
    }
    return impl_->register_timer(wake_time, std::move(handshake), std::move(resume_action));
}

void Scheduler::cancel_timer(std::uint64_t timer_id) const {
    if (impl_) {
        impl_->cancel_timer(timer_id);
    }
}

Scheduler::Scheduler(SchedulerOptions options) {
    validate_options(options);
    const RuntimeId id = allocate_runtime_id();
    const auto backend = detail::ChaseLevDeque<void*>::is_lock_free()
                             ? LocalDequeBackend::ChaseLevLockFree
                             : LocalDequeBackend::Locked;
    const SchedulerCapabilities caps{backend};
    impl_ = std::make_shared<Impl>(id, std::move(options), caps);
}

Scheduler::~Scheduler() noexcept {
    if (impl_) {
        // R-103 / R-105: 只有最后一个 Handle 销毁才触发关停
        if (impl_.use_count() == 1) {
            if (detail::t_current_worker_runtime_id == impl_->runtime_id) {
                // R-021 / R-022: Worker 线程销毁最后 Handle 触发异步 orphan handoff
                impl_->execute_worker_orphan_handoff(impl_);
                impl_.reset();
                return;
            } else {
                // R-103 / R-105: 非 Worker 线程销毁最后 Handle 触发同步 Graceful RAII
                try {
                    impl_->shutdown_sync(ShutdownMode::Graceful);
                } catch (...) {
                    // noexcept 析构必须吸收异常
                }
            }
        }
    }
}

Scheduler::Scheduler(const Scheduler&) = default;
Scheduler& Scheduler::operator=(const Scheduler&) = default;

Scheduler::Scheduler(Scheduler&&) noexcept = default;
Scheduler& Scheduler::operator=(Scheduler&&) noexcept = default;

bool Scheduler::valid() const noexcept {
    return static_cast<bool>(impl_);
}

RuntimeId Scheduler::runtime_id() const noexcept {
    return impl_ ? impl_->runtime_id : RuntimeId{};
}

SchedulerStatus Scheduler::status() const {
    if (!impl_) {
        throw std::logic_error("operating on empty/moved-from Scheduler");
    }
    return impl_->get_status();
}

SchedulerCapabilities Scheduler::capabilities() const {
    if (!impl_) {
        throw std::logic_error("operating on empty/moved-from Scheduler");
    }
    return impl_->capabilities;
}

void Scheduler::shutdown() {
    if (!impl_) {
        throw std::logic_error("operating on empty/moved-from Scheduler");
    }
    // R-019: 如果已经处于 Stopped 状态，立即成功返回且无任何副作用
    if (impl_->get_status().state == SchedulerState::Stopped) {
        return;
    }
    // R-013 / R-108 / D-011 / D-166: 目标 Worker 调用 self-shutdown 必须在副作用前抛 std::logic_error
    if (detail::current_worker_runtime_id() == impl_->runtime_id) {
        throw std::logic_error("self-shutdown attempted from worker of the same runtime");
    }
    impl_->shutdown_sync(ShutdownMode::Graceful);
}

void Scheduler::shutdown_now() {
    if (!impl_) {
        throw std::logic_error("operating on empty/moved-from Scheduler");
    }
    // R-019: 如果已经处于 Stopped 状态，立即成功返回且无任何副作用
    if (impl_->get_status().state == SchedulerState::Stopped) {
        return;
    }
    // R-011 / R-108 / D-009 / D-166: 目标 Worker 调用 self-shutdown_now 必须在副作用前抛 std::logic_error
    if (detail::current_worker_runtime_id() == impl_->runtime_id) {
        throw std::logic_error("self-shutdown_now attempted from worker of the same runtime");
    }
    impl_->shutdown_sync(ShutdownMode::Immediate);
}

GraphRun Scheduler::run_impl(std::optional<TaskOptions> options, FrozenTaskGraph&& graph) {
    if (!valid()) {
        throw std::logic_error("operating on empty/moved-from Scheduler");
    }

    if (impl_->metrics.level != MetricsLevel::Off) {
        Scheduler::Impl::MetricsTracker::saturating_inc(
            impl_->metrics.shard_for_current().graph_admission_attempts);
    }

    const std::size_t n = graph.node_count();
    const bool is_internal = (detail::current_worker_runtime_id() == runtime_id());
    const bool is_worker = (detail::current_worker_runtime_id() != RuntimeId{0});
    const bool can_block = !is_worker;

    Priority graph_priority = Priority::Normal;
    if (options.has_value()) {
        validate_priority(options->priority);
        graph_priority = options->priority;
    } else if (is_internal) {
        graph_priority = detail::current_executing_task_priority();
    } else {
        graph_priority = Priority::Normal;
    }

    // R-070 / D-106: all-or-nothing slot reservation
    const auto decision = impl_->acquire_admission_slots(n, can_block, is_internal);
    if (decision == detail::AdmissionDecision::Stopping ||
        decision == detail::AdmissionDecision::Stopped ||
        decision == detail::AdmissionDecision::CapacityExhausted) {
        if (impl_->metrics.level != MetricsLevel::Off) {
            Scheduler::Impl::MetricsTracker::saturating_inc(
                impl_->metrics.shard_for_current().graph_runs_rejected);
        }
        if (decision == detail::AdmissionDecision::Stopping) {
            throw submission_rejected(SubmissionError::Stopping);
        }
        if (decision == detail::AdmissionDecision::Stopped) {
            throw submission_rejected(SubmissionError::Stopped);
        }
        if (decision == detail::AdmissionDecision::CapacityExhausted) {
            throw submission_rejected(SubmissionError::CapacityExhausted);
        }
    }

    if (impl_->metrics.level != MetricsLevel::Off) {
        Scheduler::Impl::MetricsTracker::saturating_inc(
            impl_->metrics.shard_for_current().graph_runs_accepted);
        impl_->metrics.active_graph_runs.fetch_add(1, std::memory_order_relaxed);
    }

    const GraphRunId gid = impl_->allocate_graph_run_id();

    // R-070: 空图直接完成
    if (n == 0) {
        auto state = std::make_shared<detail::GraphRunSharedState>(gid, 0);
        state->run_state.store(GraphRunState::Succeeded, std::memory_order_release);
        if (impl_->metrics.level != MetricsLevel::Off) {
            if (impl_->metrics.active_graph_runs.load(std::memory_order_relaxed) > 0) {
                impl_->metrics.active_graph_runs.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        return GraphRun(std::move(state));
    }

    std::shared_ptr<detail::GraphRunSharedState> state;
    try {
        state = std::make_shared<detail::GraphRunSharedState>(gid, n);
    } catch (...) {
        if (impl_->metrics.level != MetricsLevel::Off) {
            if (impl_->metrics.active_graph_runs.load(std::memory_order_relaxed) > 0) {
                impl_->metrics.active_graph_runs.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        if (!is_internal) {
            impl_->release_external_slots(n);
        }
        throw;
    }

    // 填充 node_entries 与 edges
    auto& nodes = graph.nodes_internal();
    for (auto& node_data : nodes) {
        const std::size_t idx = node_data.id.value();
        Priority node_priority = graph_priority;
        std::optional<TaskDeadline> node_deadline = std::nullopt;
        if (node_data.options.has_value()) {
            validate_priority(node_data.options->priority);
            node_priority = node_data.options->priority;
            node_deadline = node_data.options->deadline;
        }
        state->node_entries[idx].id = node_data.id;
        state->node_entries[idx].invoker = std::move(node_data.invoker);
        state->node_entries[idx].priority = node_priority;
        state->node_entries[idx].deadline = node_deadline;
        if (state->node_entries[idx].invoker && state->node_entries[idx].invoker->is_coroutine_node()) {
            auto* coro_node = static_cast<detail::GraphCoroutineNodeInvoker*>(state->node_entries[idx].invoker.get());
            const TaskId task_id = detail::allocate_task_id(impl_->runtime_id);
            auto task_state = std::make_shared<detail::TaskSharedState<void>>(task_id, node_priority, node_deadline);
            task_state->set_timer_functions(
                [impl_ptr = impl_.get()](std::chrono::steady_clock::time_point wt, std::shared_ptr<AwaitHandshake> hs, std::function<void()> act) {
                    return impl_ptr->register_timer(wt, std::move(hs), std::move(act));
                },
                [impl_ptr = impl_.get()](std::uint64_t tid) {
                    impl_ptr->cancel_timer(tid);
                }
            );
            coro_node->coro.promise().shared_state = task_state;
            coro_node->task_state = task_state;
        }
    }

    for (const auto& edge : graph.edges()) {
        const std::size_t u = edge.from.value();
        const std::size_t v = edge.to.value();
        state->node_entries[v].remaining_predecessors.fetch_add(1, std::memory_order_relaxed);
        state->node_entries[u].successors.push_back({edge.to, edge.policy});
    }

    // 递归/内部分发与依赖传播函数（R-070 / R-071 / D-107 / D-109 / D-110）
    auto trigger_successors = [impl_ptr = impl_.get(), state, is_internal](auto self, auto post_node_fn, NodeId u_id) -> void {
        const std::size_t node_idx = u_id.value();
        auto& entry = state->node_entries[node_idx];
        const TaskState u_outcome = entry.outcome.load(std::memory_order_acquire);

        for (const auto& [succ_id, policy] : entry.successors) {
            auto& succ_entry = state->node_entries[succ_id.value()];
            if (policy == EdgePolicy::RequireSuccess && u_outcome != TaskState::Succeeded) {
                succ_entry.has_failed_required_predecessor.store(true, std::memory_order_release);
            }
            if (succ_entry.remaining_predecessors.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                // 成为唯一 1->0 successor release owner
                if (succ_entry.has_failed_required_predecessor.load(std::memory_order_acquire) ||
                    state->cancel_requested.load(std::memory_order_acquire)) {
                    // 前置依赖失败或图已取消：取消该后继节点，不执行 Callable，直接发布 Cancelled 并释放 external slot
                    if (!is_internal) {
                        impl_ptr->release_external_slots(1);
                    }
                    state->mark_node_terminal(succ_id.value(), TaskState::Cancelled);
                    // 递归触发 succ_id 的后继
                    self(self, post_node_fn, succ_id);
                } else {
                    // 前置依赖全部满足且未取消：发布给 Scheduler 调度执行
                    post_node_fn(post_node_fn, succ_id);
                }
            }
        }
    };

    auto post_node = [impl_ptr = impl_.get(), state, is_internal, trigger_successors](auto self, NodeId u_id) -> void {
        const std::size_t node_idx = u_id.value();
        auto& entry = state->node_entries[node_idx];
        const Priority node_p = entry.priority;

        if (entry.invoker && entry.invoker->is_coroutine_node()) {
            auto* coro_node = static_cast<detail::GraphCoroutineNodeInvoker*>(entry.invoker.get());
            auto task_state = coro_node->task_state;
            auto coro = coro_node->coro;
            coro_node->coro = nullptr;

            task_state->set_rescheduler([impl_ptr, state, u_id, is_internal, task_state, trigger_successors, self](std::unique_ptr<detail::TaskInvokerBase> invoker) {
                auto trigger_fn = [trigger_successors, self](NodeId id) {
                    trigger_successors(trigger_successors, self, id);
                };
                auto wrapper = std::make_unique<detail::GraphCoroutineResumeWrapper>(
                    std::move(invoker), state, task_state, u_id, std::move(trigger_fn));
                impl_ptr->post_task_internal(std::move(wrapper), false /* is_external */);
            });

            auto task_fn = [impl_ptr, state, u_id, is_internal, self, trigger_successors, task_state, coro, node_p] {
                const std::size_t n_idx = u_id.value();

                if (state->cancel_requested.load(std::memory_order_acquire)) {
                    task_state->request_cancel();
                }

                detail::GraphNodeExecutionContextGuard node_guard(state->id, node_p);

                auto start_invoker = std::make_unique<detail::CoroutineTaskInvokerModel<void>>(
                    coro, task_state);
                start_invoker->execute();

                if (task_state->is_completed()) {
                    TaskState outcome = task_state->state();
                    std::exception_ptr ex = (outcome == TaskState::Failed) ? task_state->exception() : nullptr;
                    state->mark_node_terminal(n_idx, outcome, ex);
                    trigger_successors(trigger_successors, self, u_id);
                }
            };

            impl_ptr->post_task_internal(
                detail::make_graph_node_invoker<true>(std::move(task_fn), node_p, task_state->deadline()),
                !is_internal);
            return;
        }

        auto task_fn = [impl_ptr, state, u_id, is_internal, self, trigger_successors, node_p] {
            const std::size_t n_idx = u_id.value();
            auto& node_entry = state->node_entries[n_idx];

            if (state->cancel_requested.load(std::memory_order_acquire)) {
                state->mark_node_terminal(n_idx, TaskState::Cancelled);
                trigger_successors(trigger_successors, self, u_id);
                return;
            }

            detail::GraphNodeExecutionContextGuard node_guard(state->id, node_p);

            if (node_entry.deadline.has_value() && node_entry.deadline_disposition == DeadlineDisposition::None) {
                const auto now = std::chrono::steady_clock::now();
                if (now <= node_entry.deadline->time_point()) {
                    node_entry.deadline_disposition = DeadlineDisposition::Met;
                } else {
                    node_entry.deadline_disposition = DeadlineDisposition::Missed;
                    const auto lateness = std::chrono::duration_cast<std::chrono::nanoseconds>(now - node_entry.deadline->time_point()).count();
                    detail::record_metrics_deadline_start_lateness(TaskId{impl_ptr->runtime_id, n_idx}, lateness);
                }
            }
            detail::record_metrics_first_start(TaskId{impl_ptr->runtime_id, n_idx},
                node_entry.deadline.has_value() ? std::optional{node_entry.deadline_disposition} : std::nullopt);

            const auto t_exec_start = std::chrono::steady_clock::now();
            try {
                if (node_entry.invoker) {
                    node_entry.invoker->execute();
                }
                const auto t_exec_end = std::chrono::steady_clock::now();
                detail::record_metrics_execution_segment(TaskId{impl_ptr->runtime_id, n_idx},
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t_exec_end - t_exec_start).count());
                detail::record_metrics_succeeded(TaskId{impl_ptr->runtime_id, n_idx});
                state->mark_node_terminal(n_idx, TaskState::Succeeded);
            } catch (const astra::task_cancelled&) {
                const auto t_exec_end = std::chrono::steady_clock::now();
                detail::record_metrics_execution_segment(TaskId{impl_ptr->runtime_id, n_idx},
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t_exec_end - t_exec_start).count());
                detail::record_metrics_cancelled_cooperative(TaskId{impl_ptr->runtime_id, n_idx});
                state->mark_node_terminal(n_idx, TaskState::Cancelled);
            } catch (...) {
                const auto t_exec_end = std::chrono::steady_clock::now();
                detail::record_metrics_execution_segment(TaskId{impl_ptr->runtime_id, n_idx},
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t_exec_end - t_exec_start).count());
                detail::record_metrics_failed(TaskId{impl_ptr->runtime_id, n_idx});
                state->mark_node_terminal(n_idx, TaskState::Failed, std::current_exception());
            }

            trigger_successors(trigger_successors, self, u_id);
        };

        impl_ptr->post_task_internal(
            detail::make_graph_node_invoker<true>(std::move(task_fn), node_p, entry.deadline),
            !is_internal);
    };

    // 寻找所有 0-predecessor roots 并直接发布 Ready
    std::vector<NodeId> roots;
    roots.reserve(n);
    for (std::size_t i = 1; i <= n; ++i) {
        if (state->node_entries[i].remaining_predecessors.load(std::memory_order_relaxed) == 0) {
            roots.push_back(NodeId{i});
        }
    }

    for (NodeId root_id : roots) {
        post_node(post_node, root_id);
    }

    return GraphRun(std::move(state));
}

GraphRun Scheduler::run(FrozenTaskGraph&& graph) {
    return run_impl(std::nullopt, std::move(graph));
}

GraphRun Scheduler::run(TaskOptions options, FrozenTaskGraph&& graph) {
    return run_impl(options, std::move(graph));
}

RuntimeMetricsSnapshot Scheduler::metrics_snapshot() const {
    if (!impl_) {
        throw std::logic_error("operating on empty/moved-from Scheduler");
    }
    const auto t_start = std::chrono::steady_clock::now();
    RuntimeMetricsSnapshot snapshot{};
    snapshot.schema_version = 1;
    snapshot.runtime_id = impl_->runtime_id;
    snapshot.capture_started_at = t_start;
    snapshot.metrics_level = impl_->options.metrics_level;
    snapshot.worker_count = impl_->options.worker_count;
    const auto st = Impl::unpack(impl_->packed_status.load(std::memory_order_acquire));
    snapshot.scheduler_state = st.state;
    snapshot.shutdown_mode = st.shutdown_mode;

    if (impl_->options.metrics_level == MetricsLevel::Off) {
        snapshot.enabled = false;
        snapshot.saturated = false;
        snapshot.capture_finished_at = std::chrono::steady_clock::now();
        return snapshot;
    }

    snapshot.enabled = true;
    bool any_saturated = false;

    auto add_to = [&any_saturated](std::uint64_t& dst, std::uint64_t src) {
        if (src == std::numeric_limits<std::uint64_t>::max() ||
            std::numeric_limits<std::uint64_t>::max() - dst < src) {
            any_saturated = true;
            dst = std::numeric_limits<std::uint64_t>::max();
        } else {
            dst += src;
        }
    };

    auto accumulate_shard = [&](const Impl::MetricsTracker::WorkerShard& shard) {
        add_to(snapshot.counters.submission_attempts, shard.submission_attempts.load(std::memory_order_relaxed));
        add_to(snapshot.counters.accepted_task_identities, shard.accepted_task_identities.load(std::memory_order_relaxed));
        add_to(snapshot.counters.rejected_lifecycle, shard.rejected_lifecycle.load(std::memory_order_relaxed));
        add_to(snapshot.counters.rejected_capacity, shard.rejected_capacity.load(std::memory_order_relaxed));
        add_to(snapshot.counters.blocking_submit_waits, shard.blocking_submit_waits.load(std::memory_order_relaxed));
        add_to(snapshot.counters.blocking_submit_wakeups, shard.blocking_submit_wakeups.load(std::memory_order_relaxed));

        add_to(snapshot.counters.first_starts, shard.first_starts.load(std::memory_order_relaxed));
        add_to(snapshot.counters.resume_segments, shard.resume_segments.load(std::memory_order_relaxed));
        add_to(snapshot.counters.succeeded, shard.succeeded.load(std::memory_order_relaxed));
        add_to(snapshot.counters.failed, shard.failed.load(std::memory_order_relaxed));
        add_to(snapshot.counters.cancelled_before_start, shard.cancelled_before_start.load(std::memory_order_relaxed));
        add_to(snapshot.counters.cancelled_cooperative, shard.cancelled_cooperative.load(std::memory_order_relaxed));
        add_to(snapshot.counters.unobserved_failures, shard.unobserved_failures.load(std::memory_order_relaxed));

        add_to(snapshot.counters.global_claims, shard.global_claims.load(std::memory_order_relaxed));
        add_to(snapshot.counters.local_claims, shard.local_claims.load(std::memory_order_relaxed));
        add_to(snapshot.counters.steal_attempts, shard.steal_attempts.load(std::memory_order_relaxed));
        add_to(snapshot.counters.steal_successes, shard.steal_successes.load(std::memory_order_relaxed));
        add_to(snapshot.counters.steal_failures, shard.steal_failures.load(std::memory_order_relaxed));
        add_to(snapshot.counters.worker_parks, shard.worker_parks.load(std::memory_order_relaxed));
        add_to(snapshot.counters.worker_wakes, shard.worker_wakes.load(std::memory_order_relaxed));
        add_to(snapshot.counters.explicit_yields, shard.explicit_yields.load(std::memory_order_relaxed));

        add_to(snapshot.counters.coroutine_suspends, shard.coroutine_suspends.load(std::memory_order_relaxed));
        add_to(snapshot.counters.timer_registrations, shard.timer_registrations.load(std::memory_order_relaxed));
        add_to(snapshot.counters.timer_fires, shard.timer_fires.load(std::memory_order_relaxed));
        add_to(snapshot.counters.timer_cancellations, shard.timer_cancellations.load(std::memory_order_relaxed));

        add_to(snapshot.counters.graph_admission_attempts, shard.graph_admission_attempts.load(std::memory_order_relaxed));
        add_to(snapshot.counters.graph_runs_accepted, shard.graph_runs_accepted.load(std::memory_order_relaxed));
        add_to(snapshot.counters.graph_runs_rejected, shard.graph_runs_rejected.load(std::memory_order_relaxed));
        add_to(snapshot.counters.graph_nodes_terminal, shard.graph_nodes_terminal.load(std::memory_order_relaxed));

        add_to(snapshot.counters.deadline_admitted, shard.deadline_admitted.load(std::memory_order_relaxed));
        add_to(snapshot.counters.deadline_met, shard.deadline_met.load(std::memory_order_relaxed));
        add_to(snapshot.counters.deadline_missed, shard.deadline_missed.load(std::memory_order_relaxed));
        add_to(snapshot.counters.deadline_cancelled_before_start, shard.deadline_cancelled_before_start.load(std::memory_order_relaxed));
    };

    if (impl_->metrics.control_shard) {
        accumulate_shard(*impl_->metrics.control_shard);
    }
    for (const auto& shard_ptr : impl_->metrics.worker_shards) {
        if (shard_ptr) {
            accumulate_shard(*shard_ptr);
        }
    }

    if (impl_->options.metrics_level == MetricsLevel::Detailed) {
        auto accumulate_hist = [&add_to](Log2Histogram& dst, const Impl::MetricsTracker::WorkerShard::ShardedHistogram& src) {
            add_to(dst.count, src.count.load(std::memory_order_relaxed));
            add_to(dst.sum_ns, src.sum_ns.load(std::memory_order_relaxed));
            const std::uint64_t src_max = src.max_ns.load(std::memory_order_relaxed);
            if (src_max > dst.max_ns) {
                dst.max_ns = src_max;
            }
            for (std::size_t i = 0; i < Log2Histogram::kBucketCount; ++i) {
                add_to(dst.buckets[i], src.buckets[i].load(std::memory_order_relaxed));
            }
        };

        auto accumulate_histograms = [&](const Impl::MetricsTracker::WorkerShard& shard) {
            accumulate_hist(snapshot.histograms.ready_queue_wait, shard.ready_queue_wait);
            accumulate_hist(snapshot.histograms.execution_segment, shard.execution_segment);
            accumulate_hist(snapshot.histograms.task_wall_time, shard.task_wall_time);
            accumulate_hist(snapshot.histograms.blocking_admission_wait, shard.blocking_admission_wait);
            accumulate_hist(snapshot.histograms.timer_wake_lateness, shard.timer_wake_lateness);
            accumulate_hist(snapshot.histograms.deadline_start_lateness, shard.deadline_start_lateness);
            accumulate_hist(snapshot.histograms.worker_park_duration, shard.worker_park_duration);
            accumulate_hist(snapshot.histograms.runtime_join_latency, shard.runtime_join_latency);
        };

        if (impl_->metrics.control_shard) {
            accumulate_histograms(*impl_->metrics.control_shard);
        }
        for (const auto& shard_ptr : impl_->metrics.worker_shards) {
            if (shard_ptr) {
                accumulate_histograms(*shard_ptr);
            }
        }
    }

    snapshot.gauges.waiting_tasks = impl_->metrics.waiting_tasks.load(std::memory_order_relaxed);
    snapshot.gauges.ready_tasks = impl_->metrics.ready_tasks.load(std::memory_order_relaxed);
    snapshot.gauges.running_tasks = impl_->metrics.running_tasks.load(std::memory_order_relaxed);
    snapshot.gauges.suspended_tasks = impl_->metrics.suspended_tasks.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
        snapshot.gauges.external_pending_slots_used = static_cast<std::uint64_t>(impl_->external_pending_count);
    }
    snapshot.gauges.parked_workers = static_cast<std::uint64_t>(impl_->parked_workers.load(std::memory_order_relaxed));
    {
        std::lock_guard<std::mutex> lock(impl_->timer_mutex);
        snapshot.gauges.active_timer_entries = static_cast<std::uint64_t>(impl_->timer_heap.size());
    }
    snapshot.gauges.active_graph_runs = impl_->metrics.active_graph_runs.load(std::memory_order_relaxed);

    snapshot.saturated = any_saturated;
    snapshot.capture_finished_at = std::chrono::steady_clock::now();
    return snapshot;
}

}  // namespace astra

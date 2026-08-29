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

    struct QueuedTask {
        std::unique_ptr<detail::TaskInvokerBase> invoker;
        bool is_external{false};
    };
    std::deque<QueuedTask> global_injection_queue;

    struct LockedLocalDeque {
        mutable std::mutex mutex;
        std::deque<QueuedTask> tasks;

        void push_back(QueuedTask task) {
            std::lock_guard<std::mutex> lock(mutex);
            tasks.push_back(std::move(task));
        }

        bool pop_back(QueuedTask& out) {
            std::lock_guard<std::mutex> lock(mutex);
            if (tasks.empty()) {
                return false;
            }
            out = std::move(tasks.back());
            tasks.pop_back();
            return true;
        }

        bool steal_front(QueuedTask& out) {
            std::lock_guard<std::mutex> lock(mutex);
            if (tasks.empty()) {
                return false;
            }
            out = std::move(tasks.front());
            tasks.pop_front();
            return true;
        }

        bool empty() const {
            std::lock_guard<std::mutex> lock(mutex);
            return tasks.empty();
        }

        std::size_t size() const {
            std::lock_guard<std::mutex> lock(mutex);
            return tasks.size();
        }
    };
    std::vector<std::unique_ptr<LockedLocalDeque>> local_deques;

    Impl(RuntimeId id, SchedulerOptions opts, SchedulerCapabilities caps)
        : runtime_id(id),
          options(std::move(opts)),
          capabilities(caps),
          packed_status(pack(SchedulerState::Running, ShutdownMode::None)) {
        
        local_deques.reserve(options.worker_count);
        for (std::size_t i = 0; i < options.worker_count; ++i) {
            local_deques.push_back(std::make_unique<LockedLocalDeque>());
        }
        
        // 1. Reaper 注册与能力预留（R-023, R-024, R-097）
        auto& registry = detail::ReaperRegistry::instance();
        if (!registry.is_registration_open()) {
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
            throw;
        }
    }

    ~Impl() {
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

            // Leader 负责 join 全部 worker 线程（恰好 join 一次）
            for (auto& t : worker_threads) {
                if (t.joinable()) {
                    t.join();
                }
            }
            worker_threads.clear();

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
        std::deque<QueuedTask> remaining_global;
        while (!global_injection_queue.empty()) {
            auto task = std::move(global_injection_queue.front());
            global_injection_queue.pop_front();
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
        global_injection_queue = std::move(remaining_global);

        for (auto& d : local_deques) {
            if (d) {
                std::lock_guard<std::mutex> lk(d->mutex);
                std::deque<QueuedTask> remaining_local;
                while (!d->tasks.empty()) {
                    auto task = std::move(d->tasks.front());
                    d->tasks.pop_front();
                    if (task.invoker && task.invoker->is_resume_segment()) {
                        remaining_local.push_back(std::move(task));
                    } else {
                        if (task.invoker) {
                            task.invoker->cancel_pre_start();
                        }
                    }
                }
                d->tasks = std::move(remaining_local);
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

            for (auto& t : worker_threads) {
                if (t.joinable()) {
                    t.join();
                }
            }
            worker_threads.clear();

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
                return detail::AdmissionDecision::Stopped;
            }
            if (st.state == SchedulerState::Stopping &&
                (!is_internal || st.shutdown_mode != ShutdownMode::Graceful)) {
                return detail::AdmissionDecision::Stopping;
            }
            return detail::AdmissionDecision::Success;
        }

        std::unique_lock<std::mutex> lock(lifecycle_mutex);
        while (true) {
            const auto st = unpack(packed_status.load(std::memory_order_acquire));
            if (st.state == SchedulerState::Stopped) {
                return detail::AdmissionDecision::Stopped;
            }
            if (st.state == SchedulerState::Stopping) {
                // R-006 / D-002: Graceful Stopping 接受授权的 Internal Submission
                if (is_internal && st.shutdown_mode == ShutdownMode::Graceful) {
                    return detail::AdmissionDecision::Success;
                }
                return detail::AdmissionDecision::Stopping;
            }

            if (is_internal) {
                return detail::AdmissionDecision::Success;
            }

            // R-070 / D-106: 若 count > external_pending_capacity，即使 policy 为 Block 也立即以 CapacityExhausted 拒绝
            if (count > options.external_pending_capacity) {
                return detail::AdmissionDecision::CapacityExhausted;
            }

            if (external_pending_count + count <= options.external_pending_capacity) {
                external_pending_count += count;
                return detail::AdmissionDecision::Success;
            }

            if (!block || options.external_backpressure != ExternalBackpressure::Block) {
                return detail::AdmissionDecision::CapacityExhausted;
            }

            // Ordinary thread waiting on slot_cv (R-061 / D-086 / D-106)
            slot_cv.wait(lock, [this, count] {
                const auto current_st = unpack(packed_status.load(std::memory_order_acquire));
                return current_st.state != SchedulerState::Running ||
                       (external_pending_count + count <= options.external_pending_capacity);
            });
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
        std::vector<std::pair<std::shared_ptr<AwaitHandshake>, std::function<void()>>> due_items;
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
                due_items.push_back({std::move(entry->handshake), std::move(entry->resume_action)});
            }
        }
        for (auto& [hs, act] : due_items) {
            if (hs && act) {
                hs->trigger(act);
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
        if (!is_external && detail::t_current_worker_impl == this &&
            detail::t_current_worker_runtime_id == runtime_id &&
            detail::t_current_worker_index < local_deques.size()) {
            local_deques[detail::t_current_worker_index]->push_back({std::move(task), false});
        } else {
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex);
                global_injection_queue.push_back({std::move(task), is_external});
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

namespace detail {

thread_local RuntimeId t_current_worker_runtime_id{0};
thread_local void* t_current_worker_impl{nullptr};
thread_local std::size_t t_current_worker_index{0};
thread_local TaskId t_current_executing_task_id{};
thread_local Priority t_current_executing_task_priority{Priority::Normal};
thread_local GraphRunId t_current_executing_graph_run_id{};
thread_local std::size_t t_current_helping_depth{0};

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

    while (!target.is_completed()) {
        if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline) {
            break;
        }

        Scheduler::Impl::QueuedTask task;
        bool found_task = false;

        if (t_current_worker_index < impl->local_deques.size()) {
            if (impl->local_deques[t_current_worker_index]->pop_back(task)) {
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
                !impl->global_injection_queue.empty()) {
                task = std::move(impl->global_injection_queue.front());
                impl->global_injection_queue.pop_front();
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
                if (v < impl->local_deques.size() && impl->local_deques[v]->steal_front(task)) {
                    found_task = true;
                    std::lock_guard<std::mutex> lock(impl->lifecycle_mutex);
                    ++impl->active_task_count;
                    break;
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

    while (target.run_state.load(std::memory_order_acquire) == GraphRunState::Running) {
        if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline) {
            break;
        }

        Scheduler::Impl::QueuedTask task;
        bool found_task = false;

        if (t_current_worker_index < impl->local_deques.size()) {
            if (impl->local_deques[t_current_worker_index]->pop_back(task)) {
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
                !impl->global_injection_queue.empty()) {
                task = std::move(impl->global_injection_queue.front());
                impl->global_injection_queue.pop_front();
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
                if (v < impl->local_deques.size() && impl->local_deques[v]->steal_front(task)) {
                    found_task = true;
                    std::lock_guard<std::mutex> lock(impl->lifecycle_mutex);
                    ++impl->active_task_count;
                    break;
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
        return s.impl_->global_injection_queue.size();
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
            if (my_local_deque.pop_back(task)) {
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
            if (!global_injection_queue.empty()) {
                task = std::move(global_injection_queue.front());
                global_injection_queue.pop_front();
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
                if (my_local_deque.pop_back(task)) {
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
                if (v < local_deques.size() && local_deques[v]->steal_front(task)) {
                    found_task = true;
                    consecutive_local_count = 0;
                    std::lock_guard<std::mutex> lock(lifecycle_mutex);
                    ++active_task_count;
                    break;
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
                        if (!global_injection_queue.empty() || !my_local_deque.empty() || (active_task_count == 0 && !has_timers())) {
                            return true;
                        }
                        for (const auto& d : local_deques) {
                            if (d && !d->empty()) return true;
                        }
                    } else {
                        if (work_epoch.load(std::memory_order_acquire) != observed_epoch) {
                            return true;
                        }
                        if (!global_injection_queue.empty() || !my_local_deque.empty()) {
                            return true;
                        }
                        for (const auto& d : local_deques) {
                            if (d && !d->empty()) return true;
                        }
                    }
                    return false;
                };

                if (next_wake.has_value()) {
                    work_cv.wait_until(lock, *next_wake, predicate);
                } else {
                    work_cv.wait(lock, predicate);
                }
                parked_workers.fetch_sub(1, std::memory_order_seq_cst);

                process_due_timers();

                if (!global_injection_queue.empty()) {
                    task = std::move(global_injection_queue.front());
                    global_injection_queue.pop_front();
                    ++active_task_count;
                    found_task = true;
                    consecutive_local_count = 0;
                    if (task.is_external && external_pending_count > 0) {
                        --external_pending_count;
                        slot_cv.notify_one();
                    }
                } else if (my_local_deque.pop_back(task)) {
                    ++active_task_count;
                    found_task = true;
                    ++consecutive_local_count;
                } else {
                    // 唤醒后尝试窃取一轮
                    lock.unlock();
                    detail::generate_steal_victims(worker_index, options.worker_count, options.steal_probe_limit, rng_state, victims);
                    for (std::size_t v : victims) {
                        if (v < local_deques.size() && local_deques[v]->steal_front(task)) {
                            found_task = true;
                            consecutive_local_count = 0;
                            std::lock_guard<std::mutex> lk(lifecycle_mutex);
                            ++active_task_count;
                            break;
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
                        for (const auto& entry : global_injection_queue) {
                            if (entry.invoker && entry.invoker->is_resume_segment()) {
                                any_resumes = true;
                                break;
                            }
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
                        bool any_tasks = !global_injection_queue.empty() || !my_local_deque.empty();
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
    if (decision == detail::AdmissionDecision::Stopping) {
        throw submission_rejected(SubmissionError::Stopping);
    }
    if (decision == detail::AdmissionDecision::Stopped) {
        throw submission_rejected(SubmissionError::Stopped);
    }
    if (decision == detail::AdmissionDecision::CapacityExhausted) {
        throw submission_rejected(SubmissionError::CapacityExhausted);
    }

    const GraphRunId gid = impl_->allocate_graph_run_id();

    // R-070: 空图直接完成
    if (n == 0) {
        auto state = std::make_shared<detail::GraphRunSharedState>(gid, 0);
        state->run_state.store(GraphRunState::Succeeded, std::memory_order_release);
        return GraphRun(std::move(state));
    }

    std::shared_ptr<detail::GraphRunSharedState> state;
    try {
        state = std::make_shared<detail::GraphRunSharedState>(gid, n);
    } catch (...) {
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
        if (node_data.options.has_value()) {
            validate_priority(node_data.options->priority);
            node_priority = node_data.options->priority;
        }
        state->node_entries[idx].id = node_data.id;
        state->node_entries[idx].invoker = std::move(node_data.invoker);
        state->node_entries[idx].priority = node_priority;
        if (state->node_entries[idx].invoker && state->node_entries[idx].invoker->is_coroutine_node()) {
            auto* coro_node = static_cast<detail::GraphCoroutineNodeInvoker*>(state->node_entries[idx].invoker.get());
            const TaskId task_id = detail::allocate_task_id(impl_->runtime_id);
            auto task_state = std::make_shared<detail::TaskSharedState<void>>(task_id, node_priority);
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
                detail::make_graph_node_invoker<true>(std::move(task_fn)),
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

            try {
                if (node_entry.invoker) {
                    node_entry.invoker->execute();
                }
                state->mark_node_terminal(n_idx, TaskState::Succeeded);
            } catch (const astra::task_cancelled&) {
                state->mark_node_terminal(n_idx, TaskState::Cancelled);
            } catch (...) {
                state->mark_node_terminal(n_idx, TaskState::Failed, std::current_exception());
            }

            trigger_successors(trigger_successors, self, u_id);
        };

        impl_ptr->post_task_internal(
            detail::make_graph_node_invoker<true>(std::move(task_fn)),
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

}  // namespace astra

// ============================================================================
// Scheduler 运行时核心实现。
//
// 【线程模型】
//   每个运行时包含 N 个 worker 线程（本文件）+ 全进程唯一的 Reaper 协调线程
//   （reaper_registry.cpp）。main 线程通过 submit/spawn 把任务投进队列后即返回。
//
// 【任务的三条流转路径】
//   1. 外部提交：submit() 先过准入（容量满时按策略拒绝或阻塞），进入
//      global_injection_queues（按优先级分 4 条带）；worker 从中领取。
//   2. worker 本地队列：worker 执行任务时产出的子任务进自己的本地队列
//      （LIFO，缓存友好）；其他 worker 偶尔来偷（steal），保证负载均衡。
//   3. 协程恢复：协程在挂起点让出后，恢复动作被包成 invoker 重新入队，
//      由任意 worker 继续。
//
// 【worker 的一轮循环】
//   取任务（本地 -> 全局 -> 偷）-> 执行 -> 计数与唤醒；队列全空则按
//   退避策略小睡，直到有新任务或关停。取任务失败的次数多了会主动
//   让出全局领取机会，防止饥饿。
//
// 【等待 = helping（本调度器最重要的设计）】
//   worker 等待某个任务的结果时，不空等：它把自己当成临时执行者，
//   从队列里找其他任务执行（受 helping 深度限制，防止无限递归）。
//   这既避免死锁（A 等 B、B 在队列里等 worker），又提高吞吐。
//   外部线程（main）等待则走普通的条件变量阻塞。
//
// 【关停的两级模式】
//   Graceful：不再收新任务，已有任务全部跑完；Immediate：未开始的任务
//   直接取消，已开始的靠取消点尽快退出。Escalation 单向：Immediate 之后的
//   Graceful 请求被忽略（更严的模式优先）。
//
// 【最后一个句柄销毁 -> Reaper 接管】
//   worker 线程销毁最后的运行时句柄时，把运行时的所有权（shared_ptr）
//   移交 Reaper 注册表，由 Reaper 线程完成 worker join 与清理——保证
//   析构永远立即返回（这就是"孤儿运行时"回收协议）。
//
// 【指标埋点】
//   record_* 系列函数把计数/延迟写进每 worker 一份的分片（shard），
//   避免多线程争用同一缓存行；MetricsLevel::Off 时全部为空操作。
// ============================================================================
#include <astra/coroutine.hpp>
#include <astra/scheduler.hpp>
#include <astra/trace.hpp>
#include "graph/graph_execution.hpp"
#include "graph/graph_runtime_port.hpp"
#include "graph/graph_shared_state.hpp"
#include "lifecycle/reaper_registry.hpp"
#include "observability/trace_collector.hpp"
#include "runtime/admission_controller.hpp"
#include "runtime/ready_queues.hpp"
#include "runtime/runtime_identity.hpp"
#include "runtime/runtime_metrics.hpp"
#include "runtime/runtime_registry.hpp"
#include "runtime/runtime_state.hpp"
#include "runtime/timer_queue.hpp"
#include "runtime/worker_loop.hpp"
#include "scheduling/chase_lev_deque.hpp"
#include "task/await_handshake.hpp"
#include "task/task_control_block.hpp"
#include "testing/test_seam.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
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
detail::RuntimeDiagnostics* find_runtime_diagnostics(RuntimeId id);
}  // namespace

namespace detail {
extern thread_local RuntimeId t_current_worker_runtime_id;
extern thread_local void* t_current_worker_impl;
extern thread_local std::size_t t_current_worker_index;
extern thread_local TaskId t_current_executing_task_id;
extern thread_local std::size_t t_current_helping_depth;
extern thread_local std::unique_ptr<TaskInvokerBase> t_deferred_self_resume;
extern thread_local void* t_deferred_self_resume_impl;
extern thread_local TaskId t_deferred_self_resume_owner;
void flush_deferred_self_resume();
}  // namespace detail

struct ASTRA_NO_EXPORT Scheduler::Impl : public detail::RuntimeState,
                                         public std::enable_shared_from_this<Scheduler::Impl>,
                                         public detail::GraphRuntimePort {
    Impl(RuntimeId id, SchedulerOptions opts, SchedulerCapabilities caps)
        : detail::RuntimeState(id, std::move(opts), caps) {
        register_runtime_impl(this);
        
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
                    this->admission.wake_all();
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

        // 2. Trace collector 附加（R-086 / D-158）：在 Worker 启动 barrier 前完成
        //    全部 producer buffer 预分配；Recording 中分配失败抛出 → startup rollback，
        //    不允许无 buffer 的部分 Runtime。
        if (options.trace_collector) {
            diagnostics.attach(options.trace_collector, options.worker_count);
        }

        // 3. 创建 Worker 并通过启动栅栏进行同步强事务管理（R-097, D-155）
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
        const bool on_own_worker = (detail::t_current_worker_impl == this);
        if (get_status().state != SchedulerState::Stopped && !on_own_worker) {
            try {
                shutdown_sync(ShutdownMode::Graceful);
            } catch (...) {
            }
        }
        if (!on_own_worker) {
            for (auto& t : worker_threads) {
                if (t.joinable()) {
                    t.join();
                }
            }
            worker_threads.clear();
        }
    }

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
            admission.wake_all();
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
        ready_queues.cancel_unstarted(admission);
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
                    admission.wake_all();
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
                        admission.wake_all();
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
            handoff_dispatched.store(true, std::memory_order_release);
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
            admission.wake_all();

            s_lock.lock();
            shutdown_in_progress = false;
            shutdown_done_cv.notify_all();
        } else {
            shutdown_done_cv.wait(s_lock, [this] {
                return get_status().state == SchedulerState::Stopped;
            });
        }
    }

    TaskId allocate_task_id() {
        return identities.allocate_task(runtime_id);
    }

    RuntimeId runtime_identity() const noexcept override {
        return runtime_id;
    }

    TaskId allocate_graph_task_id() override {
        return allocate_task_id();
    }

    GraphRunId allocate_graph_run_id() override {
        return identities.allocate_graph_run(runtime_id);
    }

    detail::AdmissionDecision acquire_admission_slot(bool block, bool is_internal) {
        return admission.acquire(1, block, is_internal);
    }

    detail::AdmissionDecision acquire_admission_slots(std::size_t count, bool block, bool is_internal) {
        return admission.acquire(count, block, is_internal);
    }

    detail::AdmissionDecision acquire_graph_slots(
        std::size_t count,
        bool block,
        bool is_internal) override {
        return acquire_admission_slots(count, block, is_internal);
    }

    void release_external_slot() {
        admission.release(1);
    }

    void release_external_slots(std::size_t count) {
        admission.release(count);
    }

    void release_graph_slots(std::size_t count) noexcept override {
        release_external_slots(count);
    }

    std::uint64_t register_timer(std::chrono::steady_clock::time_point wake_time,
                                 std::shared_ptr<detail::AwaitHandshake> handshake,
                                 std::function<void()> resume_action) {
        bool became_earliest = false;
        const std::uint64_t tid = timers.add(
            wake_time, std::move(handshake), std::move(resume_action), became_earliest);
        if (became_earliest) {
            work_epoch.fetch_add(1, std::memory_order_release);
            work_cv.notify_one();
        }
        return tid;
    }

    void cancel_timer(std::uint64_t tid) {
        timers.cancel(tid);
    }

    std::uint64_t register_graph_timer(
        std::chrono::steady_clock::time_point wake_time,
        std::shared_ptr<detail::AwaitHandshake> handshake,
        std::function<void()> resume_action) override {
        return register_timer(wake_time, std::move(handshake), std::move(resume_action));
    }

    void cancel_graph_timer(std::uint64_t tid) noexcept override {
        cancel_timer(tid);
    }

    void process_due_timers() {
        auto due_items = timers.collect_due(std::chrono::steady_clock::now());
        for (auto& item : due_items) {
            if (item.handshake && item.resume_action) {
                item.handshake->trigger(item.resume_action);
            }
        }
    }

    void cancel_all_timers() {
        auto all_items = timers.cancel_all();
        for (auto& item : all_items) {
            if (item.handshake && item.resume_action) {
                item.handshake->trigger_cancel(item.resume_action);
            }
        }
    }

    std::optional<std::chrono::steady_clock::time_point> earliest_wake_time() {
        return timers.earliest_wake_time();
    }

    bool has_timers() {
        return !timers.empty();
    }

    void post_task_internal(std::unique_ptr<detail::TaskInvokerBase> task, bool is_external) {
        if (!task) return;
        // R-074: await_suspend 内发布的 self-resume 必须等当前 segment 返回后再入队，
        // 否则另一 Worker 可能在协程仍位于 await_suspend 时 resume 同一帧。
        if (task->is_resume_segment() && detail::t_current_executing_task_id != TaskId{}) {
            detail::t_deferred_self_resume = std::move(task);
            detail::t_deferred_self_resume_impl = static_cast<void*>(this);
            detail::t_deferred_self_resume_owner = detail::t_current_executing_task_id;
            return;
        }
        const bool use_local_queue =
            !is_external && detail::t_current_worker_impl == this &&
            detail::t_current_worker_runtime_id == runtime_id &&
            detail::t_current_worker_index < ready_queues.worker_count();
        ready_queues.publish(
            std::move(task), is_external, use_local_queue, detail::t_current_worker_index);
        work_epoch.fetch_add(1, std::memory_order_release);
        work_cv.notify_one();
    }

    void post_graph_task(
        std::unique_ptr<detail::TaskInvokerBase> task,
        bool is_external) override {
        post_task_internal(std::move(task), is_external);
    }

    void record_graph_admission_attempt() noexcept override {
        if (metrics.level != MetricsLevel::Off) {
            detail::RuntimeMetrics::saturating_inc(
                metrics.shard_for_current().graph_admission_attempts);
        }
    }

    void record_graph_rejected() noexcept override {
        if (metrics.level != MetricsLevel::Off) {
            detail::RuntimeMetrics::saturating_inc(
                metrics.shard_for_current().graph_runs_rejected);
        }
    }

    void record_graph_started() noexcept override {
        if (metrics.level != MetricsLevel::Off) {
            detail::RuntimeMetrics::saturating_inc(
                metrics.shard_for_current().graph_runs_accepted);
            metrics.active_graph_runs.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void rollback_graph_started(std::size_t task_count) noexcept override {
        if (metrics.level == MetricsLevel::Off) {
            return;
        }
        auto ready = metrics.ready_tasks.load(std::memory_order_relaxed);
        while (ready > 0) {
            const auto decrement = std::min(ready, task_count);
            if (metrics.ready_tasks.compare_exchange_weak(
                    ready, ready - decrement, std::memory_order_relaxed)) {
                break;
            }
        }
        if (metrics.active_graph_runs.load(std::memory_order_relaxed) > 0) {
            metrics.active_graph_runs.fetch_sub(1, std::memory_order_relaxed);
        }
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
        try {
            if (arg != nullptr) {
                auto* impl = static_cast<Impl*>(arg);
                detail::run_worker_loop(*impl, impl, index);
            }
        } catch (...) {
        }
    }
};

namespace {
void register_runtime_impl(Scheduler::Impl* impl) {
    detail::register_runtime_instance(impl->runtime_id, &impl->diagnostics);
}

void unregister_runtime_impl(Scheduler::Impl* impl) {
    detail::unregister_runtime_instance(impl->runtime_id);
}

detail::RuntimeDiagnostics* find_runtime_diagnostics(RuntimeId id) {
    return detail::find_runtime_instance(id);
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

thread_local std::unique_ptr<TaskInvokerBase> t_deferred_self_resume;
thread_local void* t_deferred_self_resume_impl{nullptr};
thread_local TaskId t_deferred_self_resume_owner{};

void flush_deferred_self_resume() {
    if (!t_deferred_self_resume || t_deferred_self_resume_owner == t_current_executing_task_id) {
        return;
    }
    auto* impl = static_cast<Scheduler::Impl*>(t_deferred_self_resume_impl);
    auto inv = std::move(t_deferred_self_resume);
    t_deferred_self_resume_impl = nullptr;
    t_deferred_self_resume_owner = {};
    if (impl) {
        impl->post_task_internal(std::move(inv), false);
    }
}

TaskExecutionContextGuard::TaskExecutionContextGuard(TaskId new_id, Priority new_priority) noexcept
    : prev_id(t_current_executing_task_id), prev_priority(t_current_executing_task_priority) {
    t_current_executing_task_id = new_id;
    t_current_executing_task_priority = new_priority;
}

TaskExecutionContextGuard::~TaskExecutionContextGuard() noexcept {
    t_current_executing_task_id = prev_id;
    t_current_executing_task_priority = prev_priority;
    flush_deferred_self_resume();
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

// ---------------------------------------------------------------------------
// 等待一个任务完成（TaskHandle::wait/wait_for/get 的公共底层）。
//
// 【调用方分两种，处理完全不同】
//   - 外部线程（main）：没有任务可帮忙，直接在条件变量上阻塞到完成或超时。
//   - worker 线程：空等会浪费线程还可能死锁（A 等 B、B 排在队列里），
//     所以进入下方的 helping 循环：把队列里其他任务拿来执行，直到目标
//     完成。深度限制防止"等中等的等"无限递归。
//
// 【直接自等检查】任务等自己 = 永远不会完成，是调用方的写法错误，
//   必须立即报错（且要在任何计数/状态副作用之前）。
// ---------------------------------------------------------------------------
void perform_caller_wait(
    const TaskControlBlock& target,
    std::optional<std::chrono::steady_clock::time_point> deadline) {
    // 1. Direct Self-Wait 必须在副作用前抛 std::logic_error（R-052 / D-049 / D-065）
    if (t_current_worker_impl != nullptr &&
        t_current_executing_task_id != TaskId{} &&
        t_current_executing_task_id == target.id()) {
        // R-096 / D-149：self rejection 计数（不改变语义）
        if (auto* impl = find_runtime_diagnostics(target.id().runtime_id());
            impl && impl->metrics.level != MetricsLevel::Off) {
            detail::RuntimeMetrics::saturating_inc(
                impl->metrics.shard_for_current().direct_self_wait_rejections);
        }
        throw std::logic_error("direct self-wait detected on TaskHandle");
    }

    // --- Wait/Await 诊断（R-096 / D-149）：入口计数 + WaitBegin，scope-exit 配对 ---
    RuntimeDiagnostics* diag_impl = nullptr;
    bool diag_helping = false;
    if (t_current_worker_impl != nullptr) {
        diag_impl = current_worker_diagnostics();
        diag_helping = true;
    } else {
        diag_impl = find_runtime_diagnostics(target.id().runtime_id());
    }
    if (diag_impl && diag_impl->metrics.level != MetricsLevel::Off) {
        auto& shard = diag_impl->metrics.shard_for_current();
        if (diag_helping) {
            if (target.id().runtime_id() == diag_impl->runtime_id) {
                detail::RuntimeMetrics::saturating_inc(shard.same_runtime_helping_waits);
            } else {
                detail::RuntimeMetrics::saturating_inc(shard.cross_runtime_helping_waits);
            }
        } else {
            detail::RuntimeMetrics::saturating_inc(shard.task_wait_calls);
        }
    }
    const auto diag_begin = std::chrono::steady_clock::now();
    emit_wait_trace_event(diag_impl, TraceEventKind::WaitBegin,
                          t_current_executing_task_id, target.id(), GraphRunId{}, 0);
    WaitDiagnosticsGuard diag_guard{
        diag_impl, t_current_executing_task_id, target.id(), GraphRunId{},
        diag_begin, deadline, &target, nullptr, diag_helping};

    // 2. 已完成即时返回（即时等待也计 call 与最小 bucket，D-149）
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
        // R-096 / D-149：depth rejection 计数（不改变语义）
        if (impl->metrics.level != MetricsLevel::Off) {
            detail::RuntimeMetrics::saturating_inc(
                impl->metrics.shard_for_current().helping_depth_rejections);
        }
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

        ReadyQueues::QueuedTask task;
        bool found_task = false;

        if (impl->ready_queues.claim_local(
                t_current_worker_index, t_help_local_cal, task)) {
            found_task = true;
        }

        if (!found_task) {
            std::unique_lock<std::mutex> lock(impl->lifecycle_mutex);
            const auto st = Scheduler::Impl::unpack(impl->packed_status.load(std::memory_order_acquire));
            // R-059 / D-080: Immediate 模式下不得 first-start 新 Task
            if (st.state != SchedulerState::Stopped &&
                st.shutdown_mode != ShutdownMode::Immediate &&
                impl->ready_queues.claim_global(
                    t_help_global_cal, t_help_deadline_bursts, task)) {
                found_task = true;
                if (task.is_external) {
                    impl->admission.release(1);
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
                    detail::RuntimeMetrics::saturating_inc(impl->metrics.shard_for_current().steal_attempts);
                }
                if (impl->ready_queues.steal(v, t_help_steal_cal, task)) {
                    if (impl->metrics.level != MetricsLevel::Off) {
                        detail::RuntimeMetrics::saturating_inc(impl->metrics.shard_for_current().steal_successes);
                    }
                    found_task = true;
                    break;
                } else {
                    if (impl->metrics.level != MetricsLevel::Off) {
                        detail::RuntimeMetrics::saturating_inc(impl->metrics.shard_for_current().steal_failures);
                    }
                }
            }
        }

        if (found_task && task.invoker) {
            task.invoker->execute();
            impl->ready_queues.complete_claim();
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

    // --- Wait 诊断（R-096 / D-149）：graph_wait_calls + WaitBegin/End 配对 ---
    RuntimeDiagnostics* diag_impl = nullptr;
    bool diag_helping = false;
    if (t_current_worker_impl != nullptr) {
        diag_impl = current_worker_diagnostics();
        diag_helping = true;
    } else {
        diag_impl = find_runtime_diagnostics(target.id.runtime_id());
    }
    if (diag_impl && diag_impl->metrics.level != MetricsLevel::Off) {
        auto& shard = diag_impl->metrics.shard_for_current();
        if (diag_helping) {
            if (target.id.runtime_id() == diag_impl->runtime_id) {
                detail::RuntimeMetrics::saturating_inc(shard.same_runtime_helping_waits);
            } else {
                detail::RuntimeMetrics::saturating_inc(shard.cross_runtime_helping_waits);
            }
        } else {
            detail::RuntimeMetrics::saturating_inc(shard.graph_wait_calls);
        }
    }
    const auto diag_begin = std::chrono::steady_clock::now();
    emit_wait_trace_event(diag_impl, TraceEventKind::WaitBegin,
                          t_current_executing_task_id, TaskId{}, target.id, 0);
    WaitDiagnosticsGuard diag_guard{
        diag_impl, t_current_executing_task_id, TaskId{}, target.id,
        diag_begin, deadline, nullptr, &target, diag_helping};

    // 2. 已完成即时返回（即时等待也计 call 与最小 bucket，D-149）
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

        ReadyQueues::QueuedTask task;
        bool found_task = false;

        if (impl->ready_queues.claim_local(
                t_current_worker_index, t_graph_help_local_cal, task)) {
            found_task = true;
        }

        if (!found_task) {
            std::unique_lock<std::mutex> lock(impl->lifecycle_mutex);
            const auto st = Scheduler::Impl::unpack(impl->packed_status.load(std::memory_order_acquire));
            if (st.state != SchedulerState::Stopped &&
                st.shutdown_mode != ShutdownMode::Immediate &&
                impl->ready_queues.claim_global(
                    t_graph_help_global_cal, t_graph_help_deadline_bursts, task)) {
                found_task = true;
                if (task.is_external) {
                    impl->admission.release(1);
                }
            }
        }

        if (!found_task) {
            std::vector<std::size_t> victims;
            static thread_local std::uint64_t s_help_rng = 0x854329415849ULL;
            generate_steal_victims(t_current_worker_index, impl->options.worker_count, impl->options.steal_probe_limit, s_help_rng, victims);
            for (std::size_t v : victims) {
                if (impl->metrics.level != MetricsLevel::Off) {
                    detail::RuntimeMetrics::saturating_inc(impl->metrics.shard_for_current().steal_attempts);
                }
                if (impl->ready_queues.steal(v, t_graph_help_steal_cal, task)) {
                    if (impl->metrics.level != MetricsLevel::Off) {
                        detail::RuntimeMetrics::saturating_inc(impl->metrics.shard_for_current().steal_successes);
                    }
                    found_task = true;
                    break;
                } else {
                    if (impl->metrics.level != MetricsLevel::Off) {
                        detail::RuntimeMetrics::saturating_inc(impl->metrics.shard_for_current().steal_failures);
                    }
                }
            }
        }

        if (found_task && task.invoker) {
            task.invoker->execute();
            impl->ready_queues.complete_claim();
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

void SchedulerTestAccess::run_task_on_worker(Scheduler& s, std::function<void()> task) {
    // 测试会让该任务在 worker 上销毁最后一个 Scheduler handle；调用期间持有
    // 临时强引用，确保 publish/notify 返回前 Runtime 不会被 Reaper 回收。
    auto runtime = s.impl_;
    if (runtime) {
        runtime->post_task_internal(
            std::make_unique<FunctionTaskInvoker>(std::move(task)), false /* not external */);
    }
}

std::size_t SchedulerTestAccess::global_queue_size(const Scheduler& s) {
    if (s.impl_) {
        return s.impl_->ready_queues.global_size();
    }
    return 0;
}

std::size_t SchedulerTestAccess::external_pending_count(const Scheduler& s) {
    if (s.impl_) {
        return s.impl_->admission.pending();
    }
    return 0;
}

std::size_t SchedulerTestAccess::parked_workers_count(const Scheduler& s) {
    if (s.impl_) {
        return s.impl_->parked_workers.load(std::memory_order_acquire);
    }
    return 0;
}

std::uint64_t SchedulerTestAccess::current_work_epoch(const Scheduler& s) {
    if (s.impl_) {
        return s.impl_->work_epoch.load(std::memory_order_acquire);
    }
    return 0;
}

}  // namespace detail


detail::AdmissionDecision Scheduler::acquire_admission(bool block, bool is_internal) const {
    if (!impl_) {
        throw std::logic_error("operating on empty/moved-from Scheduler");
    }
    return impl_->acquire_admission_slot(block, is_internal);
}

TaskId Scheduler::allocate_task_id() const {
    if (!impl_) {
        throw std::logic_error("operating on empty/moved-from Scheduler");
    }
    return impl_->allocate_task_id();
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

void detail::post_task_on_impl(void* impl, std::unique_ptr<TaskInvokerBase> invoker, bool is_external) {
    if (impl && invoker) {
        static_cast<Scheduler::Impl*>(impl)->post_task_internal(std::move(invoker), is_external);
    }
}

std::uint64_t detail::register_timer_on_impl(
    void* impl,
    std::chrono::steady_clock::time_point wake_time,
    std::shared_ptr<AwaitHandshake> handshake,
    std::function<void()> resume_action) {
    if (!impl) {
        return 0;
    }
    return static_cast<Scheduler::Impl*>(impl)->register_timer(
        wake_time, std::move(handshake), std::move(resume_action));
}

void detail::cancel_timer_on_impl(void* impl, std::uint64_t timer_id) {
    if (impl) {
        static_cast<Scheduler::Impl*>(impl)->cancel_timer(timer_id);
    }
}

std::uint64_t Scheduler::register_timer(std::chrono::steady_clock::time_point wake_time,
                                        std::shared_ptr<detail::AwaitHandshake> handshake,
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
    // R-101 / D-162：能力快照必须描述 ReadyQueues 当前实际使用的后端。
    // Chase-Lev 仍由独立测试覆盖，但尚未接入四优先级 + EDF 的生产队列。
    const auto backend = LocalDequeBackend::Locked;
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
    if (!impl_) {
        throw std::logic_error("operating on empty/moved-from Scheduler");
    }
    return detail::GraphExecution::run(*impl_, options, std::move(graph));
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
    impl_->metrics.fill_counters_and_histograms(snapshot, any_saturated);
    impl_->metrics.fill_task_gauges(snapshot);
    snapshot.gauges.external_pending_slots_used = static_cast<std::uint64_t>(impl_->admission.pending());
    snapshot.gauges.parked_workers = static_cast<std::uint64_t>(impl_->parked_workers.load(std::memory_order_relaxed));
    snapshot.gauges.active_timer_entries = static_cast<std::uint64_t>(impl_->timers.size());

    snapshot.saturated = any_saturated;
    snapshot.capture_finished_at = std::chrono::steady_clock::now();
    return snapshot;
}

}  // namespace astra

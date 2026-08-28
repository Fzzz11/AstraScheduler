#include <astra/scheduler.hpp>
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
        while (!global_injection_queue.empty()) {
            auto task = std::move(global_injection_queue.front());
            global_injection_queue.pop_front();
            if (task.invoker) {
                task.invoker->cancel_pre_start();
            }
            if (task.is_external && external_pending_count > 0) {
                --external_pending_count;
                slot_cv.notify_one();
            }
        }
        for (auto& d : local_deques) {
            if (d) {
                std::lock_guard<std::mutex> lk(d->mutex);
                while (!d->tasks.empty()) {
                    auto task = std::move(d->tasks.front());
                    d->tasks.pop_front();
                    if (task.invoker) {
                        task.invoker->cancel_pre_start();
                    }
                }
            }
        }
    }

    // 状态转换与模式保持（R-014 / R-015 / R-022）
    void request_shutdown_mode(ShutdownMode requested_mode) noexcept {
        uint16_t current = packed_status.load(std::memory_order_acquire);
        while (true) {
            auto st = unpack(current);
            if (st.state == SchedulerState::Running) {
                uint16_t next = pack(SchedulerState::Stopping, requested_mode);
                if (packed_status.compare_exchange_weak(current, next, std::memory_order_acq_rel)) {
                    if (requested_mode == ShutdownMode::Immediate) {
                        std::lock_guard<std::mutex> lock(lifecycle_mutex);
                        cancel_all_unstarted_tasks_locked();
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

    detail::AdmissionDecision acquire_admission_slot(bool block, bool is_internal) {
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

            if (external_pending_count < options.external_pending_capacity) {
                ++external_pending_count;
                return detail::AdmissionDecision::Success;
            }

            if (!block || options.external_backpressure != ExternalBackpressure::Block) {
                return detail::AdmissionDecision::CapacityExhausted;
            }

            // Ordinary thread waiting on slot_cv (R-061 / D-086)
            slot_cv.wait(lock, [this] {
                const auto current_st = unpack(packed_status.load(std::memory_order_acquire));
                return current_st.state != SchedulerState::Running ||
                       external_pending_count < options.external_pending_capacity;
            });
        }
    }

    void release_external_slot() {
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex);
            if (external_pending_count > 0) {
                --external_pending_count;
            }
        }
        slot_cv.notify_one();
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
thread_local std::size_t t_current_helping_depth{0};

TaskExecutionContextGuard::TaskExecutionContextGuard(TaskId new_id) noexcept
    : prev_id(t_current_executing_task_id) {
    t_current_executing_task_id = new_id;
}

TaskExecutionContextGuard::~TaskExecutionContextGuard() noexcept {
    t_current_executing_task_id = prev_id;
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
    constexpr std::size_t kMaxConsecutiveLocalTasks = 64;

    // 运行期工作循环（执行内部/测试任务，直至收到 stop_requested 且 Drain Closure 排空）
    while (true) {
        QueuedTask task;
        bool found_task = false;

        // 1. 优先尝试从本 Worker 的 Local Deque (LIFO) 获取任务（受防饥饿上限限制）
        if (consecutive_local_count < kMaxConsecutiveLocalTasks) {
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
            } else if (consecutive_local_count >= kMaxConsecutiveLocalTasks) {
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

        // 3. 若仍无任务，进入等待或退出判断
        if (!found_task) {
            std::unique_lock<std::mutex> lock(lifecycle_mutex);
            work_cv.wait(lock, [this, &my_local_deque] {
                if (stop_requested) {
                    const auto st = unpack(packed_status.load(std::memory_order_acquire));
                    if (st.state == SchedulerState::Stopped || st.shutdown_mode == ShutdownMode::Immediate) {
                        return true;
                    }
                    if (!global_injection_queue.empty() || !my_local_deque.empty() || active_task_count == 0) {
                        return true;
                    }
                } else {
                    if (!global_injection_queue.empty() || !my_local_deque.empty()) {
                        return true;
                    }
                }
                return false;
            });

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
            } else if (stop_requested) {
                const auto st = unpack(packed_status.load(std::memory_order_acquire));
                if (st.state == SchedulerState::Stopped || st.shutdown_mode == ShutdownMode::Immediate) {
                    break;
                }
                if (global_injection_queue.empty() && my_local_deque.empty() && active_task_count == 0) {
                    work_cv.notify_all();
                    break;
                }
            }
        }

        // 4. 执行获取到的任务
        if (found_task && task.invoker) {
            task.invoker->execute();
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

Scheduler::Scheduler(SchedulerOptions options) {
    validate_options(options);
    const RuntimeId id = allocate_runtime_id();
    const SchedulerCapabilities caps{LocalDequeBackend::Locked};
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

}  // namespace astra

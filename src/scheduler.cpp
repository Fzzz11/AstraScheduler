#include <astra/scheduler.hpp>
#include "reaper_registry.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
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

thread_local RuntimeId t_current_worker_runtime_id{0};

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
    bool startup_done{false};
    bool startup_failed{false};
    bool stop_requested{false};
    bool handoff_dispatched{false};
    std::size_t workers_ready{0};
    std::vector<std::thread> worker_threads;
    std::vector<std::function<void()>> worker_tasks;

    Impl(RuntimeId id, SchedulerOptions opts, SchedulerCapabilities caps)
        : runtime_id(id),
          options(std::move(opts)),
          capabilities(caps),
          packed_status(pack(SchedulerState::Running, ShutdownMode::None)) {
        
        // 1. Reaper 注册与能力预留（R-023, R-024, R-097）
        auto& registry = detail::ReaperRegistry::instance();
        if (!registry.is_registration_open()) {
            throw scheduler_creation_rejected(SchedulerCreationError::FinalizationStarted);
        }
        if (!registry.register_runtime(runtime_id)) {
            if (registry.should_fail_reservation()) {
                throw std::bad_alloc();
            }
            throw scheduler_creation_rejected(SchedulerCreationError::FinalizationStarted);
        }

        // 2. 创建 Worker 并通过启动栅栏进行同步强事务管理（R-097, D-155）
        const std::size_t count = options.worker_count;
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

            for (auto& t : worker_threads) {
                if (t.joinable()) {
                    t.join();
                }
            }
            worker_threads.clear();
            registry.unregister_runtime(runtime_id);
            throw;
        }
    }

    ~Impl() {
        // 非 Worker 正常析构（若尚未经过 Worker handoff 移交）
        if (!handoff_dispatched) {
            request_shutdown_mode(ShutdownMode::Graceful);
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex);
                stop_requested = true;
            }
            work_cv.notify_all();
            for (auto& t : worker_threads) {
                if (t.joinable()) {
                    t.join();
                }
            }
            worker_threads.clear();
            const auto current_mode = get_status().shutdown_mode;
            packed_status.store(pack(SchedulerState::Stopped, current_mode), std::memory_order_release);
            detail::ReaperRegistry::instance().unregister_runtime(runtime_id);
        }
    }

    // 状态转换与模式保持（R-022）
    void request_shutdown_mode(ShutdownMode requested_mode) noexcept {
        uint16_t current = packed_status.load(std::memory_order_acquire);
        while (true) {
            auto st = unpack(current);
            if (st.state == SchedulerState::Running) {
                uint16_t next = pack(SchedulerState::Stopping, requested_mode);
                if (packed_status.compare_exchange_weak(current, next, std::memory_order_acq_rel)) {
                    break;
                }
            } else {
                // 已处于 Stopping 或 Stopped，保持现有模式（R-022）
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
        for (auto& t : worker_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        worker_threads.clear();
        const auto current_mode = get_status().shutdown_mode;
        packed_status.store(pack(SchedulerState::Stopped, current_mode), std::memory_order_release);
    }

    void worker_main(std::size_t /*worker_index*/) {
        t_current_worker_runtime_id = runtime_id;

        // 等待 startup 栅栏完成或中止
        {
            std::unique_lock<std::mutex> lock(lifecycle_mutex);
            ++workers_ready;
            startup_cv.notify_all();
            startup_cv.wait(lock, [this] {
                return startup_done || startup_failed || stop_requested;
            });
            if (startup_failed || stop_requested) {
                t_current_worker_runtime_id = RuntimeId{0};
                return;
            }
        }

        // 运行期工作循环（执行内部/测试任务，直至收到 stop_requested）
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(lifecycle_mutex);
                work_cv.wait(lock, [this] {
                    return stop_requested || !worker_tasks.empty();
                });
                if (!worker_tasks.empty()) {
                    task = std::move(worker_tasks.back());
                    worker_tasks.pop_back();
                } else if (stop_requested) {
                    break;
                }
            }
            if (task) {
                task();
            }
        }

        t_current_worker_runtime_id = RuntimeId{0};
    }

    void post_task_internal(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex);
            worker_tasks.push_back(std::move(task));
        }
        work_cv.notify_one();
    }

    static void post_test_task(Scheduler& s, std::function<void()> task) {
        if (s.impl_) {
            s.impl_->post_task_internal(std::move(task));
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
        if (arg != nullptr) {
            static_cast<Impl*>(arg)->worker_main(index);
        }
    }
};

namespace detail {
void run_test_task_on_worker(Scheduler& s, std::function<void()> task) {
    if (s.impl_) {
        s.impl_->post_task_internal(std::move(task));
    }
}
}  // namespace detail

Scheduler::Scheduler(SchedulerOptions options) {
    validate_options(options);
    const RuntimeId id = allocate_runtime_id();
    const SchedulerCapabilities caps{LocalDequeBackend::None};
    impl_ = std::make_shared<Impl>(id, std::move(options), caps);
}

Scheduler::~Scheduler() {
    if (impl_) {
        // R-021: 检查是否在属于该 Runtime 的 Worker 线程上销毁最后一个 Handle
        if (impl_.use_count() == 1 && t_current_worker_runtime_id == impl_->runtime_id) {
            impl_->execute_worker_orphan_handoff(impl_);
            impl_.reset();
            return;
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

}  // namespace astra

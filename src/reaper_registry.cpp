// ============================================================================
// Reaper：全进程唯一的"运行时回收者"线程。
//
// 【它解决什么问题】
//   Scheduler 句柄可能在任意线程销毁——包括 worker 自己。运行时的收尾
//   （join 所有 worker 线程、清理状态）不能在 worker 里做（自己 join 自己
//   是死锁），也不能在 main 里做（main 可能早已退出）。所以运行时的
//   "身后事"统一移交给这个永不退出的协调线程。
//
// 【一个运行时的生命周期（在本文件里的状态）】
//   注册（Scheduler 启动时）-> 正常运行 -> 最后一个句柄销毁，worker 把
//   所有权移交（handoff，runtime 进入"孤儿"状态，由 Reaper 持有）->
//   worker 退出后进入 join-ready -> Reaper 认领并执行 cleanup（join 全部
//   worker）-> 完毕。
//
// 【begin_finalization 与永久关闭】
//   进程级"最终收尾"会把注册表永久关闭：此后新 Scheduler 一律拒绝创建。
//   已注册的运行时按上面的流程逐个回收，全部结束后进入 Finalized——
//   这是不可逆的，防止关闭后又冒出新的后台线程。
//
// 【测试注意】
//   reset_for_testing() 是测试专用重置（JOIN 掉 coordinator 并清空全部
//   状态）；生产代码禁止调用。
// ============================================================================
#include "reaper_registry.hpp"
#include <astra/scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace astra::detail {

ReaperRegistry& ReaperRegistry::instance() noexcept {
    static ReaperRegistry registry;
    return registry;
}

ReaperRegistry::~ReaperRegistry() {
    reset_for_testing();
}

void ReaperRegistry::ensure_coordinator_started_locked() {
    if (!coordinator_thread_) {
        coordinator_stop_ = false;
        coordinator_exited_ = false;
        coordinator_thread_ = std::make_unique<std::thread>(&ReaperRegistry::coordinator_loop, this);
    }
}

bool ReaperRegistry::register_runtime(
    RuntimeId id,
    std::function<void()> req_graceful,
    std::function<void()> req_immediate,
    std::function<void()> cleanup_fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RegistrationState::Open) {
        return false;
    }
    if (inject_reservation_fail_) {
        return false;
    }
    ensure_coordinator_started_locked();
    auto slot = std::make_unique<HandoffCapabilitySlot>();
    slot->runtime_id = id;
    slot->request_graceful_fn = std::move(req_graceful);
    slot->request_immediate_fn = std::move(req_immediate);
    slot->cleanup_fn = std::move(cleanup_fn);
    slots_.push_back(std::move(slot));
    registered_ids_.push_back(id.value());
    total_registrations_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ReaperRegistry::unregister_runtime(RuntimeId id) noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find(registered_ids_.begin(), registered_ids_.end(), id.value());
        if (it != registered_ids_.end()) {
            registered_ids_.erase(it);
        }
        auto sit = std::find_if(slots_.begin(), slots_.end(), [id](const auto& s) {
            return s->runtime_id == id;
        });
        if (sit != slots_.end()) {
            slots_.erase(sit);
        }
    }
    coordinator_cv_.notify_one();
}

bool ReaperRegistry::is_registration_open() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == RegistrationState::Open;
}

void ReaperRegistry::close_registration() noexcept {
    std::vector<std::function<void()>> graceful_callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == RegistrationState::Open) {
            mark_finalization_started_locked();
            if (registered_ids_.empty() && !coordinator_thread_) {
                mark_finalized_locked();
                finalization_cv_.notify_all();
            } else {
                state_ = RegistrationState::Finalizing;
                coordinator_cv_.notify_all();
            }
            for (const auto& s : slots_) {
                if (s->request_graceful_fn) {
                    graceful_callbacks.push_back(s->request_graceful_fn);
                }
            }
        }
    }
    for (const auto& cb : graceful_callbacks) {
        cb();
    }
}

void ReaperRegistry::request_all_immediate() noexcept {
    finalization_escalations_.fetch_add(1, std::memory_order_relaxed);
    std::vector<std::function<void()>> immediate_callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& s : slots_) {
            if (s->request_immediate_fn) {
                immediate_callbacks.push_back(s->request_immediate_fn);
            }
        }
    }
    for (const auto& cb : immediate_callbacks) {
        cb();
    }
}

HandoffCapabilitySlot* ReaperRegistry::find_slot(RuntimeId id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : slots_) {
        if (s->runtime_id == id) {
            return s.get();
        }
    }
    return nullptr;
}

void ReaperRegistry::execute_worker_handoff(
    RuntimeId id,
    std::shared_ptr<void> state,
    std::function<void()> cleanup_fn) noexcept {
    
    std::lock_guard<std::mutex> lock(mutex_);
            for (auto& s : slots_) {
                if (s->runtime_id == id) {
                    s->handoff_executed.store(true, std::memory_order_release);
                    s->retained_state = std::move(state);
                    s->cleanup_fn = std::move(cleanup_fn);
                    total_handoffs_.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
    coordinator_cv_.notify_one();
}

void ReaperRegistry::notify_join_ready(RuntimeId id) noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& s : slots_) {
            if (s->runtime_id == id) {
                s->join_ready.store(true, std::memory_order_release);
                break;
            }
        }
    }
    coordinator_cv_.notify_one();
}

bool ReaperRegistry::has_join_ready_slot_locked() const noexcept {
    for (const auto& s : slots_) {
        if ((s->handoff_executed.load(std::memory_order_acquire) || state_ == RegistrationState::Finalizing) &&
            s->join_ready.load(std::memory_order_acquire) &&
            !s->join_claimed.load(std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void ReaperRegistry::coordinator_loop() noexcept {
    try {
        while (true) {
            std::unique_lock<std::mutex> lock(mutex_);
            coordinator_cv_.wait(lock, [this] {
                return coordinator_stop_ || inject_coordinator_fail_ || has_join_ready_slot_locked() ||
                       (state_ == RegistrationState::Finalizing && slots_.empty() && registered_ids_.empty());
            });

            if (inject_coordinator_fail_) {
                throw std::runtime_error("Injected coordinator control plane fatal failure");
            }

            if (coordinator_stop_ && !has_join_ready_slot_locked()) {
                coordinator_exited_ = true;
                finalization_cv_.notify_all();
                break;
            }

            if (state_ == RegistrationState::Finalizing && slots_.empty() && registered_ids_.empty()) {
                coordinator_exited_ = true;
                finalization_cv_.notify_all();
                break;
            }

            struct ReadyWork {
                RuntimeId runtime_id;
                std::function<void()> cleanup_fn;
                std::shared_ptr<void> retained_state;
            };
            std::vector<ReadyWork> works;

            for (auto it = slots_.begin(); it != slots_.end(); ) {
                auto& s = *it;
                if ((s->handoff_executed.load(std::memory_order_acquire) || state_ == RegistrationState::Finalizing) &&
                    s->join_ready.load(std::memory_order_acquire) &&
                    !s->join_claimed.exchange(true, std::memory_order_acq_rel)) {
                    
                    works.push_back(ReadyWork{
                        s->runtime_id,
                        std::move(s->cleanup_fn),
                        std::move(s->retained_state)
                    });

                    auto rid_it = std::find(registered_ids_.begin(), registered_ids_.end(), s->runtime_id.value());
                    if (rid_it != registered_ids_.end()) {
                        registered_ids_.erase(rid_it);
                    }
                    it = slots_.erase(it);
                } else {
                    ++it;
                }
            }

            if (!works.empty()) {
                total_joins_.fetch_add(static_cast<std::uint64_t>(works.size()), std::memory_order_relaxed);
            }

            lock.unlock();

            // 锁外执行 join 与状态发布，防止 head-of-line 锁竞争
            for (auto& work : works) {
                if (work.cleanup_fn) {
                    work.cleanup_fn();
                }
                work.retained_state.reset();
            }
        }
    } catch (...) {
        std::fputs("Fatal: Unhandled escaping exception in Reaper coordinator loop\n", stderr);
        std::terminate();
    }
}

void ReaperRegistry::wait_finalization() {
    if (current_worker_runtime_id() != RuntimeId{0}) {
        throw std::logic_error("FinalizationControl::wait cannot be called by a worker thread");
    }

    std::unique_lock<std::mutex> lock(mutex_);
    while (state_ != RegistrationState::Finalized) {
        if (!coordinator_thread_) {
            mark_finalized_locked();
            finalization_cv_.notify_all();
            return;
        }

        if (coordinator_exited_ && registered_ids_.empty() && slots_.empty()) {
            if (!coordinator_join_in_progress_) {
                coordinator_join_in_progress_ = true;
                auto t = std::move(coordinator_thread_);
                lock.unlock();
                if (t && t->joinable()) {
                    t->join();
                }
                lock.lock();
                mark_finalized_locked();
                coordinator_join_in_progress_ = false;
                finalization_cv_.notify_all();
                return;
            } else {
                finalization_cv_.wait(lock, [this] {
                    return state_ == RegistrationState::Finalized;
                });
                return;
            }
        }

        finalization_cv_.wait(lock, [this] {
            return state_ == RegistrationState::Finalized ||
                   (coordinator_exited_ && registered_ids_.empty() && slots_.empty());
        });
    }
}

FinalizationWaitResult ReaperRegistry::wait_finalization_for(std::chrono::nanoseconds timeout_ns) {
    const auto result = wait_finalization_for_impl(timeout_ns);
    if (result == FinalizationWaitResult::TimedOut) {
        // 只统计真实返回 TimedOut 的等待（D-148 invariant）。
        finalization_wait_timeouts_.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

FinalizationWaitResult ReaperRegistry::wait_finalization_for_impl(std::chrono::nanoseconds timeout_ns) {
    if (current_worker_runtime_id() != RuntimeId{0}) {
        throw std::logic_error("FinalizationControl::wait_for cannot be called by a worker thread");
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ == RegistrationState::Finalized) {
        return FinalizationWaitResult::Completed;
    }

    if (!coordinator_thread_) {
        mark_finalized_locked();
        finalization_cv_.notify_all();
        return FinalizationWaitResult::Completed;
    }

    if (timeout_ns <= std::chrono::nanoseconds::zero()) {
        if (coordinator_exited_ && registered_ids_.empty() && slots_.empty()) {
            if (!coordinator_join_in_progress_) {
                coordinator_join_in_progress_ = true;
                auto t = std::move(coordinator_thread_);
                lock.unlock();
                if (t && t->joinable()) {
                    t->join();
                }
                lock.lock();
                mark_finalized_locked();
                coordinator_join_in_progress_ = false;
                finalization_cv_.notify_all();
                return FinalizationWaitResult::Completed;
            }
        }
        return state_ == RegistrationState::Finalized ? FinalizationWaitResult::Completed : FinalizationWaitResult::TimedOut;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout_ns;

    while (state_ != RegistrationState::Finalized) {
        if (coordinator_exited_ && registered_ids_.empty() && slots_.empty()) {
            if (!coordinator_join_in_progress_) {
                coordinator_join_in_progress_ = true;
                auto t = std::move(coordinator_thread_);
                lock.unlock();
                if (t && t->joinable()) {
                    t->join();
                }
                lock.lock();
                mark_finalized_locked();
                coordinator_join_in_progress_ = false;
                finalization_cv_.notify_all();
                return FinalizationWaitResult::Completed;
            } else {
                finalization_cv_.wait_until(lock, deadline, [this] {
                    return state_ == RegistrationState::Finalized;
                });
                return state_ == RegistrationState::Finalized ? FinalizationWaitResult::Completed : FinalizationWaitResult::TimedOut;
            }
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return state_ == RegistrationState::Finalized ? FinalizationWaitResult::Completed : FinalizationWaitResult::TimedOut;
        }

        if (!finalization_cv_.wait_until(lock, deadline, [this] {
            return state_ == RegistrationState::Finalized ||
                   (coordinator_exited_ && registered_ids_.empty() && slots_.empty());
        })) {
            if (coordinator_exited_ && registered_ids_.empty() && slots_.empty()) {
                if (!coordinator_join_in_progress_) {
                    coordinator_join_in_progress_ = true;
                    auto t = std::move(coordinator_thread_);
                    lock.unlock();
                    if (t && t->joinable()) {
                        t->join();
                    }
                    lock.lock();
                    mark_finalized_locked();
                    coordinator_join_in_progress_ = false;
                    finalization_cv_.notify_all();
                    return FinalizationWaitResult::Completed;
                } else {
                    finalization_cv_.wait_until(lock, deadline, [this] {
                        return state_ == RegistrationState::Finalized;
                    });
                    return state_ == RegistrationState::Finalized ? FinalizationWaitResult::Completed : FinalizationWaitResult::TimedOut;
                }
            }
            return state_ == RegistrationState::Finalized ? FinalizationWaitResult::Completed : FinalizationWaitResult::TimedOut;
        }
    }

    return FinalizationWaitResult::Completed;
}

std::size_t ReaperRegistry::coordinator_thread_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return coordinator_thread_ ? 1 : 0;
}

void ReaperRegistry::mark_finalization_started_locked() noexcept {
    if (finalization_started_at_ == std::chrono::steady_clock::time_point{}) {
        finalization_started_at_ = std::chrono::steady_clock::now();
    }
}

void ReaperRegistry::mark_finalized_locked() noexcept {
    mark_finalization_started_locked();
    state_ = RegistrationState::Finalized;
    if (finalization_completed_at_ == std::chrono::steady_clock::time_point{}) {
        finalization_completed_at_ = std::chrono::steady_clock::now();
    }
}

void ReaperRegistry::note_finalization_begin() noexcept {
    finalization_begin_calls_.fetch_add(1, std::memory_order_relaxed);
}

astra::ProcessMetricsSnapshot ReaperRegistry::process_snapshot() const noexcept {
    const auto t_start = std::chrono::steady_clock::now();
    astra::ProcessMetricsSnapshot snap{};
    snap.capture_started_at = t_start;
    snap.counters.runtime_registrations = total_registrations_.load(std::memory_order_relaxed);
    snap.counters.runtime_handoffs = total_handoffs_.load(std::memory_order_relaxed);
    snap.counters.runtimes_joined = total_joins_.load(std::memory_order_relaxed);
    snap.counters.finalization_begin_calls = finalization_begin_calls_.load(std::memory_order_relaxed);
    snap.counters.finalization_wait_timeouts = finalization_wait_timeouts_.load(std::memory_order_relaxed);
    snap.counters.finalization_escalations = finalization_escalations_.load(std::memory_order_relaxed);

    // 单一控制面锁：gauges 与状态/时间锚点在同一线性化点读取（D-148）。
    std::unique_lock<std::mutex> lock(mutex_);
    snap.gauges.registered_runtimes = static_cast<std::uint64_t>(registered_ids_.size());
    for (const auto& s : slots_) {
        if (s->join_ready.load(std::memory_order_acquire)) {
            if (!s->join_claimed.load(std::memory_order_acquire)) {
                ++snap.gauges.join_ready_runtimes;
            }
        } else if (s->handoff_executed.load(std::memory_order_acquire)) {
            ++snap.gauges.pending_runtimes;
        }
    }

    switch (state_) {
        case RegistrationState::Finalizing:
            snap.service_state = astra::ProcessServiceState::Finalizing;
            snap.finalization_state = astra::ProcessFinalizationState::Finalizing;
            break;
        case RegistrationState::Finalized:
            snap.service_state = astra::ProcessServiceState::Finalized;
            snap.finalization_state = astra::ProcessFinalizationState::Finalized;
            break;
        case RegistrationState::Open:
            if (coordinator_thread_ || total_registrations_.load(std::memory_order_relaxed) != 0) {
                snap.service_state = astra::ProcessServiceState::Active;
            } else {
                snap.service_state = astra::ProcessServiceState::NotStarted;
            }
            snap.finalization_state = astra::ProcessFinalizationState::NotStarted;
            break;
    }

    snap.finalization_started_at = finalization_started_at_;
    const auto now = std::chrono::steady_clock::now();
    constexpr auto kEpoch = std::chrono::steady_clock::time_point{};
    if (state_ == RegistrationState::Finalizing && finalization_started_at_ != kEpoch) {
        snap.finalization_elapsed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - finalization_started_at_).count());
    } else if (state_ == RegistrationState::Finalized && finalization_started_at_ != kEpoch &&
               finalization_completed_at_ != kEpoch) {
        const auto duration = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            finalization_completed_at_ - finalization_started_at_).count());
        snap.finalization_elapsed_ns = duration;
        snap.finalization_completion_duration_ns = duration;
    }
    lock.unlock();

    snap.saturated =
        snap.counters.runtime_registrations == std::numeric_limits<std::uint64_t>::max() ||
        snap.counters.runtime_handoffs == std::numeric_limits<std::uint64_t>::max() ||
        snap.counters.runtimes_joined == std::numeric_limits<std::uint64_t>::max() ||
        snap.counters.finalization_begin_calls == std::numeric_limits<std::uint64_t>::max() ||
        snap.counters.finalization_wait_timeouts == std::numeric_limits<std::uint64_t>::max() ||
        snap.counters.finalization_escalations == std::numeric_limits<std::uint64_t>::max();

    snap.capture_finished_at = std::chrono::steady_clock::now();
    return snap;
}

void ReaperRegistry::reset_for_testing() noexcept {
    std::unique_ptr<std::thread> thread_to_join;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = RegistrationState::Open;
        registered_ids_.clear();
        inject_reservation_fail_ = false;
        inject_worker_fail_at_ = 0;
        slots_.clear();
        coordinator_stop_ = true;
        coordinator_exited_ = false;
        coordinator_join_in_progress_ = false;
        inject_coordinator_fail_ = false;
        total_registrations_.store(0, std::memory_order_relaxed);
        total_handoffs_.store(0, std::memory_order_relaxed);
        total_joins_.store(0, std::memory_order_relaxed);
        finalization_begin_calls_.store(0, std::memory_order_relaxed);
        finalization_wait_timeouts_.store(0, std::memory_order_relaxed);
        finalization_escalations_.store(0, std::memory_order_relaxed);
        finalization_started_at_ = std::chrono::steady_clock::time_point{};
        finalization_completed_at_ = std::chrono::steady_clock::time_point{};
        coordinator_cv_.notify_all();
        finalization_cv_.notify_all();
        thread_to_join = std::move(coordinator_thread_);
    }
    if (thread_to_join && thread_to_join->joinable()) {
        thread_to_join->join();
    }
}

void ReaperRegistry::inject_handoff_reservation_failure(bool fail) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    inject_reservation_fail_ = fail;
}

void ReaperRegistry::inject_worker_creation_failure_at(std::size_t index) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    inject_worker_fail_at_ = index;
}

void ReaperRegistry::inject_coordinator_failure(bool fail) noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inject_coordinator_fail_ = fail;
    }
    coordinator_cv_.notify_all();
}

bool ReaperRegistry::should_fail_reservation() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return inject_reservation_fail_;
}

std::size_t ReaperRegistry::worker_creation_failure_index() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return inject_worker_fail_at_;
}

std::size_t ReaperRegistry::registered_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return registered_ids_.size();
}

}  // namespace astra::detail

namespace astra {

astra::ProcessMetricsSnapshot process_metrics_snapshot() noexcept {
    // Side-effect-free（R-095 / D-148）：仅读取既有单例状态，绝不初始化服务。
    return detail::ReaperRegistry::instance().process_snapshot();
}

}  // namespace astra

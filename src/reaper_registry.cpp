#include "reaper_registry.hpp"
#include <astra/scheduler.hpp>

#include <algorithm>
#include <chrono>
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
            if (registered_ids_.empty() && !coordinator_thread_) {
                state_ = RegistrationState::Finalized;
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
    while (true) {
        std::unique_lock<std::mutex> lock(mutex_);
        coordinator_cv_.wait(lock, [this] {
            return coordinator_stop_ || has_join_ready_slot_locked() ||
                   (state_ == RegistrationState::Finalizing && slots_.empty() && registered_ids_.empty());
        });

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

        lock.unlock();

        // 锁外执行 join 与状态发布，防止 head-of-line 锁竞争
        for (auto& work : works) {
            if (work.cleanup_fn) {
                work.cleanup_fn();
            }
            work.retained_state.reset();
        }
    }
}

void ReaperRegistry::wait_finalization() {
    if (current_worker_runtime_id() != RuntimeId{0}) {
        throw std::logic_error("FinalizationControl::wait cannot be called by a worker thread");
    }

    std::unique_lock<std::mutex> lock(mutex_);
    while (state_ != RegistrationState::Finalized) {
        if (!coordinator_thread_) {
            state_ = RegistrationState::Finalized;
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
                state_ = RegistrationState::Finalized;
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
    if (current_worker_runtime_id() != RuntimeId{0}) {
        throw std::logic_error("FinalizationControl::wait_for cannot be called by a worker thread");
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ == RegistrationState::Finalized) {
        return FinalizationWaitResult::Completed;
    }

    if (!coordinator_thread_) {
        state_ = RegistrationState::Finalized;
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
                state_ = RegistrationState::Finalized;
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
                state_ = RegistrationState::Finalized;
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
                    state_ = RegistrationState::Finalized;
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

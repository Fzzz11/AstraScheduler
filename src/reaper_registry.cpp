#include "reaper_registry.hpp"

#include <algorithm>

namespace astra::detail {

ReaperRegistry& ReaperRegistry::instance() noexcept {
    static ReaperRegistry registry;
    return registry;
}

ReaperRegistry::~ReaperRegistry() {
    reset_for_testing();
}

bool ReaperRegistry::register_runtime(RuntimeId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RegistrationState::Open) {
        return false;
    }
    if (inject_reservation_fail_) {
        return false;
    }
    auto slot = std::make_unique<HandoffCapabilitySlot>();
    slot->runtime_id = id;
    slots_.push_back(std::move(slot));
    registered_ids_.push_back(id.value());
    return true;
}

void ReaperRegistry::unregister_runtime(RuntimeId id) noexcept {
    std::unique_ptr<std::thread> thread_to_join;
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
            if ((*sit)->reaper_thread && (*sit)->reaper_thread->joinable()) {
                thread_to_join = std::move((*sit)->reaper_thread);
            }
            slots_.erase(sit);
        }
    }
    if (thread_to_join && thread_to_join->joinable()) {
        thread_to_join->join();
    }
}

bool ReaperRegistry::is_registration_open() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == RegistrationState::Open;
}

void ReaperRegistry::close_registration() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == RegistrationState::Open) {
        state_ = RegistrationState::Finalizing;
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
            s->reaper_thread = std::make_unique<std::thread>(
                [this, id, cleanup = std::move(cleanup_fn)]() mutable {
                    if (cleanup) {
                        cleanup();
                    }
                    std::lock_guard<std::mutex> lk(mutex_);
                    for (auto& slot : slots_) {
                        if (slot->runtime_id == id) {
                            slot->join_ready.store(true, std::memory_order_release);
                            slot->retained_state.reset();
                            break;
                        }
                    }
                    auto it = std::find(registered_ids_.begin(), registered_ids_.end(), id.value());
                    if (it != registered_ids_.end()) {
                        registered_ids_.erase(it);
                    }
                });
            return;
        }
    }
}

void ReaperRegistry::reset_for_testing() noexcept {
    std::vector<std::unique_ptr<std::thread>> threads_to_join;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = RegistrationState::Open;
        registered_ids_.clear();
        inject_reservation_fail_ = false;
        inject_worker_fail_at_ = 0;
        for (auto& s : slots_) {
            if (s->reaper_thread && s->reaper_thread->joinable()) {
                threads_to_join.push_back(std::move(s->reaper_thread));
            }
        }
        slots_.clear();
    }
    for (auto& t : threads_to_join) {
        if (t && t->joinable()) {
            t->join();
        }
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

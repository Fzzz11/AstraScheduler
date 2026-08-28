#include "reaper_registry.hpp"

#include <algorithm>

namespace astra::detail {

ReaperRegistry& ReaperRegistry::instance() noexcept {
    static ReaperRegistry registry;
    return registry;
}

bool ReaperRegistry::register_runtime(RuntimeId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RegistrationState::Open) {
        return false;
    }
    if (inject_reservation_fail_) {
        return false;
    }
    registered_ids_.push_back(id.value());
    return true;
}

void ReaperRegistry::unregister_runtime(RuntimeId id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find(registered_ids_.begin(), registered_ids_.end(), id.value());
    if (it != registered_ids_.end()) {
        registered_ids_.erase(it);
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

void ReaperRegistry::reset_for_testing() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = RegistrationState::Open;
    registered_ids_.clear();
    inject_reservation_fail_ = false;
    inject_worker_fail_at_ = 0;
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

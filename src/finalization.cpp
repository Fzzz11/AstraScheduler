#include <astra/finalization.hpp>
#include "reaper_registry.hpp"

#include <chrono>
#include <memory>

namespace astra {

struct FinalizationControl::Impl {
    explicit Impl() = default;

    void wait() const {
        detail::ReaperRegistry::instance().wait_finalization();
    }

    FinalizationWaitResult wait_for(std::chrono::nanoseconds timeout_ns) const {
        return detail::ReaperRegistry::instance().wait_finalization_for(timeout_ns);
    }

    void request_immediate() const noexcept {
        detail::ReaperRegistry::instance().request_all_immediate();
    }
};

FinalizationControl::FinalizationControl(std::shared_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

void FinalizationControl::wait() const {
    if (impl_) {
        impl_->wait();
    }
}

FinalizationWaitResult FinalizationControl::wait_for_impl(std::chrono::nanoseconds timeout_ns) const {
    if (impl_) {
        return impl_->wait_for(timeout_ns);
    }
    return FinalizationWaitResult::Completed;
}

void FinalizationControl::request_immediate() const noexcept {
    if (impl_) {
        impl_->request_immediate();
    }
}

FinalizationControl begin_finalization() noexcept {
    // 每次 API invocation 累计 begin_calls，即使共享同一 Finalization（D-148）。
    detail::ReaperRegistry::instance().note_finalization_begin();
    detail::ReaperRegistry::instance().close_registration();
    static auto shared_impl = std::make_shared<FinalizationControl::Impl>();
    return FinalizationControl(shared_impl);
}

}  // namespace astra

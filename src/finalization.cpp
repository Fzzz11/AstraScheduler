#include <astra/finalization.hpp>
#include "reaper_registry.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace astra {

struct FinalizationControl::Impl {
    explicit Impl() = default;

    void wait() const {
        // AST-018: 基础 capability surface；AST-020 扩展完整等待闭包
    }

    FinalizationWaitResult wait_for(std::chrono::nanoseconds timeout_ns) const {
        // AST-018: 基础 capability surface；AST-020 扩展限时等待
        (void)timeout_ns;
        return FinalizationWaitResult::Completed;
    }

    void request_immediate() const noexcept {
        // AST-018: 基础 capability surface；AST-021 扩展全域 Immediate 升级
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
    detail::ReaperRegistry::instance().close_registration();
    static auto shared_impl = std::make_shared<FinalizationControl::Impl>();
    return FinalizationControl(shared_impl);
}

}  // namespace astra

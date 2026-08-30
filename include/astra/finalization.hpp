#ifndef ASTRA_FINALIZATION_HPP
#define ASTRA_FINALIZATION_HPP

// AstraScheduler 进程级 Finalization 控制接口（AST-018 / R-035 / R-036 / R-043 / R-044 / R-045 / R-046）
// 仅支持 64-bit Linux（R-111，经 export.hpp 验证）。

#include <astra/export.hpp>

#include <chrono>
#include <cstdint>
#include <memory>

namespace astra {

// Finalization 等待结果枚举（R-044 / D-039）。
enum class FinalizationWaitResult : std::uint8_t {
    Completed,
    TimedOut
};

class FinalizationControl;

// 进程级收尾入口：永久关闭新 Scheduler 注册，并开始等待已有 Runtime 退出。
// 可重复调用，返回共享同一收尾的 capability。不抛（R-035 / R-045）。
[[nodiscard]] ASTRA_EXPORT FinalizationControl begin_finalization() noexcept;

// 进程级收尾句柄。可复制；析构无副作用，不会取消正在进行的 Finalization（R-036）。
class ASTRA_EXPORT FinalizationControl {
public:
    ~FinalizationControl() noexcept = default;

    FinalizationControl(const FinalizationControl&) noexcept = default;
    FinalizationControl& operator=(const FinalizationControl&) noexcept = default;
    FinalizationControl(FinalizationControl&&) noexcept = default;
    FinalizationControl& operator=(FinalizationControl&&) noexcept = default;

    // 阻塞直到全部已注册 Runtime 关停且 Reaper 退出（R-045）。
    void wait() const;

    // 有界等待。到期返回 TimedOut，收尾继续进行，不取消（R-044 / R-045）。
    template <typename Rep, typename Period>
    [[nodiscard]] FinalizationWaitResult wait_for(const std::chrono::duration<Rep, Period>& timeout) const {
        const auto timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout);
        return wait_for_impl(timeout_ns);
    }

    // 将尚未停机的 Runtime 升级为 Immediate。可重复调用，不抛（R-045）。
    void request_immediate() const noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;

    // 私有构造，仅允许 begin_finalization() 创建（R-035 / D-030）。
    explicit FinalizationControl(std::shared_ptr<Impl> impl) noexcept;

    [[nodiscard]] FinalizationWaitResult wait_for_impl(std::chrono::nanoseconds timeout_ns) const;

    friend ASTRA_EXPORT FinalizationControl begin_finalization() noexcept;
};

}  // namespace astra

#endif  // ASTRA_FINALIZATION_HPP

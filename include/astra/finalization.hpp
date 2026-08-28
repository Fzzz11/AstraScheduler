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

// 进程级 Finalization 唯一合法创建入口（R-035 / R-045 / D-030 / D-039）。
[[nodiscard]] ASTRA_EXPORT FinalizationControl begin_finalization() noexcept;

// Finalization 控制 capability 对象（R-035 / R-036 / R-045 / D-030 / D-031 / D-039）。
class ASTRA_EXPORT FinalizationControl {
public:
    ~FinalizationControl() noexcept = default;

    // 共享 capability 语义：可复制、可移动，析构无副作用（R-036 / R-045）。
    FinalizationControl(const FinalizationControl&) noexcept = default;
    FinalizationControl& operator=(const FinalizationControl&) noexcept = default;
    FinalizationControl(FinalizationControl&&) noexcept = default;
    FinalizationControl& operator=(FinalizationControl&&) noexcept = default;

    // 同步无界等待所有已注册 Runtime 完成 shutdown 与 Reaper 线程退出（R-045 / D-039）。
    void wait() const;

    // 限时等待 Finalization 完成（R-045 / D-039）。
    template <typename Rep, typename Period>
    [[nodiscard]] FinalizationWaitResult wait_for(const std::chrono::duration<Rep, Period>& timeout) const {
        const auto timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout);
        return wait_for_impl(timeout_ns);
    }

    // 升级为 Immediate 停机模式（R-045 / D-039）。
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

#ifndef ASTRA_CAPABILITIES_HPP
#define ASTRA_CAPABILITIES_HPP

// AstraScheduler 运行期能力快照（AST-004 / R-101 / D-101 / D-162 / D-167）。
// 报告实际 Local Deque backend，不可由用户 aggregate-initialize。
// Supported Configuration 仅 64-bit Linux（R-111，经 export.hpp 检查）。

#include <astra/export.hpp>

#include <compare>
#include <cstdint>

namespace astra {

// Local Deque 实际后端实现（D-162）。
enum class LocalDequeBackend : std::uint8_t {
    None,
    Locked,
    ChaseLevLockFree
};

// 冻结的不可变 Scheduler 能力快照（D-162：非 aggregate、可平凡复制）。
class SchedulerCapabilities {
public:
    constexpr SchedulerCapabilities() noexcept = default;
    constexpr explicit SchedulerCapabilities(LocalDequeBackend backend) noexcept
        : backend_(backend) {}

    [[nodiscard]] constexpr LocalDequeBackend local_deque_backend() const noexcept {
        return backend_;
    }

    [[nodiscard]] constexpr bool lock_free_local_deque() const noexcept {
        return backend_ == LocalDequeBackend::ChaseLevLockFree;
    }

    friend constexpr bool operator==(const SchedulerCapabilities&,
                                     const SchedulerCapabilities&) = default;
    friend constexpr auto operator<=>(const SchedulerCapabilities&,
                                      const SchedulerCapabilities&) = default;

private:
    LocalDequeBackend backend_{LocalDequeBackend::None};
};

}  // namespace astra

#endif  // ASTRA_CAPABILITIES_HPP

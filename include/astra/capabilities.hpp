#ifndef ASTRA_CAPABILITIES_HPP
#define ASTRA_CAPABILITIES_HPP

// AstraScheduler 运行期能力快照（AST-004 / R-101 / D-101 / D-162 / D-167）。
// 报告实际 Local Deque backend，不可由用户 aggregate-initialize。
// Supported Configuration 仅 64-bit Linux（R-111，经 export.hpp 检查）。

// 【通俗说明】同一份代码在不同机器上可能走不同实现路径（例如本地队列的
// 无锁版本，或它的加锁回退版）。capabilities() 把"这个运行时实际用的
// 是哪一种"如实报告，供性能问题归因——绝不为了好听而虚报 lock-free。

#include <astra/export.hpp>

#include <compare>
#include <cstdint>

namespace astra {

// Local Deque 实际后端实现（D-162）。报告 Runtime 实际选用的本地任务队列后端。
/** @brief 实际启用的本地任务队列后端。 */
enum class LocalDequeBackend : std::uint8_t {
    // 未启用本地队列（默认构造的空快照）。
    None,
    // mutex 保护的本地队列。
    Locked,
    // Chase-Lev 无锁本地队列。
    ChaseLevLockFree
};

// 启动时冻结的能力快照，由 Scheduler::capabilities() 返回。非 aggregate（D-162）。
/**
 * @brief Scheduler 启动时冻结的运行期能力快照。
 * @note 该快照反映实际后端，不会把加锁实现报告为 lock-free。
 */
class SchedulerCapabilities {
public:
    constexpr SchedulerCapabilities() noexcept = default;
    constexpr explicit SchedulerCapabilities(LocalDequeBackend backend) noexcept
        : backend_(backend) {}

    /** @brief 返回 Runtime 实际选用的本地队列后端。 */
    [[nodiscard]] constexpr LocalDequeBackend local_deque_backend() const noexcept {
        return backend_;
    }

    /** @brief 返回本地队列是否为 Chase-Lev 无锁后端。 */
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

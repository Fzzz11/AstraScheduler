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
enum class LocalDequeBackend : std::uint8_t {
    // 未启用本地双端队列后端（当前 Scheduler 实现的默认状态，尚未接入真实队列）。
    None,
    // 基于锁（mutex）的本地双端队列后端，实现简单但存在锁竞争开销。
    Locked,
    // 基于 Chase-Lev 算法的无锁（lock-free）本地双端队列后端，适用于高并发 work-stealing。
    ChaseLevLockFree
};

// 冻结的不可变 Scheduler 能力快照（D-162：非 aggregate、可平凡复制）。
class SchedulerCapabilities {
public:
    // 默认构造：后端为 None（D-162）。
    constexpr SchedulerCapabilities() noexcept = default;
    // 以指定的 Local Deque 后端构造能力快照（D-162）。
    constexpr explicit SchedulerCapabilities(LocalDequeBackend backend) noexcept
        : backend_(backend) {}

    // 返回 Runtime 实际选用的本地双端队列后端（D-162）。
    [[nodiscard]] constexpr LocalDequeBackend local_deque_backend() const noexcept {
        return backend_;
    }

    // 当且仅当本地双端队列后端为无锁（ChaseLevLockFree）时返回 true（D-162）。
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

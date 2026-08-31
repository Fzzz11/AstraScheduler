#ifndef ASTRA_STATUS_HPP
#define ASTRA_STATUS_HPP

// 【通俗说明】TaskState 描述一个任务的一生：Ready（已提交）-> Running ->
// 终态（Succeeded / Failed / Cancelled）；Running 与 Suspended（挂起中）
// 之间可往返。WaitResult 是 wait_for 的返回：TimedOut 只是你没等到，
// 任务本身还在继续跑，并没有被取消。
// AstraScheduler 生命周期状态与关停模式成对快照（AST-004 / R-099 / D-160）。
// SchedulerStatus 为可平凡复制的成对快照，合法组合仅有：
// - Running + None
// - Stopping + Graceful / Immediate
// - Stopped + Graceful / Immediate
// Supported Configuration 仅 64-bit Linux（R-111，经 export.hpp 检查）。

#include <astra/export.hpp>

#include <cstdint>

namespace astra {

// 对外可见的 Runtime 生命周期。不暴露 Created/Starting（D-155 / D-160）。
/** @brief Runtime 对外可见的生命周期状态。 */
enum class SchedulerState : std::uint8_t {
    Running,
    Stopping,
    Stopped
};

// None 仅与 Running 配对；Stopping/Stopped 为 Graceful 或 Immediate（D-160）。
/** @brief Runtime 当前采用的关停模式。 */
enum class ShutdownMode : std::uint8_t {
    None,
    Graceful,
    Immediate
};

// status() 的非阻塞快照。合法组合见文件头。
/** @brief Scheduler 生命周期与关停模式的不可变快照。 */
struct SchedulerStatus {
    SchedulerState state{SchedulerState::Running};
    ShutdownMode shutdown_mode{ShutdownMode::None};

    friend constexpr bool operator==(const SchedulerStatus&,
                                     const SchedulerStatus&) = default;
};

// 任务生命周期。Succeeded/Failed/Cancelled 为终态，不可逆。
// Running 与 Suspended 可往返（R-057）。
/** @brief 单个任务从提交到终态的生命周期状态。 */
enum class TaskState : std::uint8_t {
    Waiting,
    Ready,
    Running,
    Suspended,
    Succeeded,
    Failed,
    Cancelled
};

// wait_for 的返回。TimedOut 只表示调用方没等到，任务未被取消（R-056）。
/** @brief TaskHandle::wait_for 的完成或超时结果。 */
enum class WaitResult : std::uint8_t {
    Completed,
    TimedOut,
};

}  // namespace astra

#endif  // ASTRA_STATUS_HPP

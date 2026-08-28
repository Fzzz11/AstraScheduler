#ifndef ASTRA_STATUS_HPP
#define ASTRA_STATUS_HPP

// AstraScheduler 生命周期状态与关停模式成对快照（AST-004 / R-099 / D-160）。
// SchedulerStatus 为可平凡复制的成对快照，合法组合仅有：
// - Running + None
// - Stopping + Graceful / Immediate
// - Stopped + Graceful / Immediate
// Supported Configuration 仅 64-bit Linux（R-111，经 export.hpp 检查）。

#include <astra/export.hpp>

#include <cstdint>

namespace astra {

// Scheduler 生命周期状态（D-155 / D-160：不公开 Created/Starting）。
enum class SchedulerState : std::uint8_t {
    Running,
    Stopping,
    Stopped
};

// Scheduler 关停策略模式（D-012 / D-160）。
enum class ShutdownMode : std::uint8_t {
    None,
    Graceful,
    Immediate
};

// 一次线性化返回的成对生命周期快照（D-160）。
struct SchedulerStatus {
    SchedulerState state{SchedulerState::Running};
    ShutdownMode shutdown_mode{ShutdownMode::None};

    friend constexpr bool operator==(const SchedulerStatus&,
                                     const SchedulerStatus&) = default;
};

// Task 稳定生命周期状态（R-057 / D-069 / D-070）。
enum class TaskState : std::uint8_t {
    Waiting,
    Ready,
    Running,
    Suspended,
    Succeeded,
    Failed,
    Cancelled
};

// 有界等待结果枚举（R-056 / D-063）。
enum class WaitResult : std::uint8_t {
    Completed,
    TimedOut,
};

}  // namespace astra

#endif  // ASTRA_STATUS_HPP

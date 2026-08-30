#ifndef ASTRA_ERROR_HPP
#define ASTRA_ERROR_HPP

#include <astra/export.hpp>
#include <astra/id.hpp>

// AstraScheduler 的异常契约集中地。
//
// 【异常家族】
//   - submission_rejected：提交被拒（Stopping/Stopped/CapacityExhausted 等），
//     带机器可读的 reason()，调用方按枚举分支而非解析字符串。
//   - scheduler_creation_rejected：Scheduler 创建被拒（如进程级 Finalization
//     已启动后不再允许新建运行时）。
//   - task_cancelled：任务被取消时在任务体内/挂起点抛出的协作取消信号，
//     用 catch(...) 之外的 try/catch(task_cancelled&) 做清理。
//   - helping_depth_exceeded：worker 帮忙等待嵌套过深（防递归爆炸）。
//   - std::logic_error 系列：调用方用法错误（空句柄、自等待等），属编程
//     缺陷而非运行时状态。
// 所有异常类型都继承 std 标准异常，保证与既有 catch 结构兼容。
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace astra {

enum class SchedulerCreationError : std::uint8_t {
    FinalizationStarted = 1,
};

class ASTRA_EXPORT scheduler_creation_rejected : public std::runtime_error {
public:
    explicit scheduler_creation_rejected(SchedulerCreationError reason)
        : std::runtime_error(format_message(reason)), reason_(reason) {}

    // 机器可读拒绝原因，勿解析 what()。
    [[nodiscard]] SchedulerCreationError reason() const noexcept {
        return reason_;
    }

private:
    static const char* format_message(SchedulerCreationError reason) noexcept {
        switch (reason) {
            case SchedulerCreationError::FinalizationStarted:
                return "Scheduler creation rejected: process Finalization has already started";
            default:
                return "Scheduler creation rejected";
        }
    }

    SchedulerCreationError reason_;
};

// 任务提交拒绝原因（R-062 / D-087 / D-155）。
enum class SubmissionError : std::uint8_t {
    Stopping = 1,           // 已请求关停，不再接纳
    Stopped = 2,            // Runtime 已停止
    CapacityExhausted = 3,  // 外部队列满且未阻塞等待
};

// 任务提交拒绝异常（R-062 / D-087）。
class ASTRA_EXPORT submission_rejected : public std::runtime_error {
public:
    explicit submission_rejected(SubmissionError reason)
        : std::runtime_error(format_message(reason)), reason_(reason) {}

    // Stopping / Stopped / CapacityExhausted。
    [[nodiscard]] SubmissionError reason() const noexcept {
        return reason_;
    }

private:
    static const char* format_message(SubmissionError reason) noexcept {
        switch (reason) {
            case SubmissionError::Stopping:
                return "Task submission rejected: scheduler is stopping";
            case SubmissionError::Stopped:
                return "Task submission rejected: scheduler is stopped";
            case SubmissionError::CapacityExhausted:
                return "Task submission rejected: external pending capacity exhausted";
            default:
                return "Task submission rejected";
        }
    }

    SubmissionError reason_;
};

// 任务取消异常（R-050 / D-057 / D-058）。
class ASTRA_EXPORT task_cancelled : public std::exception {
public:
    task_cancelled() noexcept = default;
    [[nodiscard]] const char* what() const noexcept override {
        return "AstraScheduler task was cancelled";
    }
};

// 帮助等待深度超限异常（R-059 / D-079）。
class ASTRA_EXPORT helping_depth_exceeded : public std::runtime_error {
public:
    helping_depth_exceeded()
        : std::runtime_error("AstraScheduler helping depth limit exceeded") {}
};

// Graph 结构校验失败原因（R-069 / D-105）。
enum class GraphValidationError : std::uint8_t {
    ForeignNode = 1,
    SelfEdge = 2,
    DuplicateEdge = 3,
    Cycle = 4,
};

// Graph 结构校验失败异常（R-069 / D-105）。
class ASTRA_EXPORT graph_validation_error : public std::logic_error {
public:
    explicit graph_validation_error(GraphValidationError reason,
                                    std::vector<NodeId> witness = {})
        : std::logic_error(format_message(reason)),
          reason_(reason),
          cycle_witness_(std::move(witness)) {}

    [[nodiscard]] GraphValidationError reason() const noexcept {
        return reason_;
    }

    // 仅 Cycle 时非空：一条闭合环路上的 NodeId。
    [[nodiscard]] const std::vector<NodeId>& cycle_witness() const noexcept {
        return cycle_witness_;
    }

private:
    static const char* format_message(GraphValidationError reason) noexcept {
        switch (reason) {
            case GraphValidationError::ForeignNode:
                return "Graph validation failed: edge references node foreign to builder";
            case GraphValidationError::SelfEdge:
                return "Graph validation failed: self-edge detected";
            case GraphValidationError::DuplicateEdge:
                return "Graph validation failed: duplicate edge detected";
            case GraphValidationError::Cycle:
                return "Graph validation failed: cycle detected in graph";
            default:
                return "Graph validation failed";
        }
    }

    GraphValidationError reason_;
    std::vector<NodeId> cycle_witness_;
};

}  // namespace astra

#endif  // ASTRA_ERROR_HPP

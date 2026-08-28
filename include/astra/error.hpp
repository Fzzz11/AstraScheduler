#ifndef ASTRA_ERROR_HPP
#define ASTRA_ERROR_HPP

#include <astra/export.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace astra {

enum class SchedulerCreationError : std::uint8_t {
    FinalizationStarted = 1,
};

class ASTRA_EXPORT scheduler_creation_rejected : public std::runtime_error {
public:
    explicit scheduler_creation_rejected(SchedulerCreationError reason)
        : std::runtime_error(format_message(reason)), reason_(reason) {}

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
    Stopping = 1,
    Stopped = 2,
    CapacityExhausted = 3,
};

// 任务提交拒绝异常（R-062 / D-087）。
class ASTRA_EXPORT submission_rejected : public std::runtime_error {
public:
    explicit submission_rejected(SubmissionError reason)
        : std::runtime_error(format_message(reason)), reason_(reason) {}

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

}  // namespace astra

#endif  // ASTRA_ERROR_HPP

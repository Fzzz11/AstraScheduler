#ifndef ASTRA_TASK_OPTIONS_HPP
#define ASTRA_TASK_OPTIONS_HPP

#include <astra/export.hpp>

#include <chrono>

// 提交任务时的可选参数（submit/spawn 的第一个参数）。
//
// 【Priority】0..3 四个优先级带，数字越小越优先（Critical=0）。公平性
// 由调度器的日历轮转保证：高优先级先服务，但低优先级不会被饿死。
// 【Deadline】期望的开始时间点：在点前开始记 met，晚于记 missed——
// 截止时间描述的是"开始晚了没有"，不是"执行超时没有"。
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace astra {

// Task 优先级枚举（R-080 / D-129）。
enum class Priority : std::uint8_t {
    Low = 0,
    Normal = 1,
    High = 2,
    Critical = 3,
};

inline constexpr bool is_valid_priority(Priority p) noexcept {
    return static_cast<std::uint8_t>(p) <= static_cast<std::uint8_t>(Priority::Critical);
}

inline void validate_priority(Priority p) {
    if (!is_valid_priority(p)) {
        throw std::invalid_argument("unknown or invalid Priority value (R-080 / D-129)");
    }
}

// 强类型任务截止时间，包装 steady_clock 绝对时刻（R-082 / D-132）。
class TaskDeadline {
public:
    TaskDeadline() noexcept : target_(std::chrono::steady_clock::time_point::min()) {}
    explicit TaskDeadline(std::chrono::steady_clock::time_point tp) noexcept : target_(tp) {}

    static TaskDeadline at(std::chrono::steady_clock::time_point tp) noexcept {
        return TaskDeadline(tp);
    }

    template <typename Rep, typename Period>
    static TaskDeadline after(const std::chrono::duration<Rep, Period>& dur) noexcept {
        const auto now = std::chrono::steady_clock::now();
        using Duration = std::chrono::steady_clock::duration;
        const auto req_dur = std::chrono::duration_cast<Duration>(dur);

        if (req_dur >= Duration::zero()) {
            const auto max_add = std::chrono::steady_clock::time_point::max() - now;
            if (req_dur > max_add) {
                return TaskDeadline(std::chrono::steady_clock::time_point::max());
            }
        } else {
            const auto min_dur = std::chrono::steady_clock::duration::min();
            if (now.time_since_epoch() < min_dur - req_dur) {
                return TaskDeadline(std::chrono::steady_clock::time_point::min());
            }
        }
        return TaskDeadline(now + req_dur);
    }

    [[nodiscard]] std::chrono::steady_clock::time_point time_point() const noexcept {
        return target_;
    }

    friend bool operator==(const TaskDeadline& lhs, const TaskDeadline& rhs) noexcept {
        return lhs.target_ == rhs.target_;
    }

    friend auto operator<=>(const TaskDeadline& lhs, const TaskDeadline& rhs) noexcept {
        return lhs.target_ <=> rhs.target_;
    }

private:
    std::chrono::steady_clock::time_point target_;
};

enum class DeadlineDisposition : std::uint8_t {
    None = 0,
    Met = 1,
    Missed = 2,
};

// 统一任务配置选项值类型（R-080 / R-082 / D-129 / D-132）。
struct TaskOptions {
    Priority priority{Priority::Normal};
    std::optional<TaskDeadline> deadline{};

    friend bool operator==(const TaskOptions&, const TaskOptions&) = default;
};

}  // namespace astra

#endif  // ASTRA_TASK_OPTIONS_HPP

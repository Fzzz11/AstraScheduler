#ifndef ASTRA_TASK_OPTIONS_HPP
#define ASTRA_TASK_OPTIONS_HPP

#include <astra/export.hpp>

#include <chrono>

// 提交任务时的可选参数（submit/spawn/run 的 TaskOptions 参数）。
//
// 【Priority】Low < Normal < High < Critical；Critical 数值最大、服务份额最高。
// 日历轮转保证高优先级先服务，低优先级不会被饿死。
// 【Deadline】期望的首次开始时刻：按时开始记 Met，晚于记 Missed。
// 描述的是「开始晚了没有」，不是「执行超时没有」。
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace astra {

// 任务优先级。数值越大越优先；未知值在提交时抛 invalid_argument（R-080 / D-129）。
enum class Priority : std::uint8_t {
    Low = 0,
    Normal = 1,
    High = 2,
    Critical = 3,
};

inline constexpr bool is_valid_priority(Priority p) noexcept {
    return static_cast<std::uint8_t>(p) <= static_cast<std::uint8_t>(Priority::Critical);
}

// 非法 Priority 抛 invalid_argument。submit/emplace 在接纳前调用。
inline void validate_priority(Priority p) {
    if (!is_valid_priority(p)) {
        throw std::invalid_argument("unknown or invalid Priority value (R-080 / D-129)");
    }
}

// 首次开始的绝对截止时刻（steady_clock）。默认构造为 min，表示未指定（R-082 / D-132）。
class TaskDeadline {
public:
    TaskDeadline() noexcept : target_(std::chrono::steady_clock::time_point::min()) {}
    explicit TaskDeadline(std::chrono::steady_clock::time_point tp) noexcept : target_(tp) {}

    // 指定绝对时刻。
    static TaskDeadline at(std::chrono::steady_clock::time_point tp) noexcept {
        return TaskDeadline(tp);
    }

    // 相对 now 的偏移；溢出时钳制到 clock 的 min/max，不抛。
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

    // 底层绝对时刻。
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

// 首次 start 相对 deadline 的结论。无 deadline 为 None。
enum class DeadlineDisposition : std::uint8_t {
    None = 0,
    Met = 1,
    Missed = 2,
};

// 单次提交的 priority/deadline。值类型，提交时拷入任务，之后不可改（R-080 / R-082）。
struct TaskOptions {
    Priority priority{Priority::Normal};
    std::optional<TaskDeadline> deadline{};

    friend bool operator==(const TaskOptions&, const TaskOptions&) = default;
};

}  // namespace astra

#endif  // ASTRA_TASK_OPTIONS_HPP

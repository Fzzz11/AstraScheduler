#ifndef ASTRA_TASK_OPTIONS_HPP
#define ASTRA_TASK_OPTIONS_HPP

#include <astra/export.hpp>

#include <cstdint>
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

// 统一任务配置选项值类型（R-080 / R-082 / D-129 / D-132）。
struct TaskOptions {
    Priority priority{Priority::Normal};

    friend constexpr bool operator==(const TaskOptions&, const TaskOptions&) = default;
};

}  // namespace astra

#endif  // ASTRA_TASK_OPTIONS_HPP

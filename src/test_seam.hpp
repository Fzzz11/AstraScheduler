#ifndef ASTRA_SRC_TEST_SEAM_HPP
#define ASTRA_SRC_TEST_SEAM_HPP

// 白盒测试专用 seam（AST-006..031 测试基础设施）。非 public API：
// 仅测试工程与内部翻译单元使用，不随 install 面暴露（R-110）。

#include <astra/scheduler.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace astra::detail {

struct SchedulerTestAccess {
    static void run_task_on_worker(Scheduler& s, std::function<void()> task);
    static std::size_t global_queue_size(const Scheduler& s);
    static std::size_t external_pending_count(const Scheduler& s);
    static std::size_t parked_workers_count(const Scheduler& s);
    static std::uint64_t current_work_epoch(const Scheduler& s);
};

inline void run_test_task_on_worker(Scheduler& s, std::function<void()> task) {
    SchedulerTestAccess::run_task_on_worker(s, std::move(task));
}
inline std::size_t global_injection_queue_size(const Scheduler& s) {
    return SchedulerTestAccess::global_queue_size(s);
}
inline std::size_t external_pending_count(const Scheduler& s) {
    return SchedulerTestAccess::external_pending_count(s);
}
inline std::size_t parked_workers_count(const Scheduler& s) {
    return SchedulerTestAccess::parked_workers_count(s);
}
inline std::uint64_t current_work_epoch(const Scheduler& s) {
    return SchedulerTestAccess::current_work_epoch(s);
}

}  // namespace astra::detail

#endif  // ASTRA_SRC_TEST_SEAM_HPP

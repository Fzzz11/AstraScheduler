#ifndef ASTRA_SCHEDULER_HPP
#define ASTRA_SCHEDULER_HPP

// AstraScheduler 调度器公共 Handle 与契约（AST-004 / R-098 / R-099 / R-100 / R-101 / D-155）。
// Scheduler 为可复制/移动的共享 Handle，析构或操作遵循生命周期契约。
// Supported Configuration 仅 64-bit Linux（R-111，经 export.hpp 检查）。

#include <astra/export.hpp>
#include <astra/capabilities.hpp>
#include <astra/error.hpp>
#include <astra/id.hpp>
#include <astra/scheduler_options.hpp>
#include <astra/status.hpp>

#include <memory>

namespace astra {

// 调度器共享 Handle（D-155）。
class ASTRA_EXPORT Scheduler {
public:
    // 同步验证配置并初始化 Runtime（D-155 / D-157）。
    explicit Scheduler(SchedulerOptions options = {});

    ~Scheduler();

    // 共享 Handle 语义：复制关联同一 Runtime State（D-155）。
    Scheduler(const Scheduler&);
    Scheduler& operator=(const Scheduler&);

    // 移动构造与移动赋值：使源 Handle 为空（D-155）。
    Scheduler(Scheduler&&) noexcept;
    Scheduler& operator=(Scheduler&&) noexcept;

    // 查询当前 Handle 是否关联有效 Runtime State（D-155）。
    // [[nodiscard]] 告诉编译器这个函数的返回值不应该被忽略，帮助开发者避免潜在的错误。
    [[nodiscard]] bool valid() const noexcept;

    // 获取当前 Runtime 的强类型唯一标识；空 Handle 返回默认无效 ID（D-153 / D-155）。
    [[nodiscard]] RuntimeId runtime_id() const noexcept;

    // 获取一次线性化、非阻塞、无副作用的状态快照；空 Handle 抛 std::logic_error（D-160）。
    [[nodiscard]] SchedulerStatus status() const;

    // 获取当前 Runtime 冻结的能力快照；空 Handle 抛 std::logic_error（D-162）。
    [[nodiscard]] SchedulerCapabilities capabilities() const;

private:
    struct ASTRA_NO_EXPORT Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace astra

#endif  // ASTRA_SCHEDULER_HPP

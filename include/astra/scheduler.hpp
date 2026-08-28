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
#include <astra/task_handle.hpp>

#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace astra {

class Scheduler;

namespace detail {
enum class AdmissionDecision : std::uint8_t {
    Success,
    Stopping,
    Stopped,
    CapacityExhausted,
};

ASTRA_EXPORT RuntimeId current_worker_runtime_id() noexcept;
void run_test_task_on_worker(Scheduler& s, std::function<void()> task);
std::size_t global_injection_queue_size(const Scheduler& s);
std::size_t external_pending_count(const Scheduler& s);
}

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

    // 阻塞/按策略提交任务（R-048 / R-058 / R-061 / R-062 / R-102）。
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> TaskHandle<typename detail::InvocationTraits<F, Args...>::ResultType> {
        using Traits = detail::InvocationTraits<F, Args...>;

        static_assert(Traits::is_valid,
            "Callable must be invocable as f(args...) or f(std::stop_token, args...)");
        static_assert(!Traits::returns_reference,
            "AstraScheduler tasks cannot return raw references (R-058 / D-074). Wrap in std::reference_wrapper if needed.");
        static_assert(Traits::is_move_constructible,
            "Task result type must be move-constructible (R-058 / D-075).");

        using ResultType = typename Traits::ResultType;

        if (!valid()) {
            throw std::logic_error("operating on empty/moved-from Scheduler");
        }

        const bool is_internal = (detail::current_worker_runtime_id() == runtime_id());
        const bool is_worker = (detail::current_worker_runtime_id() != RuntimeId{0});
        const bool can_block = !is_worker; // R-061 / D-085: 仅普通非 Worker 线程可 Block

        const auto decision = acquire_admission(can_block, is_internal);
        if (decision == detail::AdmissionDecision::Stopping) {
            throw submission_rejected(SubmissionError::Stopping);
        }
        if (decision == detail::AdmissionDecision::Stopped) {
            throw submission_rejected(SubmissionError::Stopped);
        }
        if (decision == detail::AdmissionDecision::CapacityExhausted) {
            throw submission_rejected(SubmissionError::CapacityExhausted);
        }

        // 强异常安全事务：构造过程抛出异常则回滚 slot
        const TaskId tid = detail::allocate_task_id(runtime_id());
        std::shared_ptr<detail::TaskSharedState<ResultType>> state;
        std::unique_ptr<detail::TaskInvokerBase> invoker;

        try {
            state = std::make_shared<detail::TaskSharedState<ResultType>>(tid);
            invoker = detail::make_task_invoker<Traits::is_ordinary_invocable, ResultType>(
                state, std::forward<F>(f), std::forward<Args>(args)...);
        } catch (...) {
            if (!is_internal) {
                rollback_external_slot();
            }
            throw;
        }

        post_task_invoker(std::move(invoker), !is_internal);
        return TaskHandle<ResultType>(std::move(state));
    }

    // 非阻塞尝试提交任务（R-061 / R-062 / D-088）。
    template <typename F, typename... Args>
    auto try_submit(F&& f, Args&&... args) -> SubmissionResult<typename detail::InvocationTraits<F, Args...>::ResultType> {
        using Traits = detail::InvocationTraits<F, Args...>;

        static_assert(Traits::is_valid,
            "Callable must be invocable as f(args...) or f(std::stop_token, args...)");
        static_assert(!Traits::returns_reference,
            "AstraScheduler tasks cannot return raw references (R-058 / D-074). Wrap in std::reference_wrapper if needed.");
        static_assert(Traits::is_move_constructible,
            "Task result type must be move-constructible (R-058 / D-075).");

        using ResultType = typename Traits::ResultType;

        if (!valid()) {
            throw std::logic_error("operating on empty/moved-from Scheduler");
        }

        const bool is_internal = (detail::current_worker_runtime_id() == runtime_id());

        // try_submit 永不等待 capacity（R-061 / D-088）
        const auto decision = acquire_admission(false /* no block */, is_internal);
        if (decision == detail::AdmissionDecision::Stopping) {
            return SubmissionResult<ResultType>(SubmissionError::Stopping);
        }
        if (decision == detail::AdmissionDecision::Stopped) {
            return SubmissionResult<ResultType>(SubmissionError::Stopped);
        }
        if (decision == detail::AdmissionDecision::CapacityExhausted) {
            return SubmissionResult<ResultType>(SubmissionError::CapacityExhausted);
        }

        // 强异常安全事务：构造过程抛出异常则回滚 slot
        const TaskId tid = detail::allocate_task_id(runtime_id());
        std::shared_ptr<detail::TaskSharedState<ResultType>> state;
        std::unique_ptr<detail::TaskInvokerBase> invoker;

        try {
            state = std::make_shared<detail::TaskSharedState<ResultType>>(tid);
            invoker = detail::make_task_invoker<Traits::is_ordinary_invocable, ResultType>(
                state, std::forward<F>(f), std::forward<Args>(args)...);
        } catch (...) {
            if (!is_internal) {
                rollback_external_slot();
            }
            throw;
        }

        post_task_invoker(std::move(invoker), !is_internal);
        return SubmissionResult<ResultType>(TaskHandle<ResultType>(std::move(state)));
    }

private:
    struct ASTRA_NO_EXPORT Impl;
    std::shared_ptr<Impl> impl_;

    detail::AdmissionDecision acquire_admission(bool block, bool is_internal) const;
    void rollback_external_slot() const;
    void post_task_invoker(std::unique_ptr<detail::TaskInvokerBase> invoker, bool is_external) const;

    friend void detail::run_test_task_on_worker(Scheduler&, std::function<void()>);
    friend std::size_t detail::global_injection_queue_size(const Scheduler&);
    friend std::size_t detail::external_pending_count(const Scheduler&);
    friend void detail::perform_caller_wait(const detail::TaskSharedStateBase&,
                                            std::optional<std::chrono::steady_clock::time_point>);
};

namespace detail {
void run_test_task_on_worker(Scheduler& s, std::function<void()> task);
std::size_t global_injection_queue_size(const Scheduler& s);
std::size_t external_pending_count(const Scheduler& s);
}

}  // namespace astra

#endif  // ASTRA_SCHEDULER_HPP

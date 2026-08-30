#ifndef ASTRA_SRC_GRAPH_RUNTIME_PORT_HPP
#define ASTRA_SRC_GRAPH_RUNTIME_PORT_HPP

// GraphExecution 所需的最小运行时能力面。图模块只依赖这组操作，不再
// 了解 Scheduler::Impl 的字段、队列或同步实现（R-125 / D-177）。

#include <astra/scheduler.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace astra::detail {

class GraphRuntimePort {
public:
    [[nodiscard]] virtual RuntimeId runtime_identity() const noexcept = 0;
    [[nodiscard]] virtual TaskId allocate_graph_task_id() = 0;
    [[nodiscard]] virtual GraphRunId allocate_graph_run_id() = 0;

    [[nodiscard]] virtual AdmissionDecision acquire_graph_slots(
        std::size_t count,
        bool block,
        bool is_internal) = 0;
    virtual void release_graph_slots(std::size_t count) noexcept = 0;

    virtual void post_graph_task(
        std::unique_ptr<TaskInvokerBase> task,
        bool is_external) = 0;
    [[nodiscard]] virtual std::uint64_t register_graph_timer(
        std::chrono::steady_clock::time_point wake_time,
        std::shared_ptr<AwaitHandshake> handshake,
        std::function<void()> resume_action) = 0;
    virtual void cancel_graph_timer(std::uint64_t timer_id) noexcept = 0;

    virtual void record_graph_admission_attempt() noexcept = 0;
    virtual void record_graph_rejected() noexcept = 0;
    virtual void record_graph_started() noexcept = 0;
    virtual void rollback_graph_started(std::size_t task_count) noexcept = 0;

protected:
    virtual ~GraphRuntimePort() = default;
};

// 准入成功后、首个节点发布前的临时所有权。若图物化或准备发布期间抛出，
// 析构统一回滚 active_graph_runs 与外部 pending slots；commit() 后所有权转交
// 给节点终态协议（R-125）。
class GraphAdmissionLease final {
public:
    GraphAdmissionLease(
        GraphRuntimePort& runtime,
        std::size_t slot_count,
        bool is_internal) noexcept
        : runtime_(&runtime), slot_count_(slot_count), is_internal_(is_internal) {}

    GraphAdmissionLease(const GraphAdmissionLease&) = delete;
    GraphAdmissionLease& operator=(const GraphAdmissionLease&) = delete;

    ~GraphAdmissionLease() {
        rollback();
    }

    void commit() noexcept {
        runtime_ = nullptr;
    }

    void rollback() noexcept {
        if (runtime_ == nullptr) {
            return;
        }
        runtime_->rollback_graph_started(slot_count_);
        if (!is_internal_) {
            runtime_->release_graph_slots(slot_count_);
        }
        runtime_ = nullptr;
    }

private:
    GraphRuntimePort* runtime_;
    std::size_t slot_count_;
    bool is_internal_;
};

}  // namespace astra::detail

#endif  // ASTRA_SRC_GRAPH_RUNTIME_PORT_HPP

#include "graph_execution.hpp"

#include "graph_runtime_port.hpp"
#include "graph_shared_state.hpp"

#include <astra/coroutine.hpp>
#include <astra/error.hpp>
#include <astra/task_handle.hpp>

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace astra::detail {

extern thread_local Priority t_current_executing_task_priority;

namespace {

class GraphNodeExecutionContextGuard final {
public:
    explicit GraphNodeExecutionContextGuard(
        GraphRunId new_id,
        Priority new_priority = Priority::Normal) noexcept
        : previous_id_(t_current_executing_graph_run_id),
          previous_priority_(t_current_executing_task_priority) {
        t_current_executing_graph_run_id = new_id;
        t_current_executing_task_priority = new_priority;
    }

    ~GraphNodeExecutionContextGuard() {
        t_current_executing_graph_run_id = previous_id_;
        t_current_executing_task_priority = previous_priority_;
    }

private:
    GraphRunId previous_id_;
    Priority previous_priority_;
};

}  // namespace

struct GraphCoroutineResumeWrapper final : TaskInvokerBase {
    std::unique_ptr<TaskInvokerBase> inner;
    std::shared_ptr<GraphRunSharedState> graph_state;
    std::shared_ptr<TaskHandle<void>::ResultCell> task_state;
    NodeId node_id;
    std::function<void(NodeId)> trigger_fn;

    GraphCoroutineResumeWrapper(
        std::unique_ptr<TaskInvokerBase> in,
        std::shared_ptr<GraphRunSharedState> gs,
        std::shared_ptr<TaskHandle<void>::ResultCell> ts,
        NodeId nid,
        std::function<void(NodeId)> tfn)
        : inner(std::move(in)),
          graph_state(std::move(gs)),
          task_state(std::move(ts)),
          node_id(nid),
          trigger_fn(std::move(tfn)) {}

    void execute() override {
        const Priority priority = task_state ? task_state->priority() : Priority::Normal;
        GraphNodeExecutionContextGuard node_guard(graph_state->id, priority);
        if (inner) {
            inner->execute();
        }
        if (task_state && task_state->is_completed()) {
            const TaskState outcome = task_state->state();
            std::exception_ptr exception =
                outcome == TaskState::Failed ? task_state->exception() : nullptr;
            graph_state->mark_node_terminal(node_id.value(), outcome, std::move(exception));
            if (trigger_fn) {
                trigger_fn(node_id);
            }
        }
    }

    void cancel_pre_start() noexcept override {
        if (inner) {
            inner->cancel_pre_start();
        }
    }

    [[nodiscard]] bool is_resume_segment() const noexcept override {
        return true;
    }

    [[nodiscard]] Priority priority() const noexcept override {
        return task_state ? task_state->priority() : Priority::Normal;
    }
};

GraphExecution::GraphExecution(
    GraphRuntimePort& runtime,
    std::shared_ptr<GraphRunSharedState> state,
    bool is_internal,
    Priority graph_priority) noexcept
    : runtime_(runtime),
      state_(std::move(state)),
      is_internal_(is_internal),
      graph_priority_(graph_priority) {}

GraphRun GraphExecution::run(
    GraphRuntimePort& runtime,
    std::optional<TaskOptions> options,
    FrozenTaskGraph&& graph) {
    runtime.record_graph_admission_attempt();

    const std::size_t node_count = graph.node_count();
    // Identity exhaustion is rejected before reserving admission capacity.
    const GraphRunId graph_run_id = runtime.allocate_graph_run_id();
    const bool is_internal = current_worker_runtime_id() == runtime.runtime_identity();
    const bool is_worker = current_worker_runtime_id() != RuntimeId{0};
    const bool can_block = !is_worker;

    Priority graph_priority = Priority::Normal;
    if (options.has_value()) {
        validate_priority(options->priority);
        graph_priority = options->priority;
    } else if (is_internal) {
        graph_priority = current_executing_task_priority();
    }

    const auto decision = runtime.acquire_graph_slots(node_count, can_block, is_internal);
    if (decision != AdmissionDecision::Success) {
        runtime.record_graph_rejected();
        switch (decision) {
        case AdmissionDecision::Stopping:
            throw submission_rejected(SubmissionError::Stopping);
        case AdmissionDecision::Stopped:
            throw submission_rejected(SubmissionError::Stopped);
        case AdmissionDecision::CapacityExhausted:
            throw submission_rejected(SubmissionError::CapacityExhausted);
        case AdmissionDecision::Success:
            break;
        }
    }

    runtime.record_graph_started();
    GraphAdmissionLease admission_lease(runtime, node_count, is_internal);

    auto state = std::make_shared<GraphRunSharedState>(graph_run_id, node_count);
    if (node_count == 0) {
        state->run_state.store(GraphRunState::Succeeded, std::memory_order_release);
        return GraphRun(std::move(state));
    }

    auto execution = std::shared_ptr<GraphExecution>(
        new GraphExecution(runtime, state, is_internal, graph_priority));
    execution->materialize(graph);

    std::vector<NodeId> roots;
    roots.reserve(node_count);
    for (std::size_t index = 1; index <= node_count; ++index) {
        if (state->node_entries[index].remaining_predecessors.load(
                std::memory_order_relaxed) == 0) {
            roots.push_back(NodeId{index});
        }
    }

    // 从首个 root 发布开始，slot 与 active-run 的回收由节点终态协议接管。
    admission_lease.commit();
    for (NodeId root_id : roots) {
        execution->post_node(root_id);
    }

    return GraphRun(std::move(state));
}

void GraphExecution::materialize(FrozenTaskGraph& graph) {
    for (auto& node_data : graph.nodes_internal()) {
        const std::size_t index = node_data.id.value();
        Priority node_priority = graph_priority_;
        std::optional<TaskDeadline> node_deadline = std::nullopt;
        if (node_data.options.has_value()) {
            validate_priority(node_data.options->priority);
            node_priority = node_data.options->priority;
            node_deadline = node_data.options->deadline;
        }

        auto& entry = state_->node_entries[index];
        entry.id = node_data.id;
        // 普通节点和协程节点共享同一 Runtime TaskId 分配协议。
        entry.task_id = runtime_.allocate_graph_task_id();
        entry.invoker = std::move(node_data.invoker);
        entry.priority = node_priority;
        entry.deadline = node_deadline;

        if (entry.invoker && entry.invoker->is_coroutine_node()) {
            auto* coroutine_node =
                static_cast<GraphCoroutineNodeInvoker*>(entry.invoker.get());
            const TaskId task_id = entry.task_id;
            auto task_state = std::make_shared<TaskHandle<void>::ResultCell>(
                task_id, node_priority, node_deadline);
            task_state->set_timer_functions(
                [runtime = &runtime_](
                    std::chrono::steady_clock::time_point wake_time,
                    std::shared_ptr<AwaitHandshake> handshake,
                    std::function<void()> resume_action) {
                    return runtime->register_graph_timer(
                        wake_time, std::move(handshake), std::move(resume_action));
                },
                [runtime = &runtime_](std::uint64_t timer_id) {
                    runtime->cancel_graph_timer(timer_id);
                });
            coroutine_node->coro.promise().shared_state = task_state;
            coroutine_node->task_state = std::move(task_state);
        }
    }

    for (const auto& edge : graph.edges()) {
        const std::size_t predecessor = edge.from.value();
        const std::size_t successor = edge.to.value();
        state_->node_entries[successor].remaining_predecessors.fetch_add(
            1, std::memory_order_relaxed);
        state_->node_entries[predecessor].successors.push_back({edge.to, edge.policy});
    }
}

void GraphExecution::trigger_successors(NodeId node_id) {
    const std::size_t node_index = node_id.value();
    auto& entry = state_->node_entries[node_index];
    const TaskState outcome = entry.outcome.load(std::memory_order_acquire);

    for (const auto& [successor_id, policy] : entry.successors) {
        auto& successor = state_->node_entries[successor_id.value()];
        if (policy == EdgePolicy::RequireSuccess && outcome != TaskState::Succeeded) {
            successor.has_failed_required_predecessor.store(true, std::memory_order_release);
        }
        if (successor.remaining_predecessors.fetch_sub(1, std::memory_order_acq_rel) != 1) {
            continue;
        }

        if (successor.has_failed_required_predecessor.load(std::memory_order_acquire) ||
            state_->cancel_requested.load(std::memory_order_acquire)) {
            if (!is_internal_) {
                runtime_.release_graph_slots(1);
            }
            state_->mark_node_terminal(successor_id.value(), TaskState::Cancelled);
            trigger_successors(successor_id);
        } else {
            post_node(successor_id);
        }
    }
}

void GraphExecution::post_node(NodeId node_id) {
    const std::size_t node_index = node_id.value();
    auto& entry = state_->node_entries[node_index];
    const Priority node_priority = entry.priority;
    auto self = shared_from_this();

    if (entry.invoker && entry.invoker->is_coroutine_node()) {
        auto* coroutine_node = static_cast<GraphCoroutineNodeInvoker*>(entry.invoker.get());
        auto task_state = coroutine_node->task_state;
        auto coroutine = coroutine_node->coro;
        coroutine_node->coro = nullptr;

        task_state->set_rescheduler(
            [self, task_state, node_id](std::unique_ptr<TaskInvokerBase> invoker) {
                auto trigger = [self](NodeId completed_id) {
                    self->trigger_successors(completed_id);
                };
                auto wrapper = std::make_unique<GraphCoroutineResumeWrapper>(
                    std::move(invoker), self->state_, task_state, node_id, std::move(trigger));
                self->runtime_.post_graph_task(std::move(wrapper), false);
            });

        auto task = [self, task_state, coroutine, node_id, node_priority] {
            const std::size_t index = node_id.value();
            if (self->state_->cancel_requested.load(std::memory_order_acquire)) {
                task_state->request_cancel();
            }

            GraphNodeExecutionContextGuard node_guard(self->state_->id, node_priority);
            auto start_invoker = std::make_unique<CoroutineTaskInvokerModel<void>>(
                coroutine, task_state);
            start_invoker->execute();

            if (task_state->is_completed()) {
                const TaskState outcome = task_state->state();
                std::exception_ptr exception =
                    outcome == TaskState::Failed ? task_state->exception() : nullptr;
                self->state_->mark_node_terminal(index, outcome, std::move(exception));
                self->trigger_successors(node_id);
            }
        };

        runtime_.post_graph_task(
            make_graph_node_invoker<true>(
                std::move(task), node_priority, task_state->deadline()),
            !is_internal_);
        return;
    }

    auto task = [self, node_id, node_priority] {
        const std::size_t index = node_id.value();
        auto& node_entry = self->state_->node_entries[index];

        if (self->state_->cancel_requested.load(std::memory_order_acquire)) {
            self->state_->mark_node_terminal(index, TaskState::Cancelled);
            self->trigger_successors(node_id);
            return;
        }

        GraphNodeExecutionContextGuard node_guard(self->state_->id, node_priority);
        TaskExecutionContextGuard task_guard(node_entry.task_id, node_priority);
        const TaskId metric_id = node_entry.task_id;

        if (node_entry.deadline.has_value() &&
            node_entry.deadline_disposition == DeadlineDisposition::None) {
            const auto now = std::chrono::steady_clock::now();
            if (now <= node_entry.deadline->time_point()) {
                node_entry.deadline_disposition = DeadlineDisposition::Met;
            } else {
                node_entry.deadline_disposition = DeadlineDisposition::Missed;
                const auto lateness = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - node_entry.deadline->time_point()).count();
                record_metrics_deadline_start_lateness(metric_id, lateness);
            }
        }
        record_metrics_first_start(
            metric_id,
            node_entry.deadline.has_value()
                ? std::optional{node_entry.deadline_disposition}
                : std::nullopt);

        const auto execution_started_at = std::chrono::steady_clock::now();
        try {
            if (node_entry.invoker) {
                node_entry.invoker->execute();
            }
            const auto execution_finished_at = std::chrono::steady_clock::now();
            record_metrics_execution_segment(
                metric_id,
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    execution_finished_at - execution_started_at).count());
            record_metrics_succeeded(metric_id);
            self->state_->mark_node_terminal(index, TaskState::Succeeded);
        } catch (const task_cancelled&) {
            const auto execution_finished_at = std::chrono::steady_clock::now();
            record_metrics_execution_segment(
                metric_id,
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    execution_finished_at - execution_started_at).count());
            record_metrics_cancelled_cooperative(metric_id);
            self->state_->mark_node_terminal(index, TaskState::Cancelled);
        } catch (...) {
            const auto execution_finished_at = std::chrono::steady_clock::now();
            record_metrics_execution_segment(
                metric_id,
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    execution_finished_at - execution_started_at).count());
            record_metrics_failed(metric_id);
            self->state_->mark_node_terminal(
                index, TaskState::Failed, std::current_exception());
        }

        self->trigger_successors(node_id);
    };

    runtime_.post_graph_task(
        make_graph_node_invoker<true>(std::move(task), node_priority, entry.deadline),
        !is_internal_);
}

}  // namespace astra::detail

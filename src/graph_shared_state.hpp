#ifndef ASTRA_SRC_GRAPH_SHARED_STATE_HPP
#define ASTRA_SRC_GRAPH_SHARED_STATE_HPP

#include <astra/graph.hpp>
#include <astra/id.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace astra::detail {

extern thread_local GraphRunId t_current_executing_graph_run_id;

class GraphRunSharedState : public std::enable_shared_from_this<GraphRunSharedState> {
public:
    struct NodeEntry {
        NodeId id{};
        std::unique_ptr<TaskInvokerBase> invoker{nullptr};
        Priority priority{Priority::Normal};
        std::optional<TaskDeadline> deadline{std::nullopt};
        DeadlineDisposition deadline_disposition{DeadlineDisposition::None};
        std::atomic<std::size_t> remaining_predecessors{0};
        std::atomic<bool> has_failed_required_predecessor{false};
        std::vector<std::pair<NodeId, EdgePolicy>> successors;
        std::atomic<TaskState> outcome{TaskState::Ready};
    };

    GraphRunId id{};
    std::size_t total_node_count{0};
    std::atomic<std::size_t> remaining_nodes{0};
    std::atomic<GraphRunState> run_state{GraphRunState::Running};
    std::atomic<bool> cancel_requested{false};
    std::atomic<bool> failure_report_observed{false};

    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    GraphReport report;
    std::vector<std::function<void()>> completion_callbacks;

    std::vector<NodeEntry> node_entries;

    explicit GraphRunSharedState(GraphRunId gid, std::size_t n)
        : id(gid), total_node_count(n), remaining_nodes(n), node_entries(n + 1) {
        report.run_id = gid;
        report.total_nodes = n;
    }

    void add_completion_callback(std::function<void()> cb) {
        bool completed = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (remaining_nodes.load(std::memory_order_acquire) == 0) {
                completed = true;
            } else {
                completion_callbacks.push_back(std::move(cb));
            }
        }
        if (completed && cb) {
            cb();
        }
    }

    void mark_node_terminal(std::size_t node_idx, TaskState outcome, std::exception_ptr ex = nullptr) {
        node_entries[node_idx].outcome.store(outcome, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (outcome == TaskState::Succeeded) {
                report.succeeded_nodes++;
            } else if (outcome == TaskState::Failed) {
                report.failed_nodes++;
                if (ex) {
                    report.failed_node_exceptions.emplace_back(NodeId{node_idx}, std::move(ex));
                }
            } else if (outcome == TaskState::Cancelled) {
                report.cancelled_nodes++;
            }
        }

        if (remaining_nodes.fetch_sub(1) == 1) {
            // 所有 Node 已处于 Terminal 状态（D-112）
            std::vector<std::function<void()>> cbs;
            {
                std::lock_guard<std::mutex> lock(mutex);
                std::sort(report.failed_node_exceptions.begin(), report.failed_node_exceptions.end(),
                          [](const auto& a, const auto& b) {
                              return a.first < b.first;
                          });

                if (report.failed_nodes > 0) {
                    run_state.store(GraphRunState::Failed, std::memory_order_release);
                } else if (report.cancelled_nodes > 0) {
                    run_state.store(GraphRunState::Cancelled, std::memory_order_release);
                } else {
                    run_state.store(GraphRunState::Succeeded, std::memory_order_release);
                }
                cbs = std::move(completion_callbacks);
            }
            cv.notify_all();
            for (auto& cb : cbs) {
                if (cb) {
                    cb();
                }
            }
        }
    }
};

void perform_graph_caller_wait(const GraphRunSharedState& target,
                               std::optional<std::chrono::steady_clock::time_point> deadline);

}  // namespace astra::detail

#endif  // ASTRA_SRC_GRAPH_SHARED_STATE_HPP

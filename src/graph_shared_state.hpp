#ifndef ASTRA_SRC_GRAPH_SHARED_STATE_HPP
#define ASTRA_SRC_GRAPH_SHARED_STATE_HPP

#include <astra/graph.hpp>
#include <astra/id.hpp>

#include <atomic>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <vector>

namespace astra::detail {

class GraphRunSharedState : public std::enable_shared_from_this<GraphRunSharedState> {
public:
    struct NodeEntry {
        NodeId id{};
        std::unique_ptr<TaskInvokerBase> invoker{nullptr};
        std::atomic<std::size_t> remaining_predecessors{0};
        std::vector<std::pair<NodeId, EdgePolicy>> successors;
        std::atomic<TaskState> outcome{TaskState::Ready};
    };

    GraphRunId id{};
    std::size_t total_node_count{0};
    std::atomic<std::size_t> remaining_nodes{0};
    std::atomic<GraphRunState> run_state{GraphRunState::Running};
    std::atomic<bool> cancel_requested{false};

    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    GraphReport report;

    std::vector<NodeEntry> node_entries;

    explicit GraphRunSharedState(GraphRunId gid, std::size_t n)
        : id(gid), total_node_count(n), remaining_nodes(n), node_entries(n + 1) {
        report.total_nodes = n;
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
            // 所有 Node 已处于 Terminal 状态
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (report.failed_nodes > 0) {
                    run_state.store(GraphRunState::Failed, std::memory_order_release);
                } else if (report.cancelled_nodes > 0) {
                    run_state.store(GraphRunState::Cancelled, std::memory_order_release);
                } else {
                    run_state.store(GraphRunState::Succeeded, std::memory_order_release);
                }
            }
            cv.notify_all();
        }
    }
};

}  // namespace astra::detail

#endif  // ASTRA_SRC_GRAPH_SHARED_STATE_HPP

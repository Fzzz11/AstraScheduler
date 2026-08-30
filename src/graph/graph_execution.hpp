#ifndef ASTRA_SRC_GRAPH_EXECUTION_HPP
#define ASTRA_SRC_GRAPH_EXECUTION_HPP

// Frozen graph 的一次运行协议。该类型只存在于实现目录，统一拥有依赖传播、
// node publication、取消和 terminal report 的状态机入口。

#include <astra/graph.hpp>

#include <memory>
#include <optional>

namespace astra {

namespace detail {

class GraphRuntimePort;
class GraphRunSharedState;

class GraphExecution final : public std::enable_shared_from_this<GraphExecution> {
public:
    static GraphRun run(
        GraphRuntimePort& runtime,
        std::optional<TaskOptions> options,
        FrozenTaskGraph&& graph);

private:
    GraphExecution(
        GraphRuntimePort& runtime,
        std::shared_ptr<GraphRunSharedState> state,
        bool is_internal,
        Priority graph_priority) noexcept;

    void materialize(FrozenTaskGraph& graph);
    void trigger_successors(NodeId node_id);
    void post_node(NodeId node_id);

    GraphRuntimePort& runtime_;
    std::shared_ptr<GraphRunSharedState> state_;
    bool is_internal_;
    Priority graph_priority_;
};

}  // namespace detail
}  // namespace astra

#endif  // ASTRA_SRC_GRAPH_EXECUTION_HPP

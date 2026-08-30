#ifndef ASTRA_SRC_GRAPH_EXECUTION_HPP
#define ASTRA_SRC_GRAPH_EXECUTION_HPP

// Frozen graph 的一次运行协议。该类型只存在于实现目录，统一拥有依赖传播、
// node publication、取消和 terminal report 的状态机入口。

#include <astra/graph.hpp>

#include <optional>

namespace astra {

class Scheduler;

namespace detail {

class GraphExecution final {
public:
    GraphExecution() = delete;

    static GraphRun run(
        Scheduler& scheduler,
        std::optional<TaskOptions> options,
        FrozenTaskGraph&& graph);
};

}  // namespace detail
}  // namespace astra

#endif  // ASTRA_SRC_GRAPH_EXECUTION_HPP

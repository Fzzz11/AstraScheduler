#ifndef ASTRA_GRAPH_HPP
#define ASTRA_GRAPH_HPP

#include <astra/error.hpp>
#include <astra/export.hpp>
#include <astra/id.hpp>
#include <astra/task_handle.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace astra {

// 前向声明
class Scheduler;
class FrozenTaskGraph;

namespace detail {
template <typename F>
struct GraphTaskInvoker final : TaskInvokerBase {
    F func;

    explicit GraphTaskInvoker(F&& f) : func(std::move(f)) {}
    explicit GraphTaskInvoker(const F& f) : func(f) {}

    void execute() override {
        func();
    }

    void cancel_pre_start() noexcept override {}
};
}  // namespace detail

// 边触发策略（R-071 / D-109）。
enum class EdgePolicy : std::uint8_t {
    // 仅在前置节点成功完成时触发后继节点
    RequireSuccess = 1,
    // 无论前置节点成功、失败还是取消，均触发后继节点
    AfterCompletion = 2,
};

// 任务图边定义
struct GraphEdge {
    NodeId from{};
    NodeId to{};
    EdgePolicy policy{EdgePolicy::RequireSuccess};

    friend constexpr auto operator<=>(const GraphEdge&, const GraphEdge&) = default;
    friend constexpr bool operator==(const GraphEdge&, const GraphEdge&) = default;
};

// -----------------------------------------------------------------------------
// FrozenTaskGraph (R-069 / D-104)
// 不可变、单次执行的已校验任务图快照
// -----------------------------------------------------------------------------
class ASTRA_EXPORT FrozenTaskGraph {
public:
    struct NodeData {
        NodeId id{};
        std::unique_ptr<detail::TaskInvokerBase> invoker;
    };

    FrozenTaskGraph() noexcept = default;
    ~FrozenTaskGraph() = default;

    FrozenTaskGraph(FrozenTaskGraph&&) noexcept = default;
    FrozenTaskGraph& operator=(FrozenTaskGraph&&) noexcept = default;

    FrozenTaskGraph(const FrozenTaskGraph&) = delete;
    FrozenTaskGraph& operator=(const FrozenTaskGraph&) = delete;

    [[nodiscard]] std::size_t node_count() const noexcept {
        return nodes_.size();
    }

    [[nodiscard]] std::size_t edge_count() const noexcept {
        return edges_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return nodes_.empty();
    }

    [[nodiscard]] const std::vector<GraphEdge>& edges() const noexcept {
        return edges_;
    }

    [[nodiscard]] std::vector<NodeData>& nodes_internal() noexcept {
        return nodes_;
    }

private:
    friend class TaskGraph;
    friend class Scheduler;

    explicit FrozenTaskGraph(std::vector<NodeData> nodes, std::vector<GraphEdge> edges)
        : nodes_(std::move(nodes)), edges_(std::move(edges)) {}

    std::vector<NodeData> nodes_;
    std::vector<GraphEdge> edges_;
};

// -----------------------------------------------------------------------------
// TaskGraph (R-069 / D-104 / D-105 / D-161)
// 串行构建的可移动 Builder，经 consuming freeze() 校验并形成 FrozenTaskGraph
// -----------------------------------------------------------------------------
class ASTRA_EXPORT TaskGraph {
public:
    TaskGraph() = default;
    ~TaskGraph() = default;

    TaskGraph(TaskGraph&&) noexcept = default;
    TaskGraph& operator=(TaskGraph&&) noexcept = default;

    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;

    // 添加任务节点，返回 graph-local 强类型 NodeId（D-161）
    template <typename F>
    NodeId emplace(F&& f) {
        using DecayedF = std::decay_t<F>;
        static_assert(std::is_invocable_v<DecayedF>, "Callable must be invocable with ()");

        const std::uint64_t seq = nodes_.size() + 1;
        const NodeId id{seq};
        nodes_.push_back(FrozenTaskGraph::NodeData{
            id,
            std::make_unique<detail::GraphTaskInvoker<DecayedF>>(std::forward<F>(f))
        });
        return id;
    }

    // 添加有向边 (from -> to)
    void add_edge(NodeId from, NodeId to, EdgePolicy policy = EdgePolicy::RequireSuccess) {
        edges_.push_back(GraphEdge{from, to, policy});
    }

    [[nodiscard]] std::size_t node_count() const noexcept {
        return nodes_.size();
    }

    [[nodiscard]] std::size_t edge_count() const noexcept {
        return edges_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return nodes_.empty();
    }

    // Consuming freeze (R-069 / D-104 / D-105):
    // 校验节点有效性、自环、重复边与环路，产生不可变 FrozenTaskGraph
    FrozenTaskGraph freeze() &&;

private:
    std::vector<FrozenTaskGraph::NodeData> nodes_;
    std::vector<GraphEdge> edges_;
};

// 任务图运行状态（D-112）。
enum class GraphRunState : std::uint8_t {
    Running = 1,
    Succeeded = 2,
    Failed = 3,
    Cancelled = 4,
};

// 任务图等待结果（D-113）。
enum class GraphWaitResult : std::uint8_t {
    Completed = 1,
    TimedOut = 2,
};

// 任务图聚合执行报告（D-112）。
class GraphReport {
public:
    std::size_t total_nodes{0};
    std::size_t succeeded_nodes{0};
    std::size_t failed_nodes{0};
    std::size_t cancelled_nodes{0};
    std::vector<std::pair<NodeId, std::exception_ptr>> failed_node_exceptions;
};

namespace detail {
class GraphRunSharedState;
}

// -----------------------------------------------------------------------------
// GraphRun (R-070 / D-104 / D-112 / D-113)
// 任务图执行实例 Handle，支持多副本共享观察与状态等待
// -----------------------------------------------------------------------------
class ASTRA_EXPORT GraphRun {
public:
    GraphRun() noexcept = default;
    ~GraphRun() = default;

    GraphRun(const GraphRun&) noexcept = default;
    GraphRun& operator=(const GraphRun&) noexcept = default;
    GraphRun(GraphRun&&) noexcept = default;
    GraphRun& operator=(GraphRun&&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid();
    }

    [[nodiscard]] GraphRunId id() const noexcept;
    [[nodiscard]] std::size_t node_count() const noexcept;
    [[nodiscard]] GraphRunState state() const;
    [[nodiscard]] bool is_completed() const;

    void wait() const;
    [[nodiscard]] GraphWaitResult wait_for(std::chrono::nanoseconds timeout) const;

    [[nodiscard]] const GraphReport& get_report() const &;
    const GraphReport& get_report() const && = delete;

    void request_cancel() const noexcept;

private:
    friend class Scheduler;
    explicit GraphRun(std::shared_ptr<detail::GraphRunSharedState> state)
        : state_(std::move(state)) {}

    std::shared_ptr<detail::GraphRunSharedState> state_{nullptr};
};

}  // namespace astra

#endif  // ASTRA_GRAPH_HPP

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
template <typename T>
class Task;

namespace detail {
class GraphExecution;
template <bool Ordinary, typename F>
struct GraphTaskInvokerModel;

template <typename F>
struct GraphTaskInvokerModel<true, F> final : TaskInvokerBase {
    F func;
    Priority priority_{Priority::Normal};
    std::optional<TaskDeadline> deadline_{std::nullopt};

    explicit GraphTaskInvokerModel(F&& f, Priority p = Priority::Normal, std::optional<TaskDeadline> dl = std::nullopt)
        : func(std::move(f)), priority_(p), deadline_(dl) {}
    explicit GraphTaskInvokerModel(const F& f, Priority p = Priority::Normal, std::optional<TaskDeadline> dl = std::nullopt)
        : func(f), priority_(p), deadline_(dl) {}

    void execute() override {
        func();
    }

    void cancel_pre_start() noexcept override {}

    [[nodiscard]] Priority priority() const noexcept override {
        return priority_;
    }

    [[nodiscard]] std::optional<TaskDeadline> deadline() const noexcept override {
        return deadline_;
    }
};

template <typename F>
struct GraphTaskInvokerModel<false, F> final : TaskInvokerBase {
    F func;
    Priority priority_{Priority::Normal};
    std::optional<TaskDeadline> deadline_{std::nullopt};
    std::stop_source stop_source;

    explicit GraphTaskInvokerModel(F&& f, Priority p = Priority::Normal, std::optional<TaskDeadline> dl = std::nullopt)
        : func(std::move(f)), priority_(p), deadline_(dl) {}
    explicit GraphTaskInvokerModel(const F& f, Priority p = Priority::Normal, std::optional<TaskDeadline> dl = std::nullopt)
        : func(f), priority_(p), deadline_(dl) {}

    void execute() override {
        func(stop_source.get_token());
    }

    void cancel_pre_start() noexcept override {
        stop_source.request_stop();
    }

    [[nodiscard]] Priority priority() const noexcept override {
        return priority_;
    }

    [[nodiscard]] std::optional<TaskDeadline> deadline() const noexcept override {
        return deadline_;
    }
};

template <bool Ordinary, typename F>
inline std::unique_ptr<TaskInvokerBase> make_graph_node_invoker(
    F&& f,
    Priority p = Priority::Normal,
    std::optional<TaskDeadline> dl = std::nullopt) {
    using DecayedF = std::decay_t<F>;
    return std::make_unique<GraphTaskInvokerModel<Ordinary, DecayedF>>(
        std::forward<F>(f), p, dl);
}
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
private:
    struct NodeData {
        NodeId id{};
        std::unique_ptr<detail::TaskInvokerBase> invoker{nullptr};
        std::optional<TaskOptions> options{std::nullopt};
    };

public:
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

private:
    friend class TaskGraph;
    friend class Scheduler;
    friend class detail::GraphExecution;

    // 内部访问接口（转移 NodeData 所有权至 GraphRunSharedState）
    [[nodiscard]] std::vector<NodeData>& nodes_internal() noexcept {
        return nodes_;
    }

    explicit FrozenTaskGraph(std::vector<NodeData> nodes, std::vector<GraphEdge> edges)
        : nodes_(std::move(nodes)), edges_(std::move(edges)) {}

    std::vector<NodeData> nodes_;
    std::vector<GraphEdge> edges_;
};

// -----------------------------------------------------------------------------
// TaskGraph (R-069 / D-104 / D-105 / D-108 / D-161)
// 任务图构建器，支持移动语义的 Callable 与拓扑验证
// -----------------------------------------------------------------------------
class ASTRA_EXPORT TaskGraph {
public:
    TaskGraph() = default;
    ~TaskGraph() = default;

    TaskGraph(TaskGraph&&) noexcept = default;
    TaskGraph& operator=(TaskGraph&&) noexcept = default;

    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;

    // 添加任务节点，返回 graph-local 强类型 NodeId（D-108 / D-161）
    // 约束 Callable 返回类型必须恰好为 void（R-071 / D-108）
    template <typename F>
        requires (!std::is_same_v<std::remove_cvref_t<F>, TaskOptions>)
    NodeId emplace(F&& f) {
        using Traits = detail::InvocationTraits<F>;
        static_assert(Traits::is_valid,
            "Callable must be invocable as f() or f(std::stop_token)");
        static_assert(std::is_void_v<typename Traits::ResultType>,
            "TaskGraph Node Callable must return void (R-071 / D-108)");

        const std::uint64_t seq = nodes_.size() + 1;
        const NodeId id{seq};
        nodes_.push_back(FrozenTaskGraph::NodeData{
            id,
            detail::make_graph_node_invoker<Traits::is_ordinary_invocable>(std::forward<F>(f)),
            std::nullopt
        });
        return id;
    }

    template <typename F>
    NodeId emplace(TaskOptions options, F&& f) {
        validate_priority(options.priority);
        using Traits = detail::InvocationTraits<F>;
        static_assert(Traits::is_valid,
            "Callable must be invocable as f() or f(std::stop_token)");
        static_assert(std::is_void_v<typename Traits::ResultType>,
            "TaskGraph Node Callable must return void (R-071 / D-108)");

        const std::uint64_t seq = nodes_.size() + 1;
        const NodeId id{seq};
        nodes_.push_back(FrozenTaskGraph::NodeData{
            id,
            detail::make_graph_node_invoker<Traits::is_ordinary_invocable>(std::forward<F>(f)),
            options
        });
        return id;
    }

    // R-077 / R-080 / D-123 / D-129: 显式添加 Task<void> 协程节点
    NodeId emplace_coroutine(Task<void>&& task);
    NodeId emplace_coroutine(TaskOptions options, Task<void>&& task);

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

class GraphReport {
public:
    GraphRunId run_id{};
    std::size_t total_nodes{0};
    std::size_t succeeded_nodes{0};
    std::size_t failed_nodes{0};
    std::size_t cancelled_nodes{0};
    std::vector<std::pair<NodeId, std::exception_ptr>> failed_node_exceptions;
};

namespace detail {
class GraphRunSharedState;
struct GraphRunAwaiter;
ASTRA_EXPORT GraphRunId current_executing_graph_run_id() noexcept;
}

// -----------------------------------------------------------------------------
// GraphRun (R-070 / R-072 / D-104 / D-111 / D-112 / D-113 / D-152)
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

    [[nodiscard]] GraphRunId id() const;
    [[nodiscard]] std::size_t node_count() const;
    [[nodiscard]] GraphRunState state() const;
    [[nodiscard]] bool is_completed() const;

    void wait() const;
    [[nodiscard]] GraphWaitResult wait_for(std::chrono::nanoseconds timeout) const;

    template <typename Rep, typename Period>
    [[nodiscard]] GraphWaitResult wait_for(const std::chrono::duration<Rep, Period>& timeout) const {
        return wait_for(std::chrono::duration_cast<std::chrono::nanoseconds>(timeout));
    }

    [[nodiscard]] const GraphReport& get_report() const &;
    const GraphReport& get_report() const && = delete;

    void request_cancel() const noexcept;

    // R-076 / D-121: co_await 左值 GraphRun（rvalue deleted）
    [[nodiscard]] detail::GraphRunAwaiter operator co_await() const &;
    void operator co_await() const && = delete;
    void operator co_await() && = delete;

private:
    friend class Scheduler;
    friend class detail::GraphExecution;
    friend struct detail::GraphRunAwaiter;

    void add_completion_callback_internal(std::function<void()> cb) const;

    explicit GraphRun(std::shared_ptr<detail::GraphRunSharedState> state)
        : state_(std::move(state)) {}

    std::shared_ptr<detail::GraphRunSharedState> state_{nullptr};
};

}  // namespace astra

#endif  // ASTRA_GRAPH_HPP

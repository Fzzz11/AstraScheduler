#ifndef ASTRA_GRAPH_HPP
#define ASTRA_GRAPH_HPP

#include <astra/error.hpp>
#include <astra/export.hpp>
#include <astra/id.hpp>
#include <astra/task_handle.hpp>

// ============================================================================
// TaskGraph —— 把一组有依赖关系的任务组织成 DAG（有向无环图）一起调度。
//
// 【这是什么】
//   你用 graph.emplace(函数) 添加节点，用 graph.add_edge(前, 后) 声明
//   "前成功后才能跑后"的依赖，然后 freeze() 冻结、scheduler.run() 提交。
//   返回的 GraphRun 是这次运行的句柄：可 wait/get 报告、可取消。
//
// 【为什么先 freeze 再运行】
//   图结构在运行期间被多个线程并发读取（每个节点完成时都要解锁后继）。
//   冻结（consuming freeze）把构建期的可变图一次性变成不可变的运行期
//   结构，此后零锁读取——构建时的灵活换来运行时的无竞争。
//
// 【关键语义】
//   - 节点的执行体可以抛异常；默认边策略下，前置失败则后继被取消。
//   - GraphReport 记录每个失败节点的异常，便于事后诊断。
//   - 协程也能作为节点（emplace_coroutine），与普通任务同一套调度。
// ============================================================================

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

// 前置节点到达终态后是否投放后继（R-071 / D-109）。
/** @brief 前置节点完成后是否允许投放后继节点。 */
enum class EdgePolicy : std::uint8_t {
    // 仅前置 Succeeded 时投放后继；失败/取消则后继被取消。
    RequireSuccess = 1,
    // 前置到达任一终态都投放后继。
    AfterCompletion = 2,
};

// 一条有向依赖边。from/to 必须是同一 TaskGraph 内的 NodeId。
/** @brief TaskGraph 中的一条有向依赖边。 */
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
// 已校验、不可变、仅可移动的任务图。由 TaskGraph::freeze() 产出，交给 Scheduler::run() 消耗。
/** @brief freeze() 产出的已校验、不可变、仅可移动任务图。 */
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

    // 节点数。默认构造为空图。
    [[nodiscard]] std::size_t node_count() const noexcept {
        return nodes_.size();
    }

    [[nodiscard]] std::size_t edge_count() const noexcept {
        return edges_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return nodes_.empty();
    }

    // 已校验边列表。不包含节点可调用对象。
    [[nodiscard]] const std::vector<GraphEdge>& edges() const noexcept {
        return edges_;
    }

private:
    friend class TaskGraph;
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
// 可变构建器。仅可移动；freeze() 消耗 *this 并校验结构。
/**
 * @brief 可变 TaskGraph 构建器。
 * @note freeze() 会消耗构建器并校验节点、边和环路。
 */
class ASTRA_EXPORT TaskGraph {
public:
    TaskGraph() = default;
    ~TaskGraph() = default;

    TaskGraph(TaskGraph&&) noexcept = default;
    TaskGraph& operator=(TaskGraph&&) noexcept = default;

    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;

    // 追加节点，返回 graph-local NodeId（从 1 起按插入顺序）。
    // Callable 须为 f() 或 f(stop_token)，返回类型必须是 void。
    // 非法 Priority 抛 invalid_argument（R-071 / D-108 / D-161）。
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

    // 追加 Task<void> 协程节点并消耗 Task。空/无效 Task 抛 logic_error（R-077）。
    NodeId emplace_coroutine(Task<void>&& task);
    NodeId emplace_coroutine(TaskOptions options, Task<void>&& task);

    // 记录有向边。结构错误（外节点/自环/重复/环）在 freeze() 时抛 graph_validation_error。
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

    // 消耗构建器，校验外节点/自环/重复边/环后返回不可变图。
    // 失败抛 graph_validation_error；环路时 cycle_witness() 给出一条环（R-069）。
    FrozenTaskGraph freeze() &&;

private:
    std::vector<FrozenTaskGraph::NodeData> nodes_;
    std::vector<GraphEdge> edges_;
};

// 一次图执行的聚合终态。任一节点失败则为 Failed；全部取消则为 Cancelled（D-112）。
/** @brief 一次 GraphRun 的聚合生命周期状态。 */
enum class GraphRunState : std::uint8_t {
    Running = 1,
    Succeeded = 2,
    Failed = 3,
    Cancelled = 4,
};

// GraphRun::wait_for 的返回。TimedOut 不取消图（D-113）。
/** @brief GraphRun::wait_for() 的完成或超时结果。 */
enum class GraphWaitResult : std::uint8_t {
    Completed = 1,
    TimedOut = 2,
};

// 一次 GraphRun 的终态报告。failed_node_exceptions 仅含失败节点，不含取消节点。
/** @brief 一次 GraphRun 的节点计数和失败异常报告。 */
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
struct AwaitProtocolAccess;
ASTRA_EXPORT GraphRunId current_executing_graph_run_id() noexcept;
}

// -----------------------------------------------------------------------------
// GraphRun (R-070 / R-072 / D-104 / D-111 / D-112 / D-113 / D-152)
// 任务图执行实例 Handle，支持多副本共享观察与状态等待
// -----------------------------------------------------------------------------
// 一次图执行的共享观察句柄。可复制；空/moved-from 的 valid() 为 false。
/**
 * @brief 一次任务图执行的共享观察句柄。
 * @note 句柄可复制；wait/get_report 会观察同一次运行。
 */
class ASTRA_EXPORT GraphRun {
public:
    GraphRun() noexcept = default;
    ~GraphRun() = default;

    GraphRun(const GraphRun&) noexcept = default;
    GraphRun& operator=(const GraphRun&) noexcept = default;
    GraphRun(GraphRun&&) noexcept = default;
    GraphRun& operator=(GraphRun&&) noexcept = default;

    // 是否关联一次执行。空句柄返回 false，不抛。
    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid();
    }

    // 下列查询在空句柄上抛 logic_error。
    [[nodiscard]] GraphRunId id() const;
    [[nodiscard]] std::size_t node_count() const;
    [[nodiscard]] GraphRunState state() const;
    [[nodiscard]] bool is_completed() const;

    // 阻塞直到图到达终态。空句柄抛 logic_error。
    // 节点执行中等待本 GraphRun 抛 logic_error。Worker 可 helping（R-072）。
    void wait() const;
    [[nodiscard]] GraphWaitResult wait_for(std::chrono::nanoseconds timeout) const;

    // 有界等待。到期返回 TimedOut，图继续跑。空句柄 / 自等待规则同 wait()。
    template <typename Rep, typename Period>
    [[nodiscard]] GraphWaitResult wait_for(const std::chrono::duration<Rep, Period>& timeout) const {
        return wait_for(std::chrono::duration_cast<std::chrono::nanoseconds>(timeout));
    }

    // 先 wait() 再返回报告。仅左值；rvalue 被 delete。空句柄抛 logic_error。
    [[nodiscard]] const GraphReport& get_report() const &;
    const GraphReport& get_report() const && = delete;

    // 请求取消未开始节点。空句柄 no-op。已运行节点靠自身响应取消。
    void request_cancel() const noexcept;

    // 在 Astra 协程内等待本图。仅左值；空句柄抛 logic_error（R-076）。
    [[nodiscard]] detail::GraphRunAwaiter operator co_await() const &;
    void operator co_await() const && = delete;
    void operator co_await() && = delete;

private:
    friend class detail::GraphExecution;
    friend struct detail::GraphRunAwaiter;
    friend struct detail::AwaitProtocolAccess;

    ASTRA_NO_EXPORT void add_completion_callback_internal(std::function<void()> cb) const;

    explicit GraphRun(std::shared_ptr<detail::GraphRunSharedState> state)
        : state_(std::move(state)) {}

    std::shared_ptr<detail::GraphRunSharedState> state_{nullptr};
};

}  // namespace astra

#endif  // ASTRA_GRAPH_HPP

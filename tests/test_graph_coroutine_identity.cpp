#include "astra/coroutine.hpp"
#include "astra/graph.hpp"
#include "astra/scheduler.hpp"
#include "astra/task_handle.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <concepts>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// 1. Multi-suspension coroutine node: verifies single GraphRunId + TaskId identity (R-077)
// -----------------------------------------------------------------------------
astra::Task<void> coro_graph_node_identity(
    std::atomic<int>& step,
    std::vector<astra::TaskId>& observed_task_ids,
    std::vector<astra::GraphRunId>& observed_graph_ids,
    std::mutex& mtx) {

    {
        std::lock_guard<std::mutex> lock(mtx);
        observed_task_ids.push_back(astra::detail::current_executing_task_id());
        observed_graph_ids.push_back(astra::detail::current_executing_graph_run_id());
    }
    step.store(1, std::memory_order_release);

    co_await astra::yield();

    {
        std::lock_guard<std::mutex> lock(mtx);
        observed_task_ids.push_back(astra::detail::current_executing_task_id());
        observed_graph_ids.push_back(astra::detail::current_executing_graph_run_id());
    }
    step.store(2, std::memory_order_release);

    co_await astra::yield();

    {
        std::lock_guard<std::mutex> lock(mtx);
        observed_task_ids.push_back(astra::detail::current_executing_task_id());
        observed_graph_ids.push_back(astra::detail::current_executing_graph_run_id());
    }
    step.store(3, std::memory_order_release);

    co_return;
}

void test_R077_graph_coroutine_node_identity() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    astra::TaskGraph graph;
    std::atomic<int> step{0};
    std::vector<astra::TaskId> observed_task_ids;
    std::vector<astra::GraphRunId> observed_graph_ids;
    std::mutex mtx;

    auto n1 = graph.emplace_coroutine(coro_graph_node_identity(
        step, observed_task_ids, observed_graph_ids, mtx));

    std::atomic<bool> n2_ran{false};
    auto n2 = graph.emplace([&] {
        n2_ran.store(true, std::memory_order_release);
    });

    graph.add_edge(n1, n2, astra::EdgePolicy::RequireSuccess);

    auto frozen = std::move(graph).freeze();
    auto run = sched.run(std::move(frozen));

    run.wait();
    const auto& report = run.get_report();

    assert(report.total_nodes == 2);
    assert(report.succeeded_nodes == 2);
    assert(report.failed_nodes == 0);
    assert(report.cancelled_nodes == 0);
    assert(step.load() == 3);
    assert(n2_ran.load());

    // 验证全生命周期中 TaskId 与 GraphRunId 保持稳定同一
    assert(observed_task_ids.size() == 3);
    assert(observed_task_ids[0] != astra::TaskId{});
    assert(observed_task_ids[0] == observed_task_ids[1]);
    assert(observed_task_ids[1] == observed_task_ids[2]);

    assert(observed_graph_ids.size() == 3);
    assert(observed_graph_ids[0] == run.id());
    assert(observed_graph_ids[0] == observed_graph_ids[1]);
    assert(observed_graph_ids[1] == observed_graph_ids[2]);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 2. Coroutine Graph Node awaiting TaskHandle (cross-task composition)
// -----------------------------------------------------------------------------
astra::Task<void> coro_graph_node_await_task(
    astra::TaskHandle<int>& target_handle,
    std::atomic<int>& result_out) {

    int val = co_await target_handle;
    result_out.store(val * 10, std::memory_order_release);
    co_return;
}

void test_R077_graph_coroutine_node_await_task() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    auto target = sched.submit([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 7;
    });

    std::atomic<int> result{0};
    astra::TaskGraph graph;
    auto n1 = graph.emplace_coroutine(coro_graph_node_await_task(target, result));

    std::atomic<int> downstream_seen{0};
    auto n2 = graph.emplace([&] {
        downstream_seen.store(result.load(), std::memory_order_release);
    });

    graph.add_edge(n1, n2);

    auto run = sched.run(std::move(graph).freeze());
    run.wait();

    assert(run.state() == astra::GraphRunState::Succeeded);
    assert(result.load() == 70);
    assert(downstream_seen.load() == 70);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 3. Coroutine Graph Node Failure & Edge Policy propagation
// -----------------------------------------------------------------------------
astra::Task<void> coro_failing_node() {
    co_await astra::yield();
    throw std::runtime_error("coroutine node failed!");
    co_return;
}

void test_R077_coroutine_node_failure_propagation() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    astra::TaskGraph graph;
    auto n1 = graph.emplace_coroutine(coro_failing_node());

    std::atomic<bool> req_succ_ran{false};
    auto n2 = graph.emplace([&] { req_succ_ran.store(true); });
    graph.add_edge(n1, n2, astra::EdgePolicy::RequireSuccess);

    std::atomic<bool> after_comp_ran{false};
    auto n3 = graph.emplace([&] { after_comp_ran.store(true); });
    graph.add_edge(n1, n3, astra::EdgePolicy::AfterCompletion);

    auto run = sched.run(std::move(graph).freeze());
    run.wait();

    const auto& report = run.get_report();
    assert(run.state() == astra::GraphRunState::Failed);
    assert(report.total_nodes == 3);
    assert(report.failed_nodes == 1);
    assert(report.cancelled_nodes == 1); // n2 cancelled due to failed predecessor
    assert(report.succeeded_nodes == 1); // n3 succeeded
    assert(!req_succ_ran.load());
    assert(after_comp_ran.load());

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 4. GraphRun cancellation while coroutine node is suspended
// -----------------------------------------------------------------------------
astra::Task<void> coro_suspended_cancel_node(std::atomic<bool>& started, astra::TaskHandle<int>& target) {
    started.store(true, std::memory_order_release);
    // 等待一个永远不会完成的 target
    co_await target;
    co_return;
}

void test_R077_graph_cancel_while_coroutine_suspended() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    // target 响应取消信号，避免固定 10 秒超时
    auto never_completed_target = sched.submit([](std::stop_token st) -> int {
        while (!st.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return 1;
    });

    std::atomic<bool> started{false};
    astra::TaskGraph graph;
    auto n1 = graph.emplace_coroutine(coro_suspended_cancel_node(started, never_completed_target));

    auto run = sched.run(std::move(graph).freeze());

    // 等待协程节点开始并进入挂起状态
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // 取消整个 GraphRun
    run.request_cancel();
    run.wait();

    assert(run.state() == astra::GraphRunState::Cancelled);
    const auto& report = run.get_report();
    assert(report.cancelled_nodes == 1);

    never_completed_target.request_cancel();
    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 5. Compile-time constraints (R-077 / D-123 / D-124)
// -----------------------------------------------------------------------------
template <typename T>
concept CanEmplaceCoroutine = requires(astra::TaskGraph g, astra::Task<T> t) {
    g.emplace_coroutine(std::move(t));
};

void test_R077_compile_constraints() {
    // 1. TaskGraph::emplace_coroutine 仅接受 Task<void>&&
    static_assert(CanEmplaceCoroutine<void>);

    // 2. Task<int> 不得传入 emplace_coroutine
    static_assert(!CanEmplaceCoroutine<int>);
}

}  // namespace

int main() {
    std::cout << "Running astra_graph_coroutine_identity_test..." << std::endl;

    test_R077_graph_coroutine_node_identity();
    test_R077_graph_coroutine_node_await_task();
    test_R077_coroutine_node_failure_propagation();
    test_R077_graph_cancel_while_coroutine_suspended();
    test_R077_compile_constraints();

    std::cout << "All AST-036 graph coroutine identity tests passed successfully!" << std::endl;
    return 0;
}

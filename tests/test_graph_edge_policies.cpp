#include <astra/error.hpp>
#include <astra/graph.hpp>
#include <astra/scheduler.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond,     \
                         __FILE__, __LINE__);                                  \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

namespace {

// 1. 编译期验证：void 返回类型与 stop_token 支持（D-108）
void test_R071_void_node_compile_time_contract() {
    astra::TaskGraph graph;

    // 普通 void() callable
    auto n1 = graph.emplace([] {});
    (void)n1;

    // 支持 std::stop_token 形参的 void(std::stop_token) callable
    auto n2 = graph.emplace([](std::stop_token st) {
        (void)st.stop_requested();
    });
    (void)n2;

    static_assert(std::is_same_v<decltype(n1), astra::NodeId>);
    static_assert(std::is_same_v<decltype(n2), astra::NodeId>);
}

// 2. RequireSuccess 正常成功路径
void test_R071_require_success_pipeline() {
    astra::Scheduler scheduler;
    astra::TaskGraph graph;

    std::atomic<int> step{0};

    auto n1 = graph.emplace([&step] {
        TEST_ASSERT(step.load() == 0);
        step.store(1);
    });

    auto n2 = graph.emplace([&step] {
        TEST_ASSERT(step.load() == 1);
        step.store(2);
    });

    auto n3 = graph.emplace([&step] {
        TEST_ASSERT(step.load() == 2);
        step.store(3);
    });

    graph.add_edge(n1, n2, astra::EdgePolicy::RequireSuccess);
    graph.add_edge(n2, n3, astra::EdgePolicy::RequireSuccess);

    auto run = scheduler.run(std::move(graph).freeze());
    run.wait();

    TEST_ASSERT(run.state() == astra::GraphRunState::Succeeded);
    TEST_ASSERT(step.load() == 3);

    const auto& rep = run.get_report();
    TEST_ASSERT(rep.total_nodes == 3);
    TEST_ASSERT(rep.succeeded_nodes == 3);
    TEST_ASSERT(rep.failed_nodes == 0);
    TEST_ASSERT(rep.cancelled_nodes == 0);
}

// 3. RequireSuccess 失败传播：A 失败，B 和 C 自动 Cancelled 且不执行 Callable（D-109 / D-110）
void test_R071_require_success_failure_propagation() {
    astra::Scheduler scheduler;
    astra::TaskGraph graph;

    std::atomic<bool> b_executed{false};
    std::atomic<bool> c_executed{false};

    auto a = graph.emplace([] {
        throw std::runtime_error("simulated failure in node A");
    });

    auto b = graph.emplace([&b_executed] {
        b_executed.store(true);
    });

    auto c = graph.emplace([&c_executed] {
        c_executed.store(true);
    });

    graph.add_edge(a, b, astra::EdgePolicy::RequireSuccess);
    graph.add_edge(b, c, astra::EdgePolicy::RequireSuccess);

    auto run = scheduler.run(std::move(graph).freeze());
    run.wait();

    TEST_ASSERT(run.state() == astra::GraphRunState::Failed);
    TEST_ASSERT(!b_executed.load());
    TEST_ASSERT(!c_executed.load());

    const auto& rep = run.get_report();
    TEST_ASSERT(rep.total_nodes == 3);
    TEST_ASSERT(rep.succeeded_nodes == 0);
    TEST_ASSERT(rep.failed_nodes == 1);
    TEST_ASSERT(rep.cancelled_nodes == 2);
    TEST_ASSERT(rep.failed_node_exceptions.size() == 1);
    TEST_ASSERT(rep.failed_node_exceptions[0].first == a);

    try {
        std::rethrow_exception(rep.failed_node_exceptions[0].second);
    } catch (const std::runtime_error& ex) {
        TEST_ASSERT(std::string(ex.what()) == "simulated failure in node A");
    }
}

// 4. AfterCompletion 策略：即使前置失败，清理节点依然在全部前置 Terminal 后执行（D-109 / D-110）
void test_R071_after_completion_cleanup_runs() {
    astra::Scheduler scheduler;
    astra::TaskGraph graph;

    std::atomic<bool> a_done{false};
    std::atomic<bool> cleanup_executed{false};
    std::atomic<bool> normal_executed{false};

    auto a = graph.emplace([&a_done] {
        a_done.store(true);
        throw std::runtime_error("node A failed");
    });

    auto normal_branch = graph.emplace([&normal_executed] {
        normal_executed.store(true);
    });

    auto cleanup = graph.emplace([&a_done, &cleanup_executed] {
        TEST_ASSERT(a_done.load());
        cleanup_executed.store(true);
    });

    graph.add_edge(a, normal_branch, astra::EdgePolicy::RequireSuccess);
    graph.add_edge(a, cleanup, astra::EdgePolicy::AfterCompletion);

    auto run = scheduler.run(std::move(graph).freeze());
    run.wait();

    TEST_ASSERT(run.state() == astra::GraphRunState::Failed);
    TEST_ASSERT(!normal_executed.load());
    TEST_ASSERT(cleanup_executed.load());

    const auto& rep = run.get_report();
    TEST_ASSERT(rep.total_nodes == 3);
    TEST_ASSERT(rep.succeeded_nodes == 1); // cleanup succeeded
    TEST_ASSERT(rep.failed_nodes == 1);    // node A failed
    TEST_ASSERT(rep.cancelled_nodes == 1); // normal_branch cancelled
}

// 5. 独立分支隔离：兄弟独立分支不受失败分支影响，正常调度执行（D-110）
void test_R071_independent_branch_isolation() {
    astra::Scheduler scheduler;
    astra::TaskGraph graph;

    std::atomic<bool> branch2_done{false};

    // 分支 1：失败
    auto a1 = graph.emplace([] {
        throw std::runtime_error("branch 1 failure");
    });
    auto b1 = graph.emplace([] {});
    graph.add_edge(a1, b1, astra::EdgePolicy::RequireSuccess);

    // 分支 2：独立且成功
    auto a2 = graph.emplace([] {});
    auto b2 = graph.emplace([&branch2_done] {
        branch2_done.store(true);
    });
    graph.add_edge(a2, b2, astra::EdgePolicy::RequireSuccess);

    auto run = scheduler.run(std::move(graph).freeze());
    run.wait();

    TEST_ASSERT(run.state() == astra::GraphRunState::Failed);
    TEST_ASSERT(branch2_done.load());

    const auto& rep = run.get_report();
    TEST_ASSERT(rep.total_nodes == 4);
    TEST_ASSERT(rep.succeeded_nodes == 2);
    TEST_ASSERT(rep.failed_nodes == 1);
    TEST_ASSERT(rep.cancelled_nodes == 1);
}

// 6. 混合前置依赖汇合：RequireSuccess + AfterCompletion 矩阵
void test_R071_multi_predecessor_mixed_policy_matrix() {
    astra::Scheduler scheduler;

    // Case 1: P1(AfterCompletion, Failed), P2(RequireSuccess, Succeeded) -> Successor 执行
    {
        astra::TaskGraph graph;
        std::atomic<bool> succ_executed{false};

        auto p1 = graph.emplace([] {
            throw std::runtime_error("p1 fail");
        });
        auto p2 = graph.emplace([] {});
        auto succ = graph.emplace([&succ_executed] {
            succ_executed.store(true);
        });

        graph.add_edge(p1, succ, astra::EdgePolicy::AfterCompletion);
        graph.add_edge(p2, succ, astra::EdgePolicy::RequireSuccess);

        auto run = scheduler.run(std::move(graph).freeze());
        run.wait();

        TEST_ASSERT(run.state() == astra::GraphRunState::Failed);
        TEST_ASSERT(succ_executed.load());
    }

    // Case 2: P1(AfterCompletion, Succeeded), P2(RequireSuccess, Failed) -> Successor 取消
    {
        astra::TaskGraph graph;
        std::atomic<bool> succ_executed{false};

        auto p1 = graph.emplace([] {});
        auto p2 = graph.emplace([] {
            throw std::runtime_error("p2 fail");
        });
        auto succ = graph.emplace([&succ_executed] {
            succ_executed.store(true);
        });

        graph.add_edge(p1, succ, astra::EdgePolicy::AfterCompletion);
        graph.add_edge(p2, succ, astra::EdgePolicy::RequireSuccess);

        auto run = scheduler.run(std::move(graph).freeze());
        run.wait();

        TEST_ASSERT(run.state() == astra::GraphRunState::Failed);
        TEST_ASSERT(!succ_executed.load());
    }
}

// 7. Node 抛出 task_cancelled 异常标记为 Cancelled 而非 Failed
void test_R071_node_task_cancelled_propagation() {
    astra::Scheduler scheduler;
    astra::TaskGraph graph;

    std::atomic<bool> req_executed{false};
    std::atomic<bool> cleanup_executed{false};

    auto a = graph.emplace([] {
        throw astra::task_cancelled{};
    });

    auto b = graph.emplace([&req_executed] {
        req_executed.store(true);
    });

    auto cleanup = graph.emplace([&cleanup_executed] {
        cleanup_executed.store(true);
    });

    graph.add_edge(a, b, astra::EdgePolicy::RequireSuccess);
    graph.add_edge(a, cleanup, astra::EdgePolicy::AfterCompletion);

    auto run = scheduler.run(std::move(graph).freeze());
    run.wait();

    TEST_ASSERT(run.state() == astra::GraphRunState::Cancelled);
    TEST_ASSERT(!req_executed.load());
    TEST_ASSERT(cleanup_executed.load());

    const auto& rep = run.get_report();
    TEST_ASSERT(rep.total_nodes == 3);
    TEST_ASSERT(rep.succeeded_nodes == 1);
    TEST_ASSERT(rep.failed_nodes == 0);
    TEST_ASSERT(rep.cancelled_nodes == 2);
}

}  // namespace

int main() {
    std::printf("Running astra_graph_edge_policies_test...\n");
    test_R071_void_node_compile_time_contract();
    test_R071_require_success_pipeline();
    test_R071_require_success_failure_propagation();
    test_R071_after_completion_cleanup_runs();
    test_R071_independent_branch_isolation();
    test_R071_multi_predecessor_mixed_policy_matrix();
    test_R071_node_task_cancelled_propagation();
    std::printf("All AST-030 void control graph and edge policy tests passed successfully!\n");
    return 0;
}

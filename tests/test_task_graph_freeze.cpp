#include <astra/graph.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
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

// -----------------------------------------------------------------------------
// 1. R-069 / D-104: 空图合法性验证
// -----------------------------------------------------------------------------
void test_R069_empty_graph_valid() {
    astra::TaskGraph graph;
    TEST_ASSERT(graph.empty());
    TEST_ASSERT(graph.node_count() == 0);
    TEST_ASSERT(graph.edge_count() == 0);

    astra::FrozenTaskGraph frozen = std::move(graph).freeze();
    TEST_ASSERT(frozen.empty());
    TEST_ASSERT(frozen.node_count() == 0);
    TEST_ASSERT(frozen.edge_count() == 0);
}

// -----------------------------------------------------------------------------
// 2. R-069 / D-104 / D-161: Move-only Callable 支持与 NodeId 单调递增序列
// -----------------------------------------------------------------------------
void test_R069_move_only_callables_and_node_id_sequence() {
    astra::TaskGraph graph;

    auto move_only_1 = std::make_unique<int>(10);
    auto move_only_2 = std::make_unique<int>(20);
    auto move_only_3 = std::make_unique<int>(30);

    astra::NodeId n1 = graph.emplace([p = std::move(move_only_1)] { (void)*p; });
    astra::NodeId n2 = graph.emplace([p = std::move(move_only_2)] { (void)*p; });
    astra::NodeId n3 = graph.emplace([p = std::move(move_only_3)] { (void)*p; });

    TEST_ASSERT(n1 == astra::NodeId{1});
    TEST_ASSERT(n2 == astra::NodeId{2});
    TEST_ASSERT(n3 == astra::NodeId{3});

    graph.add_edge(n1, n2);
    graph.add_edge(n2, n3);

    TEST_ASSERT(!graph.empty());
    TEST_ASSERT(graph.node_count() == 3);
    TEST_ASSERT(graph.edge_count() == 2);

    astra::FrozenTaskGraph frozen = std::move(graph).freeze();
    TEST_ASSERT(!frozen.empty());
    TEST_ASSERT(frozen.node_count() == 3);
    TEST_ASSERT(frozen.edge_count() == 2);
}

// -----------------------------------------------------------------------------
// 3. R-069 / D-105: 校验拒绝原因（ForeignNode, SelfEdge, DuplicateEdge, Cycle）
// -----------------------------------------------------------------------------
void test_R069_validation_rejection_reasons() {
    // (a) ForeignNode (越界或无效 NodeId)
    {
        astra::TaskGraph g;
        astra::NodeId n1 = g.emplace([] {});
        g.add_edge(n1, astra::NodeId{99});  // 99 不属于该图
        bool caught = false;
        try {
            (void)std::move(g).freeze();
        } catch (const astra::graph_validation_error& ex) {
            caught = true;
            TEST_ASSERT(ex.reason() == astra::GraphValidationError::ForeignNode);
        }
        TEST_ASSERT(caught);
    }

    // (b) SelfEdge (自环)
    {
        astra::TaskGraph g;
        astra::NodeId n1 = g.emplace([] {});
        g.add_edge(n1, n1);
        bool caught = false;
        try {
            (void)std::move(g).freeze();
        } catch (const astra::graph_validation_error& ex) {
            caught = true;
            TEST_ASSERT(ex.reason() == astra::GraphValidationError::SelfEdge);
        }
        TEST_ASSERT(caught);
    }

    // (c) DuplicateEdge (重复边)
    {
        astra::TaskGraph g;
        astra::NodeId n1 = g.emplace([] {});
        astra::NodeId n2 = g.emplace([] {});
        g.add_edge(n1, n2);
        g.add_edge(n1, n2);  // 重复
        bool caught = false;
        try {
            (void)std::move(g).freeze();
        } catch (const astra::graph_validation_error& ex) {
            caught = true;
            TEST_ASSERT(ex.reason() == astra::GraphValidationError::DuplicateEdge);
        }
        TEST_ASSERT(caught);
    }

    // (d) Cycle 与确定性 witness
    {
        astra::TaskGraph g;
        astra::NodeId n1 = g.emplace([] {});
        astra::NodeId n2 = g.emplace([] {});
        astra::NodeId n3 = g.emplace([] {});
        g.add_edge(n1, n2);
        g.add_edge(n2, n3);
        g.add_edge(n3, n1);  // 1 -> 2 -> 3 -> 1 环路

        bool caught = false;
        try {
            (void)std::move(g).freeze();
        } catch (const astra::graph_validation_error& ex) {
            caught = true;
            TEST_ASSERT(ex.reason() == astra::GraphValidationError::Cycle);
            const auto& w = ex.cycle_witness();
            TEST_ASSERT(w.size() == 4);
            TEST_ASSERT(w.front() == w.back());
            TEST_ASSERT(w[0] == n1 && w[1] == n2 && w[2] == n3 && w[3] == n1);
        }
        TEST_ASSERT(caught);
    }
}

// -----------------------------------------------------------------------------
// 4. R-069 / D-105: 复杂 DAG 与非连通分量环路检测
// -----------------------------------------------------------------------------
void test_R069_complex_dag_and_disconnected_cycle() {
    // 菱形无环图正常通过
    {
        astra::TaskGraph g;
        auto n1 = g.emplace([] {});
        auto n2 = g.emplace([] {});
        auto n3 = g.emplace([] {});
        auto n4 = g.emplace([] {});

        g.add_edge(n1, n2);
        g.add_edge(n1, n3);
        g.add_edge(n2, n4);
        g.add_edge(n3, n4);

        auto frozen = std::move(g).freeze();
        TEST_ASSERT(frozen.node_count() == 4);
        TEST_ASSERT(frozen.edge_count() == 4);
    }

    // 独立分支存在环路
    {
        astra::TaskGraph g;
        auto n1 = g.emplace([] {});
        auto n2 = g.emplace([] {});
        auto n3 = g.emplace([] {});
        auto n4 = g.emplace([] {});

        g.add_edge(n1, n2);  // 独立有效分支 1 -> 2
        g.add_edge(n3, n4);  // 独立环路分支 3 -> 4 -> 3
        g.add_edge(n4, n3);

        bool caught = false;
        try {
            (void)std::move(g).freeze();
        } catch (const astra::graph_validation_error& ex) {
            caught = true;
            TEST_ASSERT(ex.reason() == astra::GraphValidationError::Cycle);
            const auto& w = ex.cycle_witness();
            TEST_ASSERT(w.size() == 3);
            TEST_ASSERT(w.front() == w.back());
            TEST_ASSERT(w[0] == n3 && w[1] == n4 && w[2] == n3);
        }
        TEST_ASSERT(caught);
    }
}

}  // namespace

int main() {
    std::printf("Running astra_task_graph_freeze_test...\n");
    test_R069_empty_graph_valid();
    test_R069_move_only_callables_and_node_id_sequence();
    test_R069_validation_rejection_reasons();
    test_R069_complex_dag_and_disconnected_cycle();
    std::printf("All AST-028 TaskGraph consuming freeze tests passed successfully!\n");
    return 0;
}

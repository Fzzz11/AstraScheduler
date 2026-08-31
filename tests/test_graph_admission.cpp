#include <astra/graph.hpp>
#include <astra/scheduler.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
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

// -----------------------------------------------------------------------------
// 1. R-070 / D-106: 空图即时完成与 0 资源占用
// -----------------------------------------------------------------------------
void test_R070_empty_graph_admission() {
    astra::Scheduler scheduler;
    astra::TaskGraph graph;
    auto frozen = std::move(graph).freeze();

    astra::GraphRun run = scheduler.run(std::move(frozen));
    TEST_ASSERT(run.valid());
    TEST_ASSERT(run.node_count() == 0);
    TEST_ASSERT(run.is_completed());
    TEST_ASSERT(run.state() == astra::GraphRunState::Succeeded);
    run.wait();
}

// -----------------------------------------------------------------------------
// 2. R-070 / D-106: All-or-nothing 容量核算与超限立即拒绝
// -----------------------------------------------------------------------------
void test_R070_all_or_nothing_capacity_rejection() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    opts.external_pending_capacity = 3;
    opts.external_backpressure = astra::ExternalBackpressure::Reject;
    astra::Scheduler scheduler(opts);

    // 图规模 (4) > 外部容量 (3) -> 立即拒绝，全回滚
    astra::TaskGraph large_graph;
    for (int i = 0; i < 4; ++i) {
        large_graph.emplace([] {});
    }
    auto frozen_large = std::move(large_graph).freeze();

    bool caught = false;
    try {
        (void)scheduler.run(std::move(frozen_large));
    } catch (const astra::submission_rejected& ex) {
        caught = true;
        TEST_ASSERT(ex.reason() == astra::SubmissionError::CapacityExhausted);
    }
    TEST_ASSERT(caught);

    // 随后提交合法规模的图必须成功
    astra::TaskGraph valid_graph;
    std::atomic<int> executed{0};
    for (int i = 0; i < 3; ++i) {
        valid_graph.emplace([&executed] { executed.fetch_add(1); });
    }
    auto frozen_valid = std::move(valid_graph).freeze();
    auto run = scheduler.run(std::move(frozen_valid));
    run.wait();
    TEST_ASSERT(executed.load() == 3);
    TEST_ASSERT(run.state() == astra::GraphRunState::Succeeded);
}

// -----------------------------------------------------------------------------
// 3. R-070 / D-106: Internal Graph 豁免外部 slot 限制
// -----------------------------------------------------------------------------
void test_R070_internal_graph_exempt_from_slots() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    opts.external_pending_capacity = 2;
    opts.external_backpressure = astra::ExternalBackpressure::Reject;
    astra::Scheduler scheduler(opts);

    // 从 worker 内部发起超过外部容量的图 (4 节点)
    auto handle = scheduler.submit([&scheduler] {
        astra::TaskGraph g;
        std::atomic<int> count{0};
        for (int i = 0; i < 4; ++i) {
            g.emplace([&count] { count.fetch_add(1); });
        }
        auto frozen = std::move(g).freeze();
        auto run = scheduler.run(std::move(frozen));
        run.wait();
        TEST_ASSERT(count.load() == 4);
        TEST_ASSERT(run.state() == astra::GraphRunState::Succeeded);
    });

    handle.get();
}

// -----------------------------------------------------------------------------
// 4. R-070 / D-107: DAG 依赖汇合拓扑执行序与 countdown 仲裁
// -----------------------------------------------------------------------------
void test_R070_dag_execution_order_and_countdown_arbitration() {
    astra::SchedulerOptions opts;
    opts.worker_count = 4;
    astra::Scheduler scheduler(opts);

    astra::TaskGraph graph;
    std::atomic<bool> a_done{false};
    std::atomic<bool> b_done{false};
    std::atomic<bool> c_done{false};
    std::atomic<bool> d_done{false};

    auto na = graph.emplace([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        a_done.store(true, std::memory_order_release);
    });
    auto nb = graph.emplace([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        b_done.store(true, std::memory_order_release);
    });
    auto nc = graph.emplace([&] {
        // C 必须观察到 A 和 B 均已完成
        TEST_ASSERT(a_done.load(std::memory_order_acquire));
        TEST_ASSERT(b_done.load(std::memory_order_acquire));
        c_done.store(true, std::memory_order_release);
    });
    auto nd = graph.emplace([&] {
        // D 必须观察到 C 已完成
        TEST_ASSERT(c_done.load(std::memory_order_acquire));
        d_done.store(true, std::memory_order_release);
    });

    // 依赖: A -> C, B -> C, C -> D
    graph.add_edge(na, nc);
    graph.add_edge(nb, nc);
    graph.add_edge(nc, nd);

    auto frozen = std::move(graph).freeze();
    auto run = scheduler.run(std::move(frozen));
    run.wait();

    TEST_ASSERT(a_done.load());
    TEST_ASSERT(b_done.load());
    TEST_ASSERT(c_done.load());
    TEST_ASSERT(d_done.load());
    TEST_ASSERT(run.state() == astra::GraphRunState::Succeeded);
}

// -----------------------------------------------------------------------------
// 5. R-070 / D-107: 多前驱扇入并发竞争压力测试
// -----------------------------------------------------------------------------
void test_R070_multi_predecessor_race_stress() {
    astra::SchedulerOptions opts;
    opts.worker_count = 4;
    astra::Scheduler scheduler(opts);

    constexpr int kRounds = 50;
    constexpr int kPredecessors = 16;

    for (int r = 0; r < kRounds; ++r) {
        astra::TaskGraph graph;
        std::atomic<int> preds_done{0};
        std::atomic<int> sink_executed{0};

        std::vector<astra::NodeId> pred_ids;
        for (int i = 0; i < kPredecessors; ++i) {
            pred_ids.push_back(graph.emplace([&preds_done] {
                preds_done.fetch_add(1, std::memory_order_release);
            }));
        }

        auto sink_id = graph.emplace([&preds_done, &sink_executed] {
            TEST_ASSERT(preds_done.load(std::memory_order_acquire) == kPredecessors);
            sink_executed.fetch_add(1, std::memory_order_release);
        });

        for (auto pid : pred_ids) {
            graph.add_edge(pid, sink_id);
        }

        auto run = scheduler.run(std::move(graph).freeze());
        run.wait();

        TEST_ASSERT(preds_done.load() == kPredecessors);
        TEST_ASSERT(sink_executed.load() == 1);
        TEST_ASSERT(run.state() == astra::GraphRunState::Succeeded);
    }
}

// -----------------------------------------------------------------------------
// 6. R-072 / R-115: 普通图节点也必须获得 Runtime 分配的稳定 TaskId
// -----------------------------------------------------------------------------
void test_R115_graph_nodes_have_runtime_task_ids() {
    astra::Scheduler scheduler;
    astra::TaskGraph graph;
    std::mutex mutex;
    std::vector<astra::TaskId> observed_ids;

    graph.emplace([&] {
        std::lock_guard<std::mutex> lock(mutex);
        observed_ids.push_back(astra::detail::current_executing_task_id());
    });
    graph.emplace([&] {
        std::lock_guard<std::mutex> lock(mutex);
        observed_ids.push_back(astra::detail::current_executing_task_id());
    });

    auto run = scheduler.run(std::move(graph).freeze());
    run.wait();

    TEST_ASSERT(observed_ids.size() == 2);
    TEST_ASSERT(observed_ids[0].valid());
    TEST_ASSERT(observed_ids[1].valid());
    TEST_ASSERT(observed_ids[0] != observed_ids[1]);

    std::sort(observed_ids.begin(), observed_ids.end());
    TEST_ASSERT(observed_ids[0].sequence() == 1);
    TEST_ASSERT(observed_ids[1].sequence() == 2);

    // Graph 节点消耗的身份必须纳入同一 Runtime 序列，后续普通提交不能复用。
    auto next_task = scheduler.submit([] {});
    TEST_ASSERT(next_task.task_id().sequence() == 3);
    next_task.get();
}

}  // namespace

int main() {
    std::printf("Running astra_graph_admission_test...\n");
    test_R070_empty_graph_admission();
    test_R070_all_or_nothing_capacity_rejection();
    test_R070_internal_graph_exempt_from_slots();
    test_R070_dag_execution_order_and_countdown_arbitration();
    test_R070_multi_predecessor_race_stress();
    test_R115_graph_nodes_have_runtime_task_ids();
    std::printf("All AST-029 GraphRun admission and dependency release tests passed successfully!\n");
    return 0;
}

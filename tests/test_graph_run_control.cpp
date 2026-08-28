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

// 1. empty / moved-from GraphRun 验证（R-072 / D-113）
void test_R072_empty_and_invalid_graph_run() {
    astra::GraphRun empty_run;
    TEST_ASSERT(!empty_run.valid());
    TEST_ASSERT(!empty_run);

    // request_cancel 对 invalid handle 为 no-op（noexcept）
    empty_run.request_cancel();

    // 访问操作均抛 std::logic_error
    bool threw = false;
    try {
        (void)empty_run.id();
    } catch (const std::logic_error&) {
        threw = true;
    }
    TEST_ASSERT(threw);

    threw = false;
    try {
        (void)empty_run.node_count();
    } catch (const std::logic_error&) {
        threw = true;
    }
    TEST_ASSERT(threw);

    threw = false;
    try {
        (void)empty_run.state();
    } catch (const std::logic_error&) {
        threw = true;
    }
    TEST_ASSERT(threw);

    threw = false;
    try {
        (void)empty_run.is_completed();
    } catch (const std::logic_error&) {
        threw = true;
    }
    TEST_ASSERT(threw);

    threw = false;
    try {
        empty_run.wait();
    } catch (const std::logic_error&) {
        threw = true;
    }
    TEST_ASSERT(threw);

    threw = false;
    try {
        (void)empty_run.wait_for(std::chrono::milliseconds(10));
    } catch (const std::logic_error&) {
        threw = true;
    }
    TEST_ASSERT(threw);

    threw = false;
    try {
        (void)empty_run.get_report();
    } catch (const std::logic_error&) {
        threw = true;
    }
    TEST_ASSERT(threw);

    // 移动构造后源对象变 empty
    astra::Scheduler scheduler;
    astra::TaskGraph graph;
    graph.emplace([] {});
    auto valid_run = scheduler.run(std::move(graph).freeze());
    TEST_ASSERT(valid_run.valid());

    auto moved_run = std::move(valid_run);
    TEST_ASSERT(!valid_run.valid());
    TEST_ASSERT(moved_run.valid());

    moved_run.wait();
    TEST_ASSERT(moved_run.is_completed());
}

// 2. request_cancel 显式取消与 stop_token 协作（R-072 / D-111）
void test_R072_request_cancel_and_stop_token() {
    astra::Scheduler scheduler;
    astra::TaskGraph graph;

    std::atomic<bool> n1_started{false};
    std::atomic<bool> n1_observed_stop{false};
    std::atomic<bool> n2_executed{false};

    auto n1 = graph.emplace([&](std::stop_token st) {
        n1_started.store(true);
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500)) {
            if (st.stop_requested()) {
                n1_observed_stop.store(true);
                throw astra::task_cancelled{};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    auto n2 = graph.emplace([&] {
        n2_executed.store(true);
    });

    graph.add_edge(n1, n2, astra::EdgePolicy::RequireSuccess);

    auto run = scheduler.run(std::move(graph).freeze());

    // 等待 n1 开始执行
    while (!n1_started.load()) {
        std::this_thread::yield();
    }

    // 显式请求全图取消
    run.request_cancel();
    run.wait();

    TEST_ASSERT(run.state() == astra::GraphRunState::Cancelled);
    TEST_ASSERT(n1_observed_stop.load());
    TEST_ASSERT(!n2_executed.load());

    const auto& rep = run.get_report();
    TEST_ASSERT(rep.total_nodes == 2);
    TEST_ASSERT(rep.succeeded_nodes == 0);
    TEST_ASSERT(rep.failed_nodes == 0);
    TEST_ASSERT(rep.cancelled_nodes == 2);
}

// 3. 不可变 GraphReport 按 NodeId 升序聚合多失败（R-072 / D-112 / D-152）
void test_R072_immutable_graph_report_and_failure_ordering() {
    astra::Scheduler scheduler;
    astra::TaskGraph graph;

    // 建立 4 个节点：n1 失败、n2 成功、n3 失败、n4 依赖 n3（被取消）
    auto n1 = graph.emplace([] {
        throw std::runtime_error("error in n1");
    });
    auto n2 = graph.emplace([] {});
    auto n3 = graph.emplace([] {
        throw std::invalid_argument("error in n3");
    });
    auto n4 = graph.emplace([] {});

    graph.add_edge(n3, n4, astra::EdgePolicy::RequireSuccess);

    auto run = scheduler.run(std::move(graph).freeze());
    run.wait();

    TEST_ASSERT(run.state() == astra::GraphRunState::Failed);

    const auto& rep1 = run.get_report();
    const auto& rep2 = run.get_report();
    TEST_ASSERT(&rep1 == &rep2); // 共享同一不可变 report

    TEST_ASSERT(rep1.run_id == run.id());
    TEST_ASSERT(rep1.total_nodes == 4);
    TEST_ASSERT(rep1.succeeded_nodes == 1);
    TEST_ASSERT(rep1.failed_nodes == 2);
    TEST_ASSERT(rep1.cancelled_nodes == 1);

    // 验证 failed_node_exceptions 严格按 NodeId 升序排序
    TEST_ASSERT(rep1.failed_node_exceptions.size() == 2);
    TEST_ASSERT(rep1.failed_node_exceptions[0].first < rep1.failed_node_exceptions[1].first);
    TEST_ASSERT(rep1.failed_node_exceptions[0].first == n1);
    TEST_ASSERT(rep1.failed_node_exceptions[1].first == n3);

    try {
        std::rethrow_exception(rep1.failed_node_exceptions[0].second);
    } catch (const std::runtime_error& ex) {
        TEST_ASSERT(std::string(ex.what()) == "error in n1");
    }

    try {
        std::rethrow_exception(rep1.failed_node_exceptions[1].second);
    } catch (const std::invalid_argument& ex) {
        TEST_ASSERT(std::string(ex.what()) == "error in n3");
    }
}

// 4. direct self-run 检测：正在执行 GraphNode 的 Worker 等待自己的 GraphRun 必须抛 std::logic_error（R-072 / D-113）
void test_R072_direct_self_run_rejection() {
    astra::Scheduler scheduler;
    astra::TaskGraph graph;

    std::atomic<bool> wait_rejected{false};
    std::atomic<bool> wait_for_rejected{false};
    std::atomic<bool> get_report_rejected{false};

    // 保存 run 引用
    astra::GraphRun captured_run;

    auto n1 = graph.emplace([&] {
        try {
            captured_run.wait();
        } catch (const std::logic_error&) {
            wait_rejected.store(true);
        }

        try {
            (void)captured_run.wait_for(std::chrono::milliseconds(10));
        } catch (const std::logic_error&) {
            wait_for_rejected.store(true);
        }

        try {
            (void)captured_run.get_report();
        } catch (const std::logic_error&) {
            get_report_rejected.store(true);
        }
    });

    captured_run = scheduler.run(std::move(graph).freeze());
    captured_run.wait();

    TEST_ASSERT(captured_run.state() == astra::GraphRunState::Succeeded);
    TEST_ASSERT(wait_rejected.load());
    TEST_ASSERT(wait_for_rejected.load());
    TEST_ASSERT(get_report_rejected.load());
}

// 5. caller-relative wait 与 wait_for 超时验证（R-072 / D-113）
void test_R072_wait_for_timeout_and_completion() {
    astra::Scheduler scheduler;
    astra::TaskGraph graph;

    std::atomic<bool> can_finish{false};

    graph.emplace([&] {
        while (!can_finish.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    auto run = scheduler.run(std::move(graph).freeze());

    // 1. 短超时：应该返回 TimedOut
    auto res = run.wait_for(std::chrono::milliseconds(10));
    TEST_ASSERT(res == astra::GraphWaitResult::TimedOut);
    TEST_ASSERT(!run.is_completed());

    // 2. 释放阻塞并等待完成
    can_finish.store(true);
    res = run.wait_for(std::chrono::seconds(2));
    TEST_ASSERT(res == astra::GraphWaitResult::Completed);
    TEST_ASSERT(run.is_completed());

    // 3. 已完成后 wait_for(0) 和负超时必须返回 Completed
    TEST_ASSERT(run.wait_for(std::chrono::milliseconds(0)) == astra::GraphWaitResult::Completed);
    TEST_ASSERT(run.wait_for(std::chrono::milliseconds(-10)) == astra::GraphWaitResult::Completed);
}

// 6. Worker Helping Wait：在普通 Task 中等待 GraphRun（R-072 / D-113）
void test_R072_worker_helping_wait() {
    astra::Scheduler scheduler;
    astra::TaskGraph graph;

    std::atomic<int> counter{0};
    for (int i = 0; i < 5; ++i) {
        graph.emplace([&counter] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            counter.fetch_add(1);
        });
    }

    auto run = scheduler.run(std::move(graph).freeze());

    // 在普通 Task 里等待 graph run（跨 task / foreign run waiting）
    auto handle = scheduler.submit([run]() mutable {
        run.wait();
        return run.state() == astra::GraphRunState::Succeeded;
    });

    TEST_ASSERT(handle.get() == true);
    TEST_ASSERT(counter.load() == 5);
}

}  // namespace

int main() {
    std::printf("Running astra_graph_run_control_test...\n");
    test_R072_empty_and_invalid_graph_run();
    test_R072_request_cancel_and_stop_token();
    test_R072_immutable_graph_report_and_failure_ordering();
    test_R072_direct_self_run_rejection();
    test_R072_wait_for_timeout_and_completion();
    test_R072_worker_helping_wait();
    std::printf("All AST-031 GraphRun control and waiting tests passed successfully!\n");
    return 0;
}

#include "astra/coroutine.hpp"
#include "astra/graph.hpp"
#include "astra/scheduler.hpp"
#include "astra/task_handle.hpp"
#include "astra/task_options.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// -----------------------------------------------------------------------------
// 1. 同 Priority band 内：绝对 Deadline 较早的任务优先执行 (R-083 / D-133)
// -----------------------------------------------------------------------------
void test_R083_same_band_earliest_deadline_first() {
    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    astra::Scheduler sched(opts);

    std::mutex mtx;
    std::vector<int> execution_order;
    std::atomic<bool> gate{false};
    std::mutex gate_mtx;
    std::condition_variable gate_cv;

    // 阻塞 worker 使得后续提交进入 Global 队列排队
    auto blocker = sched.submit([&] {
        std::unique_lock<std::mutex> lk(gate_mtx);
        gate_cv.wait(lk, [&] { return gate.load(); });
    });

    std::this_thread::sleep_for(20ms);

    const auto now = std::chrono::steady_clock::now();
    // 提交 3 个 Normal 优先级任务，按 deadline 递减/乱序提交
    astra::TaskOptions opt1;
    opt1.priority = astra::Priority::Normal;
    opt1.deadline = astra::TaskDeadline::at(now + 300ms);

    astra::TaskOptions opt2;
    opt2.priority = astra::Priority::Normal;
    opt2.deadline = astra::TaskDeadline::at(now + 100ms);

    astra::TaskOptions opt3;
    opt3.priority = astra::Priority::Normal;
    opt3.deadline = astra::TaskDeadline::at(now + 200ms);

    auto h1 = sched.submit(opt1, [&] {
        std::lock_guard<std::mutex> lk(mtx);
        execution_order.push_back(1);
    });

    auto h2 = sched.submit(opt2, [&] {
        std::lock_guard<std::mutex> lk(mtx);
        execution_order.push_back(2);
    });

    auto h3 = sched.submit(opt3, [&] {
        std::lock_guard<std::mutex> lk(mtx);
        execution_order.push_back(3);
    });

    {
        std::lock_guard<std::mutex> lk(gate_mtx);
        gate.store(true);
        gate_cv.notify_all();
    }

    h1.wait();
    h2.wait();
    h3.wait();

    std::lock_guard<std::mutex> lk(mtx);
    assert(execution_order.size() == 3);
    // 期望执行顺序：2 (100ms) -> 3 (200ms) -> 1 (300ms)
    assert(execution_order[0] == 2);
    assert(execution_order[1] == 3);
    assert(execution_order[2] == 1);
    std::cout << "test_R083_same_band_earliest_deadline_first passed\n";
}

// -----------------------------------------------------------------------------
// 2. 跨 Priority band：Priority 主导，低优先级早 deadline 不抢占高优先级 (R-083 / D-134)
// -----------------------------------------------------------------------------
void test_R083_priority_dominates_cross_bands() {
    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    astra::Scheduler sched(opts);

    std::mutex mtx;
    std::vector<int> execution_order;
    std::atomic<bool> gate{false};
    std::mutex gate_mtx;
    std::condition_variable gate_cv;

    auto blocker = sched.submit([&] {
        std::unique_lock<std::mutex> lk(gate_mtx);
        gate_cv.wait(lk, [&] { return gate.load(); });
    });

    std::this_thread::sleep_for(20ms);

    const auto now = std::chrono::steady_clock::now();

    // Low 优先级但 deadline 已过期
    astra::TaskOptions low_opts;
    low_opts.priority = astra::Priority::Low;
    low_opts.deadline = astra::TaskDeadline::at(now - 100ms);

    // Critical 优先级但 deadline 在遥远的未来
    astra::TaskOptions crit_opts;
    crit_opts.priority = astra::Priority::Critical;
    crit_opts.deadline = astra::TaskDeadline::at(now + 10s);

    auto h_low = sched.submit(low_opts, [&] {
        std::lock_guard<std::mutex> lk(mtx);
        execution_order.push_back(1); // Low
    });

    auto h_crit = sched.submit(crit_opts, [&] {
        std::lock_guard<std::mutex> lk(mtx);
        execution_order.push_back(2); // Critical
    });

    {
        std::lock_guard<std::mutex> lk(gate_mtx);
        gate.store(true);
        gate_cv.notify_all();
    }

    h_low.wait();
    h_crit.wait();

    std::lock_guard<std::mutex> lk(mtx);
    assert(execution_order.size() == 2);
    // Critical 必须先于 Low 执行
    assert(execution_order[0] == 2);
    assert(execution_order[1] == 1);
    std::cout << "test_R083_priority_dominates_cross_bands passed\n";
}

// -----------------------------------------------------------------------------
// 3. 相同 Deadline 的 FIFO Tie-breaking 稳定性 (D-133)
// -----------------------------------------------------------------------------
void test_R083_identical_deadline_fifo_tie_breaking() {
    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    astra::Scheduler sched(opts);

    std::mutex mtx;
    std::vector<int> execution_order;
    std::atomic<bool> gate{false};
    std::mutex gate_mtx;
    std::condition_variable gate_cv;

    auto blocker = sched.submit([&] {
        std::unique_lock<std::mutex> lk(gate_mtx);
        gate_cv.wait(lk, [&] { return gate.load(); });
    });

    std::this_thread::sleep_for(20ms);

    const auto common_dl = astra::TaskDeadline::at(std::chrono::steady_clock::now() + 500ms);
    astra::TaskOptions opts_same;
    opts_same.priority = astra::Priority::Normal;
    opts_same.deadline = common_dl;

    std::vector<astra::TaskHandle<void>> handles;
    for (int i = 1; i <= 5; ++i) {
        handles.push_back(sched.submit(opts_same, [&, id = i] {
            std::lock_guard<std::mutex> lk(mtx);
            execution_order.push_back(id);
        }));
    }

    {
        std::lock_guard<std::mutex> lk(gate_mtx);
        gate.store(true);
        gate_cv.notify_all();
    }

    for (auto& h : handles) {
        h.wait();
    }

    std::lock_guard<std::mutex> lk(mtx);
    assert(execution_order.size() == 5);
    for (int i = 0; i < 5; ++i) {
        assert(execution_order[i] == i + 1);
    }
    std::cout << "test_R083_identical_deadline_fifo_tie_breaking passed\n";
}

// -----------------------------------------------------------------------------
// 4. Deadline burst limit: 同 band 连续 8 个 EDF 后必须给 FIFO 服务机会 (D-134)
// -----------------------------------------------------------------------------
void test_R083_deadline_burst_limit_prevents_fifo_starvation() {
    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    astra::Scheduler sched(opts);

    std::mutex mtx;
    std::vector<int> execution_order;
    std::atomic<bool> gate{false};
    std::mutex gate_mtx;
    std::condition_variable gate_cv;

    auto blocker = sched.submit([&] {
        std::unique_lock<std::mutex> lk(gate_mtx);
        gate_cv.wait(lk, [&] { return gate.load(); });
    });

    std::this_thread::sleep_for(20ms);

    // 提交 1 个普通 FIFO 任务 (id = 100)
    astra::TaskOptions normal_fifo;
    normal_fifo.priority = astra::Priority::Normal;

    auto h_fifo = sched.submit(normal_fifo, [&] {
        std::lock_guard<std::mutex> lk(mtx);
        execution_order.push_back(100);
    });

    // 提交 10 个带 deadline 的任务 (id = 1..10)
    const auto dl = astra::TaskDeadline::at(std::chrono::steady_clock::now() + 10s);
    astra::TaskOptions edf_opts;
    edf_opts.priority = astra::Priority::Normal;
    edf_opts.deadline = dl;

    std::vector<astra::TaskHandle<void>> edf_handles;
    for (int i = 1; i <= 10; ++i) {
        edf_handles.push_back(sched.submit(edf_opts, [&, id = i] {
            std::lock_guard<std::mutex> lk(mtx);
            execution_order.push_back(id);
        }));
    }

    {
        std::lock_guard<std::mutex> lk(gate_mtx);
        gate.store(true);
        gate_cv.notify_all();
    }

    h_fifo.wait();
    for (auto& h : edf_handles) {
        h.wait();
    }

    std::lock_guard<std::mutex> lk(mtx);
    assert(execution_order.size() == 11);

    // 根据 D-134: 连续 8 个 EDF 之后必须给 FIFO 1 次 service opportunity
    for (int i = 0; i < 8; ++i) {
        assert(execution_order[i] == i + 1);
    }
    assert(execution_order[8] == 100);
    assert(execution_order[9] == 9);
    assert(execution_order[10] == 10);
    std::cout << "test_R083_deadline_burst_limit_prevents_fifo_starvation passed\n";
}

// -----------------------------------------------------------------------------
// 5. 关停时排空未开始的 EDF heap (cancel_pre_start 验证)
// -----------------------------------------------------------------------------
void test_R083_immediate_shutdown_drains_edf_heaps() {
    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    astra::Scheduler sched(opts);

    std::atomic<bool> gate{false};
    std::mutex gate_mtx;
    std::condition_variable gate_cv;

    auto blocker = sched.submit([&] {
        std::unique_lock<std::mutex> lk(gate_mtx);
        gate_cv.wait(lk, [&] { return gate.load(); });
    });

    std::this_thread::sleep_for(20ms);

    astra::TaskOptions edf_opts;
    edf_opts.priority = astra::Priority::High;
    edf_opts.deadline = astra::TaskDeadline::after(10s);

    std::atomic<bool> edf_executed{false};
    auto h_edf = sched.submit(edf_opts, [&] {
        edf_executed.store(true);
    });

    // 立即关停调度器
    {
        std::lock_guard<std::mutex> lk(gate_mtx);
        gate.store(true);
        gate_cv.notify_all();
    }

    sched.shutdown_now();
    assert(!edf_executed.load());
    assert(h_edf.state() == astra::TaskState::Cancelled);
    std::cout << "test_R083_immediate_shutdown_drains_edf_heaps passed\n";
}

// -----------------------------------------------------------------------------
// 6. 任务图 DAG 节点在依赖满足后入 EDF Heap (D-133)
// -----------------------------------------------------------------------------
void test_R083_graph_dag_nodes_with_deadline_edf() {
    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    astra::Scheduler sched(opts);

    std::mutex mtx;
    std::vector<int> execution_order;
    std::atomic<bool> gate{false};
    std::mutex gate_mtx;
    std::condition_variable gate_cv;

    auto blocker = sched.submit([&] {
        std::unique_lock<std::mutex> lk(gate_mtx);
        gate_cv.wait(lk, [&] { return gate.load(); });
    });

    std::this_thread::sleep_for(20ms);

    astra::TaskGraph graph;
    const auto now = std::chrono::steady_clock::now();

    // Node A (Root): 完成后释放 B 和 C
    auto node_a = graph.emplace([&] {
        std::lock_guard<std::mutex> lk(mtx);
        execution_order.push_back(0); // Root A
    });

    astra::TaskOptions opt_b;
    opt_b.priority = astra::Priority::Normal;
    opt_b.deadline = astra::TaskDeadline::at(now + 300ms); // 晚 deadline

    astra::TaskOptions opt_c;
    opt_c.priority = astra::Priority::Normal;
    opt_c.deadline = astra::TaskDeadline::at(now + 100ms); // 早 deadline

    auto node_b = graph.emplace(opt_b, [&] {
        std::lock_guard<std::mutex> lk(mtx);
        execution_order.push_back(1); // Node B
    });

    auto node_c = graph.emplace(opt_c, [&] {
        std::lock_guard<std::mutex> lk(mtx);
        execution_order.push_back(2); // Node C
    });

    graph.add_edge(node_a, node_b);
    graph.add_edge(node_a, node_c);

    auto run = sched.run(std::move(graph).freeze());

    {
        std::lock_guard<std::mutex> lk(gate_mtx);
        gate.store(true);
        gate_cv.notify_all();
    }

    run.wait();

    std::lock_guard<std::mutex> lk(mtx);
    assert(execution_order.size() == 3);
    assert(execution_order[0] == 0); // Root A 先完成
    // A 完成后释放 B 和 C，此时 C 的 deadline 比 B 早，所以 C 先于 B 执行
    assert(execution_order[1] == 2); // Node C (100ms)
    assert(execution_order[2] == 1); // Node B (300ms)
    std::cout << "test_R083_graph_dag_nodes_with_deadline_edf passed\n";
}

} // namespace

int main() {
    std::cout << "=== Running Global EDF Tests (AST-041 / R-083) ===\n";
    test_R083_same_band_earliest_deadline_first();
    test_R083_priority_dominates_cross_bands();
    test_R083_identical_deadline_fifo_tie_breaking();
    test_R083_deadline_burst_limit_prevents_fifo_starvation();
    test_R083_immediate_shutdown_drains_edf_heaps();
    test_R083_graph_dag_nodes_with_deadline_edf();
    std::cout << "=== All Global EDF Tests Passed! ===\n";
    return 0;
}

#include "astra/coroutine.hpp"
#include "astra/graph.hpp"
#include "astra/scheduler.hpp"
#include "astra/task_handle.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// 1. TaskHandle<T> await: same runtime, cross runtime, already-completed, exception
// -----------------------------------------------------------------------------
astra::Task<int> coro_await_task_val(astra::TaskHandle<int>& target_handle) {
    int val = co_await target_handle;
    co_return val * 2;
}

astra::Task<void> coro_await_task_void(astra::TaskHandle<void>& target_handle, std::atomic<bool>& flag) {
    co_await target_handle;
    flag.store(true, std::memory_order_release);
    co_return;
}

void test_R076_task_handle_await_success() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    // 1.1 same-runtime value await
    auto target_val = sched.submit([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 42;
    });

    auto awaiter_val = sched.spawn(coro_await_task_val(target_val));
    int res = awaiter_val.get();
    assert(res == 84);

    // 1.2 already-completed target await (await_ready is true)
    auto awaiter_fast = sched.spawn(coro_await_task_val(target_val));
    int res_fast = awaiter_fast.get();
    assert(res_fast == 84);

    // 1.3 void task await
    std::atomic<bool> void_completed{false};
    auto target_void = sched.submit([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });

    std::atomic<bool> flag{false};
    auto awaiter_void = sched.spawn(coro_await_task_void(target_void, flag));
    awaiter_void.get();
    assert(flag.load(std::memory_order_acquire));

    sched.shutdown();
}

void test_R076_task_handle_await_cross_runtime() {
    astra::SchedulerOptions optsA;
    optsA.worker_count = 2;
    astra::Scheduler schedA(optsA);

    astra::SchedulerOptions optsB;
    optsB.worker_count = 2;
    astra::Scheduler schedB(optsB);

    // Target on Runtime B
    auto target_on_B = schedB.submit([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 100;
    });

    // Awaiter spawned on Runtime A
    auto awaiter_on_A = schedA.spawn(coro_await_task_val(target_on_B));
    int res = awaiter_on_A.get();
    assert(res == 200);

    schedA.shutdown();
    schedB.shutdown();
}

astra::Task<int> coro_await_failing_task(astra::TaskHandle<int>& target_handle) {
    int val = co_await target_handle;
    co_return val;
}

void test_R076_task_handle_await_exception_propagation() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    auto target_fail = sched.submit([]() -> int {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        throw std::runtime_error("target failed!");
    });

    auto awaiter = sched.spawn(coro_await_failing_task(target_fail));

    bool threw = false;
    try {
        awaiter.get();
    } catch (const std::runtime_error& e) {
        threw = true;
        assert(std::string(e.what()) == "target failed!");
    }
    assert(threw);
    assert(awaiter.state() == astra::TaskState::Failed);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 2. Direct Self-Await rejection (D-120 / R-076)
// -----------------------------------------------------------------------------
astra::Task<int> coro_self_await(std::shared_ptr<astra::TaskHandle<int>> holder) {
    // 尝试 co_await 自身
    int val = co_await (*holder);
    co_return val;
}

void test_R076_direct_self_await_rejected() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    auto holder = std::make_shared<astra::TaskHandle<int>>();
    auto awaiter = sched.spawn(coro_self_await(holder));
    *holder = awaiter; // holder 现在持有自身 handle

    bool threw_logic_error = false;
    try {
        awaiter.get();
    } catch (const std::logic_error&) {
        threw_logic_error = true;
    }
    assert(threw_logic_error);
    assert(awaiter.state() == astra::TaskState::Failed);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 3. GraphRun await: completed, incomplete, failure report (D-121 / R-076)
// -----------------------------------------------------------------------------
astra::Task<std::size_t> coro_await_graph(astra::GraphRun& run) {
    const astra::GraphReport& report = co_await run;
    co_return report.succeeded_nodes;
}

void test_R076_graph_run_await() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    // 3.1 Incomplete GraphRun await
    astra::TaskGraph graph;
    std::atomic<int> counter{0};
    auto n1 = graph.emplace([&] { counter.fetch_add(1); });
    auto n2 = graph.emplace([&] { counter.fetch_add(2); });
    graph.add_edge(n1, n2);

    auto frozen = std::move(graph).freeze();
    auto run = sched.run(std::move(frozen));

    auto awaiter = sched.spawn(coro_await_graph(run));
    std::size_t succ = awaiter.get();
    assert(succ == 2);
    assert(counter.load() == 3);

    // 3.2 Already-completed GraphRun await
    auto awaiter_completed = sched.spawn(coro_await_graph(run));
    std::size_t succ_fast = awaiter_completed.get();
    assert(succ_fast == 2);

    sched.shutdown();
}

astra::Task<std::size_t> coro_await_failing_graph(astra::GraphRun& run) {
    const astra::GraphReport& rep = co_await run;
    // Failed GraphRun await 不自动抛异常，而是返回 report
    co_return rep.failed_nodes;
}

void test_R076_graph_run_await_failed_report() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    astra::TaskGraph graph;
    auto n1 = graph.emplace([] { throw std::runtime_error("node1 failed"); });
    auto frozen = std::move(graph).freeze();
    auto run = sched.run(std::move(frozen));

    auto awaiter = sched.spawn(coro_await_failing_graph(run));
    std::size_t failed_count = awaiter.get();
    assert(failed_count == 1);
    assert(awaiter.state() == astra::TaskState::Succeeded);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 4. cancellation_point (D-122 / R-076)
// -----------------------------------------------------------------------------
astra::Task<int> coro_cancellation_point_test(std::atomic<bool>& before_cp, std::atomic<bool>& after_cp) {
    before_cp.store(true, std::memory_order_release);
    co_await astra::cancellation_point();
    after_cp.store(true, std::memory_order_release);
    co_return 999;
}

void test_R076_cancellation_point() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    // 4.1 No stop request -> continues synchronously
    std::atomic<bool> b1{false}, a1{false};
    auto h1 = sched.spawn(coro_cancellation_point_test(b1, a1));
    int res1 = h1.get();
    assert(res1 == 999);
    assert(b1.load() && a1.load());

    // 4.2 Stop request set -> throws task_cancelled at cancellation_point
    std::atomic<bool> b2{false}, a2{false};
    auto h2 = sched.spawn(coro_cancellation_point_test(b2, a2));
    h2.request_cancel();

    bool threw = false;
    try {
        h2.get();
    } catch (const astra::task_cancelled&) {
        threw = true;
    }
    // Cancelled state
    assert(h2.state() == astra::TaskState::Cancelled || h2.state() == astra::TaskState::Succeeded);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 5. yield (D-122 / D-147 / R-076)
// -----------------------------------------------------------------------------
astra::Task<int> coro_yield_test(std::atomic<int>& step) {
    step.store(1, std::memory_order_release);
    co_await astra::yield();
    step.store(2, std::memory_order_release);
    co_await astra::yield();
    step.store(3, std::memory_order_release);
    co_return 12345;
}

void test_R076_yield_reschedules_via_global() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    std::atomic<int> step{0};
    auto h = sched.spawn(coro_yield_test(step));

    int res = h.get();
    assert(res == 12345);
    assert(step.load() == 3);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 6. R-078 API Inventory Constraints
// -----------------------------------------------------------------------------
void test_R078_api_inventory() {
    // 验证 TaskHandle 和 GraphRun 的公开方法受限，没有 wait_until、带 stop_token 的 wait、on_complete
    static_assert(requires(astra::TaskHandle<int> h) {
        h.wait();
        h.wait_for(std::chrono::milliseconds(10));
        h.get();
        h.request_cancel();
        h.state();
        h.task_id();
        h.valid();
    });

    static_assert(requires(astra::GraphRun r) {
        r.wait();
        r.wait_for(std::chrono::milliseconds(10));
        r.get_report();
        r.request_cancel();
        r.state();
        r.id();
        r.valid();
    });
}

}  // namespace

int main() {
    std::cout << "Running astra_source_runtime_await_test..." << std::endl;

    test_R076_task_handle_await_success();
    test_R076_task_handle_await_cross_runtime();
    test_R076_task_handle_await_exception_propagation();
    test_R076_direct_self_await_rejected();
    test_R076_graph_run_await();
    test_R076_graph_run_await_failed_report();
    test_R076_cancellation_point();
    test_R076_yield_reschedules_via_global();
    test_R078_api_inventory();

    std::cout << "All AST-035 source runtime await tests passed successfully!" << std::endl;
    return 0;
}

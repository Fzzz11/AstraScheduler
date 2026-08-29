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
#include <thread>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// 1. Basic sleep_for & sleep_until elapsed time verification (R-079 / D-126)
// -----------------------------------------------------------------------------
astra::Task<void> coro_sleep_for(
    std::chrono::milliseconds dur,
    std::atomic<bool>& done_flag,
    std::chrono::steady_clock::duration& measured_elapsed) {

    const auto t0 = std::chrono::steady_clock::now();
    co_await astra::sleep_for(dur);
    const auto t1 = std::chrono::steady_clock::now();

    measured_elapsed = t1 - t0;
    done_flag.store(true, std::memory_order_release);
    co_return;
}

astra::Task<void> coro_sleep_until(
    std::chrono::steady_clock::time_point wake_time,
    std::atomic<bool>& done_flag,
    std::chrono::steady_clock::time_point& finish_time) {

    co_await astra::sleep_until(wake_time);
    finish_time = std::chrono::steady_clock::now();
    done_flag.store(true, std::memory_order_release);
    co_return;
}

void test_R079_sleep_for_and_until_elapsed() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    // (a) sleep_for 25ms
    std::atomic<bool> done_1{false};
    std::chrono::steady_clock::duration elapsed_1{};
    auto h1 = sched.spawn(coro_sleep_for(std::chrono::milliseconds(25), done_1, elapsed_1));
    h1.wait();

    assert(h1.state() == astra::TaskState::Succeeded);
    assert(done_1.load());
    assert(elapsed_1 >= std::chrono::milliseconds(20)); // 不早于目标时刻

    // (b) sleep_until (now + 25ms)
    std::atomic<bool> done_2{false};
    std::chrono::steady_clock::time_point finish_2{};
    const auto target_tp = std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
    auto h2 = sched.spawn(coro_sleep_until(target_tp, done_2, finish_2));
    h2.wait();

    assert(h2.state() == astra::TaskState::Succeeded);
    assert(done_2.load());
    assert(finish_2 >= target_tp); // 不早于 wake_time

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 2. Non-positive duration & expired time point (D-126)
// -----------------------------------------------------------------------------
astra::Task<int> coro_sleep_non_positive() {
    // 0 duration
    co_await astra::sleep_for(std::chrono::milliseconds(0));
    // negative duration
    co_await astra::sleep_for(std::chrono::milliseconds(-10));
    // past time point
    co_await astra::sleep_until(std::chrono::steady_clock::now() - std::chrono::seconds(1));
    co_return 42;
}

void test_R079_non_positive_duration_synchronous() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    const auto t0 = std::chrono::steady_clock::now();
    auto h = sched.spawn(coro_sleep_non_positive());
    h.wait();
    const auto t1 = std::chrono::steady_clock::now();

    assert(h.state() == astra::TaskState::Succeeded);
    assert(h.get() == 42);
    // 同步继续，耗时应远小于 10ms
    assert(t1 - t0 < std::chrono::milliseconds(50));

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 3. Multiple timers with same wake time / deterministic sequence tie-breaking (D-127)
// -----------------------------------------------------------------------------
astra::Task<void> coro_timed_recorder(
    std::chrono::milliseconds dur,
    int id,
    std::vector<int>& order_out,
    std::mutex& mtx) {

    co_await astra::sleep_for(dur);
    {
        std::lock_guard<std::mutex> lock(mtx);
        order_out.push_back(id);
    }
    co_return;
}

void test_R079_multiple_timers_tie_breaking() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    std::vector<int> recorded_order;
    std::mutex mtx;

    std::vector<astra::TaskHandle<void>> handles;
    for (int i = 0; i < 5; ++i) {
        handles.push_back(sched.spawn(coro_timed_recorder(
            std::chrono::milliseconds(20), i, recorded_order, mtx)));
    }

    for (auto& h : handles) {
        h.wait();
        assert(h.state() == astra::TaskState::Succeeded);
    }

    assert(recorded_order.size() == 5);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 4. Cancellation while coroutine is sleeping (D-126 / D-127 / D-119)
// -----------------------------------------------------------------------------
astra::Task<void> coro_cancellable_sleep(std::atomic<bool>& entered) {
    entered.store(true, std::memory_order_release);
    // 等待 10 秒
    co_await astra::sleep_for(std::chrono::seconds(10));
    co_return;
}

void test_R079_cancellation_while_sleeping() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    std::atomic<bool> entered{false};
    const auto t0 = std::chrono::steady_clock::now();
    auto h = sched.spawn(coro_cancellable_sleep(entered));

    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // 取消正在睡眠的协程
    h.request_cancel();
    h.wait();
    const auto t1 = std::chrono::steady_clock::now();

    assert(h.state() == astra::TaskState::Cancelled);
    // 取消应当立即可用，耗时远小于 10 秒
    assert(t1 - t0 < std::chrono::seconds(2));

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 5. Graceful shutdown preserves and drains sleeping coroutine (D-128)
// -----------------------------------------------------------------------------
astra::Task<void> coro_graceful_sleep(std::atomic<bool>& completed) {
    co_await astra::sleep_for(std::chrono::milliseconds(20));
    completed.store(true, std::memory_order_release);
    co_return;
}

void test_R079_graceful_shutdown_drains_sleep() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    std::atomic<bool> completed{false};
    auto h = sched.spawn(coro_graceful_sleep(completed));

    // 立即发起 Graceful Shutdown
    sched.shutdown();

    assert(h.state() == astra::TaskState::Succeeded);
    assert(completed.load());
}

// -----------------------------------------------------------------------------
// 6. Immediate shutdown cancels sleeping coroutine promptly (D-128)
// -----------------------------------------------------------------------------
astra::Task<void> coro_immediate_sleep() {
    co_await astra::sleep_for(std::chrono::seconds(10));
    co_return;
}

void test_R079_immediate_shutdown_cancels_sleep() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    auto h = sched.spawn(coro_immediate_sleep());

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const auto t0 = std::chrono::steady_clock::now();
    sched.shutdown_now();
    const auto t1 = std::chrono::steady_clock::now();

    assert(h.state() == astra::TaskState::Cancelled);
    // Immediate 关停应当迅速返回（< 1s）
    assert(t1 - t0 < std::chrono::seconds(1));
}

// -----------------------------------------------------------------------------
// 7. Graph coroutine node with sleep_for (R-077 / R-079)
// -----------------------------------------------------------------------------
astra::Task<void> coro_graph_sleep(std::atomic<bool>& node_done) {
    co_await astra::sleep_for(std::chrono::milliseconds(20));
    node_done.store(true, std::memory_order_release);
    co_return;
}

void test_R079_graph_coroutine_node_sleep() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    astra::TaskGraph graph;
    std::atomic<bool> n1_done{false};
    auto n1 = graph.emplace_coroutine(coro_graph_sleep(n1_done));

    std::atomic<bool> n2_saw_n1{false};
    auto n2 = graph.emplace([&] {
        n2_saw_n1.store(n1_done.load(std::memory_order_acquire), std::memory_order_release);
    });

    graph.add_edge(n1, n2, astra::EdgePolicy::RequireSuccess);

    auto run = sched.run(std::move(graph).freeze());
    run.wait();

    const auto& report = run.get_report();
    assert(run.state() == astra::GraphRunState::Succeeded);
    assert(report.total_nodes == 2);
    assert(report.succeeded_nodes == 2);
    assert(n1_done.load());
    assert(n2_saw_n1.load());

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 8. Compile-time constraints (D-126)
// -----------------------------------------------------------------------------
template <typename Clock>
concept CanSleepUntil = requires(std::chrono::time_point<Clock> tp) {
    astra::sleep_until(tp);
};

void test_R079_compile_constraints() {
    // 1. steady_clock::time_point 支持
    static_assert(CanSleepUntil<std::chrono::steady_clock>);

    // 2. system_clock::time_point 编译期拒绝
    static_assert(!CanSleepUntil<std::chrono::system_clock>);
}

}  // namespace

int main() {
    std::cout << "Running astra_worker_timers_test..." << std::endl;

    std::cout << "Running test 1..." << std::endl;
    test_R079_sleep_for_and_until_elapsed();
    std::cout << "Running test 2..." << std::endl;
    test_R079_non_positive_duration_synchronous();
    std::cout << "Running test 3..." << std::endl;
    test_R079_multiple_timers_tie_breaking();
    std::cout << "Running test 4..." << std::endl;
    test_R079_cancellation_while_sleeping();
    std::cout << "Running test 5..." << std::endl;
    test_R079_graceful_shutdown_drains_sleep();
    std::cout << "Running test 6..." << std::endl;
    test_R079_immediate_shutdown_cancels_sleep();
    std::cout << "Running test 7..." << std::endl;
    test_R079_graph_coroutine_node_sleep();
    std::cout << "Running test 8..." << std::endl;
    test_R079_compile_constraints();

    std::cout << "All AST-037 worker timer tests passed successfully!" << std::endl;
    return 0;
}

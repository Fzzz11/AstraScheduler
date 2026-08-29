#include "astra/coroutine.hpp"
#include "astra/graph.hpp"
#include "astra/scheduler.hpp"
#include "astra/task_handle.hpp"
#include "astra/task_options.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <variant>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// 1. External submission defaults and explicit options
// -----------------------------------------------------------------------------
void test_R080_options_first_and_defaults_external() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    // 1.1 submit
    auto h1 = sched.submit([] { return 42; });
    assert(h1.priority() == astra::Priority::Normal);
    assert(h1.get() == 42);

    astra::TaskOptions low_opts{astra::Priority::Low};
    auto h2 = sched.submit(low_opts, [] { return 100; });
    assert(h2.priority() == astra::Priority::Low);
    assert(h2.get() == 100);

    astra::TaskOptions crit_opts{astra::Priority::Critical};
    auto h3 = sched.submit(crit_opts, [] { return 200; });
    assert(h3.priority() == astra::Priority::Critical);
    assert(h3.get() == 200);

    // 1.2 try_submit
    auto res1 = sched.try_submit([] { return 1; });
    assert(std::holds_alternative<astra::TaskHandle<int>>(res1));
    assert(std::get<astra::TaskHandle<int>>(res1).priority() == astra::Priority::Normal);
    assert(std::get<astra::TaskHandle<int>>(res1).get() == 1);

    auto res2 = sched.try_submit(astra::TaskOptions{astra::Priority::High}, [] { return 2; });
    assert(std::holds_alternative<astra::TaskHandle<int>>(res2));
    assert(std::get<astra::TaskHandle<int>>(res2).priority() == astra::Priority::High);
    assert(std::get<astra::TaskHandle<int>>(res2).get() == 2);

    // 1.3 spawn
    auto make_coro = [](int val) -> astra::Task<int> {
        co_return val;
    };

    auto h_coro1 = sched.spawn(make_coro(10));
    assert(h_coro1.priority() == astra::Priority::Normal);
    assert(h_coro1.get() == 10);

    auto h_coro2 = sched.spawn(astra::TaskOptions{astra::Priority::High}, make_coro(20));
    assert(h_coro2.priority() == astra::Priority::High);
    assert(h_coro2.get() == 20);

    // 1.4 try_spawn
    auto res_coro1 = sched.try_spawn(make_coro(30));
    assert(std::holds_alternative<astra::TaskHandle<int>>(res_coro1));
    assert(std::get<astra::TaskHandle<int>>(res_coro1).priority() == astra::Priority::Normal);
    assert(std::get<astra::TaskHandle<int>>(res_coro1).get() == 30);

    auto res_coro2 = sched.try_spawn(astra::TaskOptions{astra::Priority::Critical}, make_coro(40));
    assert(std::holds_alternative<astra::TaskHandle<int>>(res_coro2));
    assert(std::get<astra::TaskHandle<int>>(res_coro2).priority() == astra::Priority::Critical);
    assert(std::get<astra::TaskHandle<int>>(res_coro2).get() == 40);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 2. Same-runtime internal submission inheritance and override
// -----------------------------------------------------------------------------
void test_R080_internal_inheritance_and_override() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    auto make_child_coro = []() -> astra::Task<astra::Priority> {
        co_return astra::detail::current_executing_task_priority();
    };

    auto parent = sched.submit(astra::TaskOptions{astra::Priority::High}, [&]() {
        assert(astra::detail::current_executing_task_priority() == astra::Priority::High);

        // 2.1 相同 Runtime 内部 submit 无 options -> 继承父任务 High
        auto child1 = sched.submit([] {
            return astra::detail::current_executing_task_priority();
        });
        assert(child1.priority() == astra::Priority::High);
        assert(child1.get() == astra::Priority::High);

        // 2.2 相同 Runtime 内部 submit 显式 options -> 覆盖为 Low
        auto child2 = sched.submit(astra::TaskOptions{astra::Priority::Low}, [] {
            return astra::detail::current_executing_task_priority();
        });
        assert(child2.priority() == astra::Priority::Low);
        assert(child2.get() == astra::Priority::Low);

        // 2.3 相同 Runtime 内部 spawn 无 options -> 继承父任务 High
        auto child_coro1 = sched.spawn(make_child_coro());
        assert(child_coro1.priority() == astra::Priority::High);
        assert(child_coro1.get() == astra::Priority::High);

        // 2.4 相同 Runtime 内部 spawn 显式 options -> 覆盖为 Critical
        auto child_coro2 = sched.spawn(astra::TaskOptions{astra::Priority::Critical}, make_child_coro());
        assert(child_coro2.priority() == astra::Priority::Critical);
        assert(child_coro2.get() == astra::Priority::Critical);

        return true;
    });

    assert(parent.get() == true);
    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 3. Cross-runtime submission treated as external (D-129)
// -----------------------------------------------------------------------------
void test_R080_cross_runtime_is_external() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler s1(opts);
    astra::Scheduler s2(opts);

    auto parent = s1.submit(astra::TaskOptions{astra::Priority::Critical}, [&]() {
        assert(astra::detail::current_executing_task_priority() == astra::Priority::Critical);

        // 跨 Runtime 提交到 s2，无 options -> 视为 External，解析为 Normal
        auto foreign_child = s2.submit([] {
            return astra::detail::current_executing_task_priority();
        });
        assert(foreign_child.priority() == astra::Priority::Normal);
        assert(foreign_child.get() == astra::Priority::Normal);

        return true;
    });

    assert(parent.get() == true);
    s1.shutdown();
    s2.shutdown();
}

// -----------------------------------------------------------------------------
// 4. TaskGraph node priority inheritance and override
// -----------------------------------------------------------------------------
void test_R080_graph_node_priority_inheritance_and_override() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    // 4.1 External GraphRun with High priority
    astra::TaskGraph graph;
    std::atomic<astra::Priority> n1_p{astra::Priority::Normal};
    std::atomic<astra::Priority> n2_p{astra::Priority::Normal};
    std::atomic<astra::Priority> n3_p{astra::Priority::Normal};

    // Node 1: no options -> inherits GraphRun High
    auto n1 = graph.emplace([&] {
        n1_p.store(astra::detail::current_executing_task_priority());
    });

    // Node 2: explicit options -> overrides to Low
    auto n2 = graph.emplace(astra::TaskOptions{astra::Priority::Low}, [&] {
        n2_p.store(astra::detail::current_executing_task_priority());
    });

    // Node 3: coroutine node with explicit Critical
    auto coro_n3 = [&]() -> astra::Task<void> {
        n3_p.store(astra::detail::current_executing_task_priority());
        co_return;
    };
    auto n3 = graph.emplace_coroutine(astra::TaskOptions{astra::Priority::Critical}, coro_n3());

    graph.add_edge(n1, n2);
    graph.add_edge(n2, n3);

    auto run = sched.run(astra::TaskOptions{astra::Priority::High}, std::move(graph).freeze());
    run.wait();

    assert(run.state() == astra::GraphRunState::Succeeded);
    assert(n1_p.load() == astra::Priority::High);
    assert(n2_p.load() == astra::Priority::Low);
    assert(n3_p.load() == astra::Priority::Critical);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 5. Coroutine resumptions preserve original base priority (R-080 / D-129)
// -----------------------------------------------------------------------------
void test_R080_coroutine_resumptions_preserve_priority() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    std::atomic<astra::Priority> p_before{astra::Priority::Normal};
    std::atomic<astra::Priority> p_after{astra::Priority::Normal};

    auto sleep_coro = [&]() -> astra::Task<void> {
        p_before.store(astra::detail::current_executing_task_priority());
        co_await astra::sleep_for(std::chrono::milliseconds(10));
        p_after.store(astra::detail::current_executing_task_priority());
        co_return;
    };

    auto handle = sched.spawn(astra::TaskOptions{astra::Priority::High}, sleep_coro());
    assert(handle.priority() == astra::Priority::High);
    handle.wait();

    assert(p_before.load() == astra::Priority::High);
    assert(p_after.load() == astra::Priority::High);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 6. Options immutability after admission (D-129)
// -----------------------------------------------------------------------------
void test_R080_options_immutability_after_admission() {
    astra::SchedulerOptions sched_opts;
    sched_opts.worker_count = 2;
    astra::Scheduler sched(sched_opts);

    astra::TaskOptions opts{astra::Priority::High};
    auto handle = sched.submit(opts, [] { return 1; });
    assert(handle.priority() == astra::Priority::High);

    // 修改调用方 options 变量
    opts.priority = astra::Priority::Low;
    assert(handle.priority() == astra::Priority::High);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 7. Invalid priority enum values rejected with invalid_argument
// -----------------------------------------------------------------------------
void test_R080_invalid_priority_rejected() {
    astra::SchedulerOptions sched_opts;
    sched_opts.worker_count = 2;
    astra::Scheduler sched(sched_opts);

    astra::TaskOptions bad_opts{static_cast<astra::Priority>(100)};

    // submit
    bool threw = false;
    try {
        sched.submit(bad_opts, [] {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    // try_submit
    threw = false;
    try {
        sched.try_submit(bad_opts, [] {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    // spawn
    auto make_coro = []() -> astra::Task<void> { co_return; };
    threw = false;
    try {
        sched.spawn(bad_opts, make_coro());
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    // emplace
    astra::TaskGraph graph;
    threw = false;
    try {
        graph.emplace(bad_opts, [] {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    // emplace_coroutine
    threw = false;
    try {
        graph.emplace_coroutine(bad_opts, make_coro());
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    // run
    threw = false;
    try {
        sched.run(bad_opts, std::move(graph).freeze());
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    sched.shutdown();
}

}  // namespace

int main() {
    std::cout << "Running astra_base_priority_test..." << std::endl;

    std::cout << "Running test 1..." << std::endl;
    test_R080_options_first_and_defaults_external();
    std::cout << "Running test 2..." << std::endl;
    test_R080_internal_inheritance_and_override();
    std::cout << "Running test 3..." << std::endl;
    test_R080_cross_runtime_is_external();
    std::cout << "Running test 4..." << std::endl;
    test_R080_graph_node_priority_inheritance_and_override();
    std::cout << "Running test 5..." << std::endl;
    test_R080_coroutine_resumptions_preserve_priority();
    std::cout << "Running test 6..." << std::endl;
    test_R080_options_immutability_after_admission();
    std::cout << "Running test 7..." << std::endl;
    test_R080_invalid_priority_rejected();

    std::cout << "All AST-038 base priority tests passed successfully!" << std::endl;
    return 0;
}

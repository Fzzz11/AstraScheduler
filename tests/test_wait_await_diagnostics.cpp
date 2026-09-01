// AST-048 / R-060 / R-096 / D-149 — wait/await 与 unobserved failure 诊断测试。
// 验证 metrics counters/histograms、trace WaitBegin/End 与 Await 三事件、
// 离线 edge 重建、Metrics Off 快速路径与 unobserved failure 不改变执行。

#include "astra/coroutine.hpp"
#include "astra/graph.hpp"
#include "astra/metrics.hpp"
#include "astra/scheduler.hpp"
#include "astra/task_handle.hpp"
#include "astra/trace.hpp"
#include "astra/trace_export.hpp"
#include "observability/trace_collector.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

using astra::RuntimeId;

template <typename Predicate> void wait_until(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

// -----------------------------------------------------------------------------
// 1. 外部线程 wait/wait_for：task_wait_calls、thread_wait_duration、timeouts (R-096)
// -----------------------------------------------------------------------------
void test_R096_external_thread_wait_metrics() {
    astra::SchedulerOptions opts{};
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Detailed;
    astra::Scheduler sched(opts);

    std::promise<void> release;
    auto release_future = release.get_future().share();
    auto h = sched.submit([release_future] {
        release_future.wait();
        return 1;
    });

    // 已完成即时等待也计 call（D-149）。
    auto h2 = sched.submit([] { return 2; });
    h2.wait();
    auto snap_instant = sched.metrics_snapshot();
    assert(snap_instant.counters.task_wait_calls >= 1);
    assert(snap_instant.histograms.thread_wait_duration.count >= 1);

    // wait_for 超时：真实返回 TimedOut 计 wait_for_timeouts。
    assert(h.wait_for(1ms) == astra::WaitResult::TimedOut);
    auto snap_timeout = sched.metrics_snapshot();
    assert(snap_timeout.counters.wait_for_timeouts >= 1);

    release.set_value();
    assert(h.get() == 1);

    std::cout << "[PASS] test_R096_external_thread_wait_metrics" << std::endl;
}

// -----------------------------------------------------------------------------
// 2. 同 Runtime Helping wait：counter 与 helping_wait_duration (R-096)
// -----------------------------------------------------------------------------
void test_R096_same_runtime_helping_wait() {
    astra::SchedulerOptions opts{};
    // Keep one worker available while the gated target and the waiting helper
    // are both active; sanitizer scheduling can otherwise starve the helper.
    opts.worker_count = 3;
    opts.metrics_level = astra::MetricsLevel::Detailed;
    astra::Scheduler sched(opts);

    std::promise<void> release;
    auto release_future = release.get_future().share();
    auto target = sched.submit([release_future] {
        release_future.wait();
        return 7;
    });

    // worker 任务内等待另一任务的 Handle：same-runtime helping wait。
    auto helper = sched.submit([&target] {
        target.wait();
        return target.get();
    });

    wait_until([&] { return sched.metrics_snapshot().counters.same_runtime_helping_waits >= 1; });
    std::this_thread::sleep_for(30ms);
    release.set_value();
    assert(helper.get() == 7);

    auto snap = sched.metrics_snapshot();
    assert(snap.counters.same_runtime_helping_waits >= 1);
    assert(snap.histograms.helping_wait_duration.count >= 1);
    assert(snap.histograms.helping_wait_duration.sum_ns >= 25'000'000);

    std::cout << "[PASS] test_R096_same_runtime_helping_wait" << std::endl;
}

// -----------------------------------------------------------------------------
// 3. Cross-runtime helping wait (R-096 / D-051)
// -----------------------------------------------------------------------------
void test_R096_cross_runtime_helping_wait() {
    astra::SchedulerOptions opts_a{};
    opts_a.worker_count = 2;
    opts_a.metrics_level = astra::MetricsLevel::Detailed;
    astra::Scheduler a(opts_a);
    astra::SchedulerOptions opts_b{};
    opts_b.worker_count = 2;
    opts_b.metrics_level = astra::MetricsLevel::Detailed;
    astra::Scheduler b(opts_b);

    std::promise<void> release;
    auto release_future = release.get_future().share();
    auto target = b.submit([release_future] {
        release_future.wait();
        return 9;
    });

    // A 的 worker 等待 B 的任务：cross-runtime helping（source helper 归属 A）。
    auto helper = a.submit([&target] {
        target.wait();
        return target.get();
    });

    wait_until([&] { return a.metrics_snapshot().counters.cross_runtime_helping_waits >= 1; });
    release.set_value();
    assert(helper.get() == 9);

    auto snap = a.metrics_snapshot();
    assert(snap.counters.cross_runtime_helping_waits >= 1);

    std::cout << "[PASS] test_R096_cross_runtime_helping_wait" << std::endl;
}

// -----------------------------------------------------------------------------
// 4. Coroutine await：registrations 与 await duration (R-096)
// -----------------------------------------------------------------------------
void test_R096_coroutine_await_metrics() {
    astra::SchedulerOptions opts{};
    opts.worker_count = 3;
    opts.metrics_level = astra::MetricsLevel::Detailed;
    astra::Scheduler sched(opts);

    std::promise<void> release;
    auto release_future = release.get_future().share();
    auto inner = sched.submit([release_future] {
        release_future.wait();
        return 42;
    });

    auto coro_fn = [&](astra::Scheduler&) -> astra::Task<int> {
        co_await inner; // TaskHandleAwaiter
        co_return 1;
    };
    auto outer = sched.spawn(coro_fn(sched));

    wait_until(
        [&] { return sched.metrics_snapshot().counters.coroutine_await_registrations >= 1; });
    std::this_thread::sleep_for(20ms);
    release.set_value();
    assert(outer.get() == 1);

    auto snap = sched.metrics_snapshot();
    assert(snap.counters.coroutine_await_registrations >= 1);
    assert(snap.histograms.coroutine_await_duration.count >= 1);
    assert(snap.histograms.coroutine_await_duration.sum_ns >= 15'000'000);

    std::cout << "[PASS] test_R096_coroutine_await_metrics" << std::endl;
}

// -----------------------------------------------------------------------------
// 5. Self-wait / depth rejection 计数（不改变语义，异常照常抛出）(R-096)
// -----------------------------------------------------------------------------
void test_R096_self_and_depth_rejections() {
    // Direct self-wait：任务 get 自身 Handle（R-052 模式）→ rejection 计数。
    {
        astra::SchedulerOptions opts{};
        opts.worker_count = 1;
        opts.metrics_level = astra::MetricsLevel::Basic;
        astra::Scheduler sched(opts);

        std::shared_ptr<astra::TaskHandle<int>> self_holder =
            std::make_shared<astra::TaskHandle<int>>();
        std::promise<void> ready;
        std::shared_future<void> ready_fut = ready.get_future().share();

        auto h = sched.submit([self_holder, ready_fut]() -> int {
            ready_fut.wait();
            try {
                (void)self_holder->get(); // direct self-wait
            } catch (const std::logic_error&) {
                // 语义不变：异常可被任务捕获（R-096 不改变执行）。
            }
            return 1;
        });
        *self_holder = h;
        ready.set_value();
        assert(h.get() == 1);

        auto snap = sched.metrics_snapshot();
        assert(snap.counters.direct_self_wait_rejections >= 1);
    }

    // Helping depth rejection：max_helping_depth=1。worker 的 helping guard
    // （depth=1）内执行 victim，victim 再等待未完成任务 → 深度超限拒绝。
    {
        astra::SchedulerOptions opts{};
        opts.worker_count = 2;
        opts.max_helping_depth = 1;
        opts.metrics_level = astra::MetricsLevel::Basic;
        astra::Scheduler sched(opts);

        std::atomic<bool> third_gate{true};
        auto third = sched.submit([&third_gate]() -> int {
            while (third_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return 1;
        });

        std::shared_ptr<astra::TaskHandle<int>> victim_holder =
            std::make_shared<astra::TaskHandle<int>>();
        std::promise<void> ready;
        std::shared_future<void> ready_fut = ready.get_future().share();
        auto helper = sched.submit([victim_holder, ready_fut]() -> int {
            ready_fut.wait();
            return victim_holder->get();
        });

        auto victim = sched.submit([&third]() -> int {
            try {
                return third.get(); // 在 helping guard（depth=1）内等待未完成任务
            } catch (const astra::helping_depth_exceeded&) {
                return -1;
            }
        });
        *victim_holder = victim;
        ready.set_value();

        assert(helper.get() == -1);
        third_gate.store(false, std::memory_order_release);

        auto snap = sched.metrics_snapshot();
        assert(snap.counters.helping_depth_rejections >= 1);
    }

    std::cout << "[PASS] test_R096_self_and_depth_rejections" << std::endl;
}

// -----------------------------------------------------------------------------
// 6. Graph wait 计数 (R-096)
// -----------------------------------------------------------------------------
void test_R096_graph_wait_calls() {
    astra::SchedulerOptions opts{};
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Detailed;
    astra::Scheduler sched(opts);

    bool ran = false;
    astra::TaskGraph graph;
    (void)graph.emplace([&ran] { ran = true; });
    auto run = sched.run(std::move(graph).freeze());
    run.wait(); // 外部线程 graph wait

    auto snap = sched.metrics_snapshot();
    assert(ran);
    assert(snap.counters.graph_wait_calls >= 1);

    std::cout << "[PASS] test_R096_graph_wait_calls" << std::endl;
}

// -----------------------------------------------------------------------------
// 7. Trace：离线重建 wait/await edge（source/target identity 配对）(R-096)
// -----------------------------------------------------------------------------
void test_R096_trace_wait_await_edges_offline_rebuild() {
    auto collector = std::make_shared<astra::TraceCollector>();

    astra::SchedulerOptions opts{};
    opts.worker_count = 2;
    opts.trace_collector = collector;
    astra::Scheduler sched(opts);

    auto capture = collector->start_capture(); // 在等待发生前开始 recording

    auto target = sched.submit([] {
        std::this_thread::sleep_for(15ms);
        return 5;
    });

    auto coro_fn = [&](astra::Scheduler&) -> astra::Task<int> {
        co_await target;
        co_return target.get();
    };
    auto outer = sched.spawn(coro_fn(sched));
    auto helper = sched.submit([&target] {
        target.wait();
        return target.get();
    });
    assert(outer.get() == 5);
    assert(helper.get() == 5);

    auto snap = capture.stop();
    assert(snap.events().size() > 0);

    std::size_t begins = 0, ends = 0, armed = 0, triggered = 0, resumed = 0;
    for (const auto& ev : snap.events()) {
        switch (static_cast<astra::TraceEventKind>(ev.kind)) {
        case astra::TraceEventKind::WaitBegin:
            ++begins;
            break;
        case astra::TraceEventKind::WaitEnd:
            ++ends;
            break;
        case astra::TraceEventKind::AwaitArmed:
            ++armed;
            break;
        case astra::TraceEventKind::AwaitTriggered:
            ++triggered;
            break;
        case astra::TraceEventKind::AwaitResumed:
            ++resumed;
            break;
        default:
            break;
        }
    }
    assert(begins >= 2); // helper 的 helping wait + outer 的 await 上下文
    assert(ends == begins);
    assert(armed >= 1 && triggered >= 1 && resumed >= 1);

    // 离线重建 edge：WaitEnd 与配对 WaitBegin 携带相同 target identity。
    for (const auto& ev : snap.events()) {
        if (static_cast<astra::TraceEventKind>(ev.kind) == astra::TraceEventKind::WaitBegin) {
            assert(ev.target_runtime_id != 0 && ev.target_task_id != 0);
        }
        if (static_cast<astra::TraceEventKind>(ev.kind) == astra::TraceEventKind::AwaitArmed) {
            assert(ev.target_task_id != 0);
        }
    }

    // 导出为有效 JSON 且事件完整（AST-047 协作边界）。
    std::ostringstream out;
    auto result = astra::write_chrome_trace(snap, out);
    assert(result.trace_complete);
    assert(out.str().find("\"name\":\"wait_begin\"") != std::string::npos);
    assert(out.str().find("\"name\":\"await_resumed\"") != std::string::npos);

    std::cout << "[PASS] test_R096_trace_wait_await_edges_offline_rebuild" << std::endl;
}

// -----------------------------------------------------------------------------
// 8. R-060：unobserved failure 计数、Off 无隐藏输出、不终止进程 (R-060)
// -----------------------------------------------------------------------------
void test_R060_unobserved_failure_diagnostics() {
    // Basic：未 get 的失败任务 → unobserved_failures 计数。
    {
        astra::SchedulerOptions opts{};
        opts.worker_count = 2;
        opts.metrics_level = astra::MetricsLevel::Basic;
        astra::Scheduler sched(opts);
        {
            auto h = sched.submit([]() -> int { throw std::runtime_error("boom"); });
            h.wait(); // wait 不标记 observed
        } // Handle 释放：unobserved
        std::this_thread::sleep_for(20ms);
        auto snap = sched.metrics_snapshot();
        assert(snap.counters.unobserved_failures >= 1);
    }

    // Off：无隐藏计数可见（enabled=false 且不崩溃/不终止）。
    {
        astra::SchedulerOptions opts{};
        opts.worker_count = 2;
        opts.metrics_level = astra::MetricsLevel::Off;
        astra::Scheduler sched(opts);
        {
            auto h = sched.submit([]() -> int { throw std::runtime_error("boom off"); });
            h.wait();
        }
        std::this_thread::sleep_for(20ms);
        auto snap = sched.metrics_snapshot();
        assert(!snap.enabled);
    }

    // Trace 可用时尽力发出 UnobservedFailure 事件。
    {
        auto collector = std::make_shared<astra::TraceCollector>();
        astra::SchedulerOptions opts{};
        opts.worker_count = 2;
        opts.trace_collector = collector;
        astra::Scheduler sched(opts);
        auto capture = collector->start_capture(); // 在 Handle 释放前开始 recording
        {
            auto h = sched.submit([]() -> int { throw std::runtime_error("boom trace"); });
            h.wait();
        }
        std::this_thread::sleep_for(20ms);
        auto snap = capture.stop();
        bool seen = false;
        for (const auto& ev : snap.events()) {
            if (static_cast<astra::TraceEventKind>(ev.kind) ==
                astra::TraceEventKind::UnobservedFailure) {
                seen = true;
            }
        }
        assert(seen);
    }

    std::cout << "[PASS] test_R060_unobserved_failure_diagnostics" << std::endl;
}

} // namespace

int main() {
    std::cout << "Running astra_wait_await_diagnostics_test..." << std::endl;

    test_R096_external_thread_wait_metrics();
    test_R096_same_runtime_helping_wait();
    test_R096_cross_runtime_helping_wait();
    test_R096_coroutine_await_metrics();
    test_R096_self_and_depth_rejections();
    test_R096_graph_wait_calls();
    test_R096_trace_wait_await_edges_offline_rebuild();
    test_R060_unobserved_failure_diagnostics();

    std::cout << "All AST-048 wait/await diagnostics tests passed successfully!" << std::endl;
    return 0;
}

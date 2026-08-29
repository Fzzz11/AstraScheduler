// AST-056 / R-073 / R-074 / R-076 — Coroutine 帧生命周期回归测试。
// 高频 spawn + co_await(yield/sleep/TaskHandle) + 主线程 get() 循环：
// 在挂起方 resume() 返回与 requeue 后的快速完成/销毁竞争下，帧必须恰好销毁一次。

#include <astra/coroutine.hpp>
#include <astra/scheduler.hpp>
#include <astra/task_handle.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>

namespace {

using namespace std::chrono_literals;

// 1. spawn + co_await yield + 主线程 get()：挂起方 resume() 返回后不得触碰已移交帧。
void test_yield_coroutine_frame_lifetime() {
    astra::SchedulerOptions opts{};
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Off;
    astra::Scheduler sched(opts);
    for (int i = 0; i < 512; ++i) {
        auto coro = [i](astra::Scheduler&) -> astra::Task<std::uint64_t> {
            co_await astra::yield();
            co_return static_cast<std::uint64_t>(i) + 1;
        };
        auto h = sched.spawn(coro(sched));
        if (h.get() != static_cast<std::uint64_t>(i) + 1) {
            std::abort();
        }
    }
    std::cout << "[PASS] test_yield_coroutine_frame_lifetime" << std::endl;
}

// 2. spawn + sleep_for + 主线程 get()：定时器 resume 路径的帧所有权。
void test_sleep_coroutine_frame_lifetime() {
    astra::SchedulerOptions opts{};
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Off;
    astra::Scheduler sched(opts);
    for (int i = 0; i < 256; ++i) {
        auto coro = [i](astra::Scheduler&) -> astra::Task<std::uint64_t> {
            co_await astra::sleep_for(1ms);
            co_return static_cast<std::uint64_t>(i) + 1;
        };
        auto h = sched.spawn(coro(sched));
        if (h.get() != static_cast<std::uint64_t>(i) + 1) {
            std::abort();
        }
    }
    std::cout << "[PASS] test_sleep_coroutine_frame_lifetime" << std::endl;
}

// 3. spawn + co_await TaskHandle + 主线程 get()：AwaitHandshake resume 路径。
void test_task_handle_await_frame_lifetime() {
    astra::SchedulerOptions opts{};
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Off;
    astra::Scheduler sched(opts);
    for (int i = 0; i < 256; ++i) {
        auto target = sched.submit([i] { return static_cast<std::uint64_t>(i) + 1; });
        auto coro = [&target](astra::Scheduler&) -> astra::Task<std::uint64_t> {
            co_await target;
            co_return target.get();
        };
        auto h = sched.spawn(coro(sched));
        if (h.get() != static_cast<std::uint64_t>(i) + 1) {
            std::abort();
        }
    }
    std::cout << "[PASS] test_task_handle_await_frame_lifetime" << std::endl;
}

// 4. 嵌套 handoff：yield 之后再次 yield（多代移交链）。
void test_nested_yield_handoff_chain() {
    astra::SchedulerOptions opts{};
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Off;
    astra::Scheduler sched(opts);
    for (int i = 0; i < 256; ++i) {
        auto coro = [i](astra::Scheduler&) -> astra::Task<std::uint64_t> {
            co_await astra::yield();
            co_await astra::yield();
            co_await astra::yield();
            co_return static_cast<std::uint64_t>(i) + 1;
        };
        auto h = sched.spawn(coro(sched));
        if (h.get() != static_cast<std::uint64_t>(i) + 1) {
            std::abort();
        }
    }
    std::cout << "[PASS] test_nested_yield_handoff_chain" << std::endl;
}

}  // namespace

int main() {
    std::cout << "Running astra_coroutine_frame_lifetime_test..." << std::endl;

    test_yield_coroutine_frame_lifetime();
    test_sleep_coroutine_frame_lifetime();
    test_task_handle_await_frame_lifetime();
    test_nested_yield_handoff_chain();

    std::cout << "All AST-056 coroutine frame lifetime tests passed successfully!" << std::endl;
    return 0;
}

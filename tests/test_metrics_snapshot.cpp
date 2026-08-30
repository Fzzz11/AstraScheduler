#include "astra/coroutine.hpp"
#include "astra/graph.hpp"
#include "astra/metrics.hpp"
#include "astra/scheduler.hpp"
#include "astra/task_handle.hpp"
#include "astra/task_options.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>
#include "testing/test_seam.hpp"

using namespace std::chrono_literals;

namespace {

// -----------------------------------------------------------------------------
// 1. Log2Histogram Bucket Index & Boundary Verification (R-085 / D-137)
// -----------------------------------------------------------------------------
void test_R085_log2_histogram_bucket_boundaries() {
    using astra::Log2Histogram;

    // Bucket 0: 0ns, 1ns
    assert(Log2Histogram::bucket_for_ns(0) == 0);
    assert(Log2Histogram::bucket_for_ns(1) == 0);

    // Bucket 1: 2ns, 3ns ([2^1, 2^2))
    assert(Log2Histogram::bucket_for_ns(2) == 1);
    assert(Log2Histogram::bucket_for_ns(3) == 1);

    // Bucket 2: 4..7ns ([2^2, 2^3))
    assert(Log2Histogram::bucket_for_ns(4) == 2);
    assert(Log2Histogram::bucket_for_ns(7) == 2);

    // Bucket 3: 8..15ns ([2^3, 2^4))
    assert(Log2Histogram::bucket_for_ns(8) == 3);
    assert(Log2Histogram::bucket_for_ns(15) == 3);

    // Bucket 10: 1024..2047ns (~1us)
    assert(Log2Histogram::bucket_for_ns(1024) == 10);
    assert(Log2Histogram::bucket_for_ns(2047) == 10);

    // Bucket 20: ~1ms
    assert(Log2Histogram::bucket_for_ns(1ULL << 20) == 20);
    assert(Log2Histogram::bucket_for_ns((1ULL << 21) - 1) == 20);

    // Bucket 30: ~1s
    assert(Log2Histogram::bucket_for_ns(1ULL << 30) == 30);
    assert(Log2Histogram::bucket_for_ns((1ULL << 31) - 1) == 30);

    // Bucket 62: [2^62, 2^63 - 1]
    assert(Log2Histogram::bucket_for_ns(1ULL << 62) == 62);
    assert(Log2Histogram::bucket_for_ns((1ULL << 63) - 1) == 62);

    // Bucket 63: 2^63 and overflow up to UINT64_MAX
    assert(Log2Histogram::bucket_for_ns(1ULL << 63) == 63);
    assert(Log2Histogram::bucket_for_ns(std::numeric_limits<std::uint64_t>::max()) == 63);

    std::cout << "[PASS] test_R085_log2_histogram_bucket_boundaries" << std::endl;
}

// -----------------------------------------------------------------------------
// 2. MetricsLevel::Detailed 延迟直方图统计 (R-085 / D-137)
// -----------------------------------------------------------------------------
void test_R085_detailed_histograms() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Detailed;
    opts.external_pending_capacity = 2;
    opts.external_backpressure = astra::ExternalBackpressure::Block;
    astra::Scheduler sched(opts);

    // (a) 普通任务：ready_queue_wait, execution_segment, task_wall_time
    {
        auto h = sched.submit([] {
            std::this_thread::sleep_for(5ms);
            return 100;
        });
        assert(h.get() == 100);
    }

    auto snap = sched.metrics_snapshot();
    assert(snap.enabled);
    assert(snap.metrics_level == astra::MetricsLevel::Detailed);
    assert(snap.histograms.execution_segment.count >= 1);
    assert(snap.histograms.execution_segment.sum_ns >= 5'000'000);
    assert(snap.histograms.execution_segment.max_ns >= 5'000'000);
    assert(snap.histograms.task_wall_time.count >= 1);
    assert(snap.histograms.task_wall_time.sum_ns >= 5'000'000);
    assert(snap.histograms.ready_queue_wait.count >= 1);

    // (b) Deadline miss 统计 deadline_start_lateness
    {
        astra::TaskOptions d_opts;
        // 传入一个过去的时间点以保证 Deadline miss
        d_opts.deadline = astra::TaskDeadline(std::chrono::steady_clock::now() - 10ms);
        auto h_dl = sched.submit(d_opts, [] { return 1; });
        assert(h_dl.get() == 1);
    }

    auto snap_dl = sched.metrics_snapshot();
    assert(snap_dl.counters.deadline_missed >= 1);
    assert(snap_dl.histograms.deadline_start_lateness.count >= 1);
    assert(snap_dl.histograms.deadline_start_lateness.sum_ns >= 10'000'000);

    // (c) 协程与定时器延迟统计 timer_wake_lateness
    {
        auto coro_fn = [](astra::Scheduler& s) -> astra::Task<int> {
            co_await astra::sleep_for(10ms);
            co_return 99;
        };
        auto h_coro = sched.spawn(coro_fn(sched));
        assert(h_coro.get() == 99);
    }

    auto snap_coro = sched.metrics_snapshot();
    assert(snap_coro.histograms.timer_wake_lateness.count >= 1);

    // (d) 阻塞准入统计 blocking_admission_wait
    {
        std::atomic<bool> block_gate{true};
        // 1 个任务占住 worker 1，1 个占住 worker 2
        auto b1 = sched.submit([&block_gate] {
            while (block_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
        auto b2 = sched.submit([&block_gate] {
            while (block_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });

        // 提交 2 个任务填满 external_pending_capacity (2)
        auto p1 = sched.submit([&block_gate] {
            while (block_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
        auto p2 = sched.submit([&block_gate] {
            while (block_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });

        // 此时 external_pending_count == 2 == capacity，第 5 个 submit 必然阻塞
        std::atomic<bool> submit_started{false};
        std::thread t([&sched, &submit_started] {
            submit_started.store(true, std::memory_order_release);
            auto h = sched.submit([] {});
            h.wait();
        });

        while (!submit_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(20ms);

        block_gate.store(false, std::memory_order_release);
        t.join();
        b1.wait();
        b2.wait();
        p1.wait();
        p2.wait();
    }

    auto snap_wait = sched.metrics_snapshot();
    assert(snap_wait.counters.blocking_submit_waits >= 1);
    assert(snap_wait.counters.blocking_submit_wakeups >= 1);
    assert(snap_wait.histograms.blocking_admission_wait.count >= 1);

    // (e) 关停后测试 runtime_join_latency
    sched.shutdown();
    auto snap_shutdown = sched.metrics_snapshot();
    assert(snap_shutdown.histograms.runtime_join_latency.count >= 1);
    assert(snap_shutdown.histograms.worker_park_duration.count >= 1);

    std::cout << "[PASS] test_R085_detailed_histograms" << std::endl;
}

// -----------------------------------------------------------------------------
// 3. Quiescent Point 守恒关系与不可变快照 (R-085 / R-084 / D-137)
// -----------------------------------------------------------------------------
void test_R085_quiescent_conservation_and_immutability() {
    astra::SchedulerOptions opts;
    opts.worker_count = 4;
    opts.metrics_level = astra::MetricsLevel::Detailed;
    astra::Scheduler sched(opts);

    constexpr int kTaskCount = 50;
    std::vector<astra::TaskHandle<int>> handles;
    handles.reserve(kTaskCount);

    for (int i = 0; i < kTaskCount; ++i) {
        if (i % 5 == 0) {
            // 抛出异常的任务
            handles.push_back(sched.submit([]() -> int {
                throw std::runtime_error("expected task failure");
            }));
        } else {
            handles.push_back(sched.submit([i] {
                return i;
            }));
        }
    }

    // 等待所有任务终态
    int succeeded_count = 0;
    int failed_count = 0;
    for (auto& h : handles) {
        h.wait();
        if (h.state() == astra::TaskState::Succeeded) {
            ++succeeded_count;
        } else if (h.state() == astra::TaskState::Failed) {
            ++failed_count;
            try {
                h.get();
            } catch (...) {}
        }
    }

    // 在 Quiescent point 拍摄 Snapshot
    auto snap = sched.metrics_snapshot();
    assert(snap.enabled);
    assert(!snap.saturated);

    // 守恒等式验证：accepted == succeeded + failed + cancelled_before + cancelled_coop
    assert(snap.counters.accepted_task_identities == kTaskCount);
    assert(snap.counters.succeeded == static_cast<std::uint64_t>(succeeded_count));
    assert(snap.counters.failed == static_cast<std::uint64_t>(failed_count));
    assert(snap.counters.first_starts == kTaskCount);
    assert(snap.counters.accepted_task_identities ==
           snap.counters.succeeded + snap.counters.failed +
           snap.counters.cancelled_before_start + snap.counters.cancelled_cooperative);

    // Quiescent 状态下所有 active gauge 必须为 0
    assert(snap.gauges.ready_tasks == 0);
    assert(snap.gauges.running_tasks == 0);
    assert(snap.gauges.suspended_tasks == 0);
    assert(snap.gauges.waiting_tasks == 0);
    assert(snap.gauges.external_pending_slots_used == 0);

    sched.shutdown();
    std::cout << "[PASS] test_R085_quiescent_conservation_and_immutability" << std::endl;
}

}  // namespace

int main() {
    std::cout << "Running astra_metrics_snapshot_test..." << std::endl;

    test_R085_log2_histogram_bucket_boundaries();
    test_R085_detailed_histograms();
    test_R085_quiescent_conservation_and_immutability();

    std::cout << "All AST-043 metrics snapshot tests passed successfully!" << std::endl;
    return 0;
}

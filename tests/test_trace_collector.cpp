// AST-045 / R-086 / D-138 / D-158 / D-163 — bounded reusable TraceCollector 测试。
// 验证显式 attach/capture/stop/submit、固定容量、重复代际、drop-newest 计数、
// 强异常安全 start_capture、move-only TraceCapture 与活动析构 abort。

#include "astra/scheduler.hpp"
#include "astra/scheduler_options.hpp"
#include "astra/trace.hpp"
#include "lifecycle/reaper_registry.hpp"
#include "observability/trace_collector.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using astra::RuntimeId;
using TraceSlot = astra::TraceSlot;

// -----------------------------------------------------------------------------
// 1. 默认 TraceOptions / Disabled fast path / Stopped 附加后可 capture (R-086/D-158)
// -----------------------------------------------------------------------------
void test_R086_defaults_disabled_fast_path_and_stopped_attach() {
    // 默认容量与 category（D-158）。
    astra::TraceOptions defaults{};
    assert(defaults.events_per_worker == 16'384);
    assert(defaults.external_control_events == 65'536);
    assert(defaults.events_per_reaper_producer == 4'096);
    assert((defaults.categories & astra::TraceCategory::TaskLifecycle) != astra::TraceCategory::None);
    assert((defaults.categories & astra::TraceCategory::StealSuccess) != astra::TraceCategory::None);
    assert((defaults.categories & astra::TraceCategory::StealAttempt) == astra::TraceCategory::None);
    assert((defaults.categories & astra::TraceCategory::Verbose) == astra::TraceCategory::None);

    // 默认不附加 collector。
    astra::SchedulerOptions sched_defaults{};
    assert(sched_defaults.trace_collector == nullptr);

    auto collector = std::make_shared<astra::TraceCollector>();
    assert(!collector->recording());

    // Stopped 附加：低成本低存在，Scheduler 正常工作（disabled fast path）。
    astra::SchedulerOptions opts{};
    opts.worker_count = 2;
    opts.trace_collector = collector;
    {
        astra::Scheduler sched(opts);
        auto h = sched.submit([] { return 5; });
        assert(h.get() == 5);
        assert(!collector->recording());
    }

    // Stopped 期间注册的 producer 在下一代 capture 获得独立 buffer。
    auto capture = collector->start_capture();
    assert(capture.valid());
    assert(capture.recording());
    assert(collector->recording());

    auto* const producer = astra::detail::trace_open_worker_producer(*collector, RuntimeId{77}, 9);
    astra::detail::trace_emit(*collector, producer, astra::TraceCategory::RuntimeLifecycle, 1,
                              RuntimeId{77}, 3, 9);

    auto snap = capture.stop();
    assert(!collector->recording());
    assert(snap.event_record_size() == sizeof(astra::TraceEvent));
    assert(snap.producer_count() == opts.worker_count + 3);  // workers + external + reaper + opened
    bool saw_worker = false, saw_external = false, saw_reaper = false;
    for (std::size_t i = 0; i < snap.producer_count(); ++i) {
        const auto p = snap.producer(i);
        if (p.kind == astra::TraceProducerKind::Worker) saw_worker = true;
        if (p.kind == astra::TraceProducerKind::ExternalControl) saw_external = true;
        if (p.kind == astra::TraceProducerKind::Reaper) saw_reaper = true;
    }
    assert(saw_worker && saw_external && saw_reaper);
    assert(snap.events().size() == 1);
    assert(snap.events()[0].local_sequence == 0);
    assert(snap.events()[0].task_id == 9);
    assert(snap.events()[0].timestamp_ns > 0);

    std::cout << "[PASS] test_R086_defaults_disabled_fast_path_and_stopped_attach" << std::endl;
}

// -----------------------------------------------------------------------------
// 2. 容量/category/溢出校验：状态改变前抛出，上一 snapshot 保留 (R-086/D-158)
// -----------------------------------------------------------------------------
void test_R086_options_validation_strong_safety() {
    auto collector = std::make_shared<astra::TraceCollector>();

    // 先完成一次合法 capture，保留其 snapshot。
    auto c1 = collector->start_capture();
    auto s1 = c1.stop();
    assert(s1.events().empty());

    const std::size_t zero_cases[3] = {0, 0, 0};
    // 零容量 → invalid_argument
    {
        astra::TraceOptions bad{};
        bad.events_per_worker = zero_cases[0];
        bool thrown = false;
        try {
            (void)collector->start_capture(bad);
        } catch (const std::invalid_argument&) {
            thrown = true;
        }
        assert(thrown);
        assert(!collector->recording());
    }
    // external/reaper 零容量
    {
        astra::TraceOptions bad{};
        bad.external_control_events = 0;
        bool thrown = false;
        try {
            (void)collector->start_capture(bad);
        } catch (const std::invalid_argument&) {
            thrown = true;
        }
        assert(thrown);
    }
    {
        astra::TraceOptions bad{};
        bad.events_per_reaper_producer = 0;
        bool thrown = false;
        try {
            (void)collector->start_capture(bad);
        } catch (const std::invalid_argument&) {
            thrown = true;
        }
        assert(thrown);
    }
    // 未知 category bit → invalid_argument
    {
        astra::TraceOptions bad{};
        bad.categories = astra::TraceCategory::All |
                         static_cast<astra::TraceCategory>(1ull << 63);
        bool thrown = false;
        try {
            (void)collector->start_capture(bad);
        } catch (const std::invalid_argument&) {
            thrown = true;
        }
        assert(thrown);
        assert(!collector->recording());
    }
    // checked 总 buffer 溢出 → length_error（≥1 个已注册 producer 触发乘法溢出）
    astra::detail::trace_open_worker_producer(*collector, RuntimeId{21}, 0);
    {
        astra::TraceOptions bad{};
        bad.events_per_worker = static_cast<std::size_t>(-1);
        bool thrown = false;
        try {
            (void)collector->start_capture(bad);
        } catch (const std::length_error&) {
            thrown = true;
        }
        assert(thrown);
        assert(!collector->recording());
    }

    // 上一 snapshot 未被清除，Collector 仍可启动下一代。
    auto c2 = collector->start_capture();
    assert(c2.recording());
    auto s2 = c2.stop();
    assert(s2.producer_count() == 1);  // 校验失败的 start 未注册/清除任何状态
    assert(s1.producer_count() == 0);
    assert(s1.event_record_size() == sizeof(astra::TraceEvent));

    std::cout << "[PASS] test_R086_options_validation_strong_safety" << std::endl;
}

// -----------------------------------------------------------------------------
// 3. 重复 capture 确定性失败与可重复代际 (R-086/D-138)
// -----------------------------------------------------------------------------
void test_R086_duplicate_capture_rejected_and_repeatable() {
    auto collector = std::make_shared<astra::TraceCollector>();

    auto c1 = collector->start_capture();
    assert(collector->recording());

    bool thrown = false;
    try {
        (void)collector->start_capture();
    } catch (const std::logic_error&) {
        thrown = true;
    }
    assert(thrown);
    assert(collector->recording());  // 第一代不受影响

    auto* const producer = astra::detail::trace_open_worker_producer(*collector, RuntimeId{5}, 0);
    astra::detail::trace_emit(*collector, producer, astra::TraceCategory::TaskLifecycle, 2,
                              RuntimeId{5}, 0, 1);
    auto s1 = c1.stop();
    assert(s1.events().size() == 1);

    // 可重复：新一代从零开始，不携带上一代事件。
    auto c2 = collector->start_capture();
    auto s2 = c2.stop();
    assert(s2.events().empty());

    std::cout << "[PASS] test_R086_duplicate_capture_rejected_and_repeatable" << std::endl;
}

// -----------------------------------------------------------------------------
// 4. 并发 producer 无丢失、sequence 严格递增 (R-086)
// -----------------------------------------------------------------------------
void test_R086_concurrent_producers_no_loss() {
    auto collector = std::make_shared<astra::TraceCollector>();
    constexpr std::size_t kProducers = 4;
    constexpr std::uint64_t kEventsPerProducer = 500;

    std::vector<TraceSlot*> slots;
    for (std::size_t i = 0; i < kProducers; ++i) {
        slots.push_back(astra::detail::trace_open_worker_producer(*collector, RuntimeId{9}, static_cast<std::uint32_t>(i)));
    }

    auto capture = collector->start_capture();

    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < kProducers; ++t) {
        threads.emplace_back([&collector, &slots, t] {
            for (std::uint64_t e = 0; e < kEventsPerProducer; ++e) {
                astra::detail::trace_emit(*collector, slots[t], astra::TraceCategory::TaskLifecycle, 3,
                                          RuntimeId{9}, static_cast<std::uint32_t>(t), e + 1);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    auto snap = capture.stop();
    assert(snap.events().size() == kProducers * kEventsPerProducer);
    assert(snap.total_dropped_events() == 0);

    // 每 producer sequence 严格递增覆盖 [0, N)。
    std::vector<std::uint64_t> expected(kProducers, 0);
    for (const auto& ev : snap.events()) {
        bool matched = false;
        for (std::size_t i = 0; i < kProducers; ++i) {
            if (ev.producer_id == slots[i]->producer_id) {
                assert(ev.local_sequence == expected[i]);
                ++expected[i];
                matched = true;
                break;
            }
        }
        assert(matched);
    }
    for (std::size_t i = 0; i < kProducers; ++i) {
        assert(expected[i] == kEventsPerProducer);
    }

    std::cout << "[PASS] test_R086_concurrent_producers_no_loss" << std::endl;
}

// -----------------------------------------------------------------------------
// 5. buffer 满 drop-newest 计 loss，下一代复用 (R-086)
// -----------------------------------------------------------------------------
void test_R086_overflow_drop_newest_and_generation_reuse() {
    auto collector = std::make_shared<astra::TraceCollector>();
    auto* const slot = astra::detail::trace_open_worker_producer(*collector, RuntimeId{11}, 0);

    astra::TraceOptions tiny{};
    tiny.events_per_worker = 8;
    auto capture = collector->start_capture(tiny);
    for (int i = 0; i < 100; ++i) {
        astra::detail::trace_emit(*collector, slot, astra::TraceCategory::TaskLifecycle, 4,
                                  RuntimeId{11}, 0, static_cast<std::uint64_t>(i));
    }
    auto snap = capture.stop();
    assert(snap.events().size() == 8);
    assert(snap.total_dropped_events() == 92);
    assert(snap.events().front().local_sequence == 0);
    assert(snap.events().back().local_sequence == 7);

    // 下一代全新 buffer。
    auto capture2 = collector->start_capture(tiny);
    astra::detail::trace_emit(*collector, slot, astra::TraceCategory::TaskLifecycle, 4, RuntimeId{11}, 0, 1);
    auto snap2 = capture2.stop();
    assert(snap2.events().size() == 1);
    assert(snap2.total_dropped_events() == 0);

    std::cout << "[PASS] test_R086_overflow_drop_newest_and_generation_reuse" << std::endl;
}

// -----------------------------------------------------------------------------
// 6. Capture 生命周期：move-only、幂等 stop、活动析构 abort (R-086/D-163)
// -----------------------------------------------------------------------------
void test_R086_capture_lifecycle_move_stop_destructor() {
    auto collector = std::make_shared<astra::TraceCollector>();
    auto* const slot = astra::detail::trace_open_worker_producer(*collector, RuntimeId{13}, 0);

    auto c1 = collector->start_capture();
    astra::TraceCapture c2 = std::move(c1);
    assert(!c1.valid());
    assert(c2.valid() && c2.recording());

    // moved-from stop 在任何 Collector 副作用前抛 logic_error。
    bool thrown = false;
    try {
        (void)c1.stop();
    } catch (const std::logic_error&) {
        thrown = true;
    }
    assert(thrown);

    astra::detail::trace_emit(*collector, slot, astra::TraceCategory::TaskLifecycle, 5, RuntimeId{13}, 0, 42);
    auto s1 = c2.stop();
    assert(s1.events().size() == 1);
    assert(!c2.recording());

    // 重复 stop 共享同一 immutable backing。
    auto s1_again = c2.stop();
    assert(s1_again.events().size() == s1.events().size());
    assert(s1_again.events().data() == s1.events().data());

    // 活动析构 abort：丢弃该代，Collector 回到 Stopped 且可再次 capture。
    {
        auto c3 = collector->start_capture();
        astra::detail::trace_emit(*collector, slot, astra::TraceCategory::TaskLifecycle, 5, RuntimeId{13}, 0, 43);
        assert(collector->recording());
    }
    assert(!collector->recording());

    auto c4 = collector->start_capture();
    auto s4 = c4.stop();
    assert(s4.events().empty());  // 上一代被丢弃，未泄漏到新一代

    std::cout << "[PASS] test_R086_capture_lifecycle_move_stop_destructor" << std::endl;
}

// -----------------------------------------------------------------------------
// 7. Recording 中附加 Scheduler + buffer 溢出不阻塞 Scheduler (R-086)
// -----------------------------------------------------------------------------
void test_R086_attach_during_recording_and_overflow_liveness() {
    auto collector = std::make_shared<astra::TraceCollector>();

    astra::TraceOptions tiny{};
    tiny.external_control_events = 4;
    auto capture = collector->start_capture(tiny);

    // Recording 中新 Scheduler 附加：startup barrier 前预分配，成功纳入。
    astra::SchedulerOptions opts{};
    opts.worker_count = 2;
    opts.trace_collector = collector;
    std::atomic<int> done{0};
    {
        astra::Scheduler sched(opts);
        for (int i = 0; i < 20; ++i) {
            sched.submit([&done] { done.fetch_add(1); return 0; });
        }
        // external/control buffer 打满：drop-newest，不阻塞 Scheduler。
        auto* const ext = astra::detail::trace_open_external_producer(*collector);
        for (int i = 0; i < 1000; ++i) {
            astra::detail::trace_emit(*collector, ext, astra::TraceCategory::QueueScheduling, 6,
                                      RuntimeId{}, 0, 0);
        }
    }
    assert(done.load() == 20);

    auto snap = capture.stop();
    assert(snap.total_dropped_events() >= 996);  // capacity 4，其余全部 drop-newest
    // 2 workers + external + reaper producer 均在 snapshot 中。
    std::size_t workers = 0;
    for (std::size_t i = 0; i < snap.producer_count(); ++i) {
        if (snap.producer(i).kind == astra::TraceProducerKind::Worker) ++workers;
    }
    assert(workers == 2);

    std::cout << "[PASS] test_R086_attach_during_recording_and_overflow_liveness" << std::endl;
}

// -----------------------------------------------------------------------------
// 8. Collector 不拥有 Runtime；Snapshot 独立离线存活 (R-086/D-163)
// -----------------------------------------------------------------------------
void test_R086_collector_not_owning_runtime_snapshot_outlives() {
    std::shared_ptr<astra::TraceSnapshot> kept;
    {
        auto collector = std::make_shared<astra::TraceCollector>();
        astra::TraceCapture capture;
        {
            astra::SchedulerOptions opts{};
            opts.worker_count = 1;
            opts.trace_collector = collector;
            astra::Scheduler sched(opts);
            (void)sched;
            capture = collector->start_capture();
            auto* const producer = astra::detail::trace_open_worker_producer(*collector, sched.runtime_id(), 0);
            astra::detail::trace_emit(*collector, producer, astra::TraceCategory::RuntimeLifecycle, 7,
                                      sched.runtime_id(), 0, 3);
        }  // Scheduler 销毁，Collector 不拥有 Runtime

        auto snap = capture.stop();
        kept = std::make_shared<astra::TraceSnapshot>(snap);
    }  // Collector 销毁；Snapshot backing 独立存活

    assert(kept->events().size() == 1);
    assert(kept->events()[0].task_id == 3);
    assert(kept->event_record_size() == sizeof(astra::TraceEvent));

    std::cout << "[PASS] test_R086_collector_not_owning_runtime_snapshot_outlives" << std::endl;
}

}  // namespace

int main() {
    std::cout << "Running astra_trace_collector_test..." << std::endl;

    test_R086_defaults_disabled_fast_path_and_stopped_attach();
    test_R086_options_validation_strong_safety();
    test_R086_duplicate_capture_rejected_and_repeatable();
    test_R086_concurrent_producers_no_loss();
    test_R086_overflow_drop_newest_and_generation_reuse();
    test_R086_capture_lifecycle_move_stop_destructor();
    test_R086_attach_during_recording_and_overflow_liveness();
    test_R086_collector_not_owning_runtime_snapshot_outlives();

    std::cout << "All AST-045 trace collector tests passed successfully!" << std::endl;
    return 0;
}

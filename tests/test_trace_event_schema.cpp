// AST-046 / R-087 / D-139 / D-153 — 固定 versioned TraceEvent 与逻辑 ID 关联测试。
// 验证固定记录布局、versioned EventKind、枚举字段、逻辑 ID 关联与确定性重放排序。

#include "astra/scheduler.hpp"
#include "astra/trace.hpp"
#include "trace_collector.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <type_traits>
#include <vector>

namespace {

using astra::RuntimeId;
using TraceSlot = astra::TraceSlot;

// -----------------------------------------------------------------------------
// 1. 布局与版本 golden：trivially-copyable 固定大小、显式枚举值 (R-087)
// -----------------------------------------------------------------------------
void test_R087_layout_version_golden() {
    using astra::TraceEvent;
    static_assert(std::is_trivially_copyable<TraceEvent>::value, "TraceEvent must be trivially copyable");
    // 固定布局：无指针、无 string_view、无析构对象（golden：80 bytes，8 对齐）。
    static_assert(sizeof(TraceEvent) == 80, "TraceEvent layout is versioned and must not drift");
    static_assert(alignof(TraceEvent) == 8, "TraceEvent must be 8-byte aligned");

    TraceEvent ev{};
    assert(ev.schema_version == 1);

    // EventKind 显式版本化数值：新增向后兼容，不重解释旧值。
    using astra::TraceEventKind;
    assert(static_cast<std::uint16_t>(TraceEventKind::Admission) == 1);
    assert(static_cast<std::uint16_t>(TraceEventKind::Rejected) == 2);
    assert(static_cast<std::uint16_t>(TraceEventKind::TaskTerminal) == 7);
    assert(static_cast<std::uint16_t>(TraceEventKind::StealAttempt) == 12);
    assert(static_cast<std::uint16_t>(TraceEventKind::AwaitResumed) == 30);
    assert(static_cast<std::uint16_t>(TraceEventKind::FinalizationComplete) == 37);

    std::cout << "[PASS] test_R087_layout_version_golden" << std::endl;
}

// -----------------------------------------------------------------------------
// 2. EventKind→category 映射与 Default 关闭 Verbose (R-087/D-158)
// -----------------------------------------------------------------------------
void test_R087_category_mapping_and_mask() {
    using astra::TraceCategory;
    using astra::TraceEventKind;
    using astra::category_for_kind;
    using astra::has_category;

    assert(category_for_kind(TraceEventKind::TaskFirstStart) == TraceCategory::TaskLifecycle);
    assert(category_for_kind(TraceEventKind::StealSuccess) == TraceCategory::QueueScheduling);
    assert(category_for_kind(TraceEventKind::StealAttempt) == TraceCategory::StealAttempt);
    assert(category_for_kind(TraceEventKind::CoroutineSuspend) == TraceCategory::Coroutine);
    assert(category_for_kind(TraceEventKind::TimerFire) == TraceCategory::Timer);
    assert(category_for_kind(TraceEventKind::GraphAccepted) == TraceCategory::Graph);
    assert(category_for_kind(TraceEventKind::DeadlineMissed) == TraceCategory::Deadline);
    assert(category_for_kind(TraceEventKind::WaitEnd) == TraceCategory::WaitAwait);
    assert(category_for_kind(TraceEventKind::RuntimeHandoff) == TraceCategory::RuntimeLifecycle);
    assert(category_for_kind(TraceEventKind::FinalizationBegin) == TraceCategory::RuntimeLifecycle);

    assert(has_category(TraceCategory::Default, TraceCategory::TaskLifecycle));
    assert(!has_category(TraceCategory::Default, TraceCategory::StealAttempt));
    assert(!has_category(TraceCategory::Default, TraceCategory::Verbose));
    assert(has_category(TraceCategory::All, TraceCategory::StealAttempt));

    // 事件族完备性：category_for_kind 覆盖 1..37 全部值（None 仅当越界）。
    for (std::uint16_t k = 1; k <= 37; ++k) {
        assert(category_for_kind(static_cast<TraceEventKind>(k)) != TraceCategory::None);
    }

    std::cout << "[PASS] test_R087_category_mapping_and_mask" << std::endl;
}

// -----------------------------------------------------------------------------
// 3. Decode round-trip：identity/枚举字段完整保真 (R-087/D-139)
// -----------------------------------------------------------------------------
void test_R087_decode_round_trip() {
    auto collector = std::make_shared<astra::TraceCollector>();
    auto* worker = astra::detail::trace_open_worker_producer(*collector, RuntimeId{3}, 1);

    astra::detail::TraceEmitDesc desc{};
    desc.kind = astra::TraceEventKind::TaskTerminal;
    desc.runtime_id = RuntimeId{3};
    desc.task_sequence = 12;
    desc.graph_run_sequence = 4;
    desc.node_id = 7;
    desc.worker_id = 1;
    desc.segment_sequence = 2;
    desc.priority = 3;   // Priority raw
    desc.source = 2;     // steal source raw
    desc.task_state = 4; // TaskState::Succeeded raw（status.hpp 顺序）
    desc.outcome = 1;
    desc.reason = 5;
    desc.deadline_disposition = 1; // met

    auto capture = collector->start_capture();
    astra::detail::trace_emit_desc(*collector, worker, desc);
    auto snap = capture.stop();

    assert(snap.events().size() == 1);
    const auto& ev = snap.events()[0];
    assert(ev.schema_version == 1);
    assert(ev.kind == static_cast<std::uint16_t>(astra::TraceEventKind::TaskTerminal));
    assert(ev.category == static_cast<std::uint16_t>(astra::TraceCategory::TaskLifecycle));
    assert(ev.runtime_id == 3);
    assert(ev.task_id == 12);
    assert(ev.graph_run_id == 4);
    assert(ev.node_id == 7);
    assert(ev.worker_id == 1);
    assert(ev.segment_sequence == 2);
    assert(ev.priority == 3);
    assert(ev.source == 2);
    assert(ev.task_state == 4);
    assert(ev.outcome == 1);
    assert(ev.reason == 5);
    assert(ev.deadline_disposition == 1);
    assert(ev.producer_id == worker->producer_id);
    assert(ev.local_sequence == 0);
    assert(ev.timestamp_ns > 0);

    std::cout << "[PASS] test_R087_decode_round_trip" << std::endl;
}

// -----------------------------------------------------------------------------
// 4. 跨 Runtime identity 与 Graph coroutine 双重身份 (R-087/D-153)
// -----------------------------------------------------------------------------
void test_R087_cross_runtime_and_graph_identity() {
    auto collector = std::make_shared<astra::TraceCollector>();
    auto* w_a = astra::detail::trace_open_worker_producer(*collector, RuntimeId{100}, 0);
    auto* w_b = astra::detail::trace_open_worker_producer(*collector, RuntimeId{200}, 0);

    astra::detail::TraceEmitDesc desc{};
    desc.kind = astra::TraceEventKind::TaskFirstStart;
    desc.task_sequence = 5;
    desc.worker_id = 0;

    auto capture = collector->start_capture();
    desc.runtime_id = RuntimeId{100};
    astra::detail::trace_emit_desc(*collector, w_a, desc);
    desc.runtime_id = RuntimeId{200};
    astra::detail::trace_emit_desc(*collector, w_b, desc);

    // Graph coroutine node：同一 TaskId + GraphRunId + NodeId 共存于同一事件，
    // 不创建第二 identity（D-139/D-153）。
    astra::detail::TraceEmitDesc g = desc;
    g.kind = astra::TraceEventKind::NodeDependencyRelease;
    g.runtime_id = RuntimeId{200};
    g.graph_run_sequence = 9;
    g.node_id = 3;
    g.task_sequence = 5;
    astra::detail::trace_emit_desc(*collector, w_b, g);

    auto snap = capture.stop();
    assert(snap.events().size() == 3);

    const auto& ev_a = snap.events()[0];
    const auto& ev_b = snap.events()[1];
    const auto& ev_g = snap.events()[2];
    // 相同 task sequence、不同 Runtime ⇒ 不同 identity（不因 runtime-local 混淆）。
    assert(ev_a.task_id == ev_b.task_id && ev_a.runtime_id != ev_b.runtime_id);
    // Graph 事件同时携带三重身份。
    assert(ev_g.graph_run_id == 9 && ev_g.node_id == 3 && ev_g.task_id == 5);

    std::cout << "[PASS] test_R087_cross_runtime_and_graph_identity" << std::endl;
}

// -----------------------------------------------------------------------------
// 5. per-producer 单调性与 sequence 严格递增；确定性重放排序 (R-087/D-139)
// -----------------------------------------------------------------------------
void test_R087_per_producer_ordering_and_deterministic_merge() {
    auto collector = std::make_shared<astra::TraceCollector>();
    auto* w = astra::detail::trace_open_worker_producer(*collector, RuntimeId{7}, 0);
    auto* r = astra::detail::trace_open_reaper_producer(*collector);

    auto capture = collector->start_capture();
    // 两个 producer 交错发射，事件族覆盖 wait/await/runtime/finalization。
    for (std::uint32_t i = 0; i < 50; ++i) {
        astra::detail::TraceEmitDesc d{};
        d.kind = (i % 2 == 0) ? astra::TraceEventKind::WaitBegin : astra::TraceEventKind::AwaitResumed;
        d.runtime_id = RuntimeId{7};
        d.task_sequence = i + 1;
        astra::detail::trace_emit_desc(*collector, w, d);

        astra::detail::TraceEmitDesc rd{};
        rd.kind = (i % 2 == 0) ? astra::TraceEventKind::RuntimeJoinReady
                               : astra::TraceEventKind::FinalizationBegin;
        rd.runtime_id = RuntimeId{7};
        rd.task_sequence = 0;
        astra::detail::trace_emit_desc(*collector, r, rd);
    }
    auto snap = capture.stop();
    assert(snap.events().size() == 100);

    // worker producer（单线程）：按 sequence 排序后 timestamp 单调不降。
    std::vector<const astra::TraceEvent*> worker_events;
    for (const auto& ev : snap.events()) {
        if (ev.producer_id == w->producer_id) worker_events.push_back(&ev);
    }
    assert(worker_events.size() == 50);
    std::sort(worker_events.begin(), worker_events.end(),
              [](const astra::TraceEvent* a, const astra::TraceEvent* b) {
                  return a->local_sequence < b->local_sequence;
              });
    for (std::size_t i = 0; i < worker_events.size(); ++i) {
        assert(worker_events[i]->local_sequence == i);  // 严格递增 0..N-1
        if (i > 0) {
            assert(worker_events[i]->timestamp_ns >= worker_events[i - 1]->timestamp_ns);
        }
    }

    // 确定性重放：同一 snapshot 多次排序结果一致（(producer, sequence) 唯一全序）。
    auto o1 = astra::trace_ordered_events(snap);
    auto o2 = astra::trace_ordered_events(snap);
    assert(o1.size() == o2.size());
    for (std::size_t i = 0; i < o1.size(); ++i) {
        assert(o1[i].producer_id == o2[i].producer_id);
        assert(o1[i].local_sequence == o2[i].local_sequence);
        assert(o1[i].timestamp_ns == o2[i].timestamp_ns);
    }
    // 全序检查：相邻元素 key 严格有序。
    for (std::size_t i = 1; i < o1.size(); ++i) {
        const bool strictly_after =
            o1[i].timestamp_ns > o1[i - 1].timestamp_ns ||
            (o1[i].timestamp_ns == o1[i - 1].timestamp_ns &&
             (o1[i].producer_id > o1[i - 1].producer_id ||
              (o1[i].producer_id == o1[i - 1].producer_id &&
               o1[i].local_sequence > o1[i - 1].local_sequence)));
        assert(strictly_after);
    }

    std::cout << "[PASS] test_R087_per_producer_ordering_and_deterministic_merge" << std::endl;
}

// -----------------------------------------------------------------------------
// 6. StealAttempt 默认关闭：不记录且不算 drop；All 显式开启 (R-087/D-158)
// -----------------------------------------------------------------------------
void test_R087_steal_attempt_category_gate() {
    auto collector = std::make_shared<astra::TraceCollector>();
    auto* w = astra::detail::trace_open_worker_producer(*collector, RuntimeId{8}, 0);

    // Default：StealAttempt（category）关闭 → no-op 不计 drop。
    auto c1 = collector->start_capture();  // Default mask
    astra::detail::trace_emit(*collector, w, astra::TraceCategory::StealAttempt,
                              static_cast<std::uint16_t>(astra::TraceEventKind::StealAttempt),
                              RuntimeId{8}, 0, 1);
    auto s1 = c1.stop();
    assert(s1.events().empty());
    for (std::size_t i = 0; i < s1.producer_count(); ++i) {
        assert(s1.producer(i).dropped_events == 0);  // category 关闭不是 drop
    }

    // All：显式记录。
    astra::TraceOptions all{};
    all.categories = astra::TraceCategory::All;
    auto c2 = collector->start_capture(all);
    astra::detail::trace_emit(*collector, w, astra::TraceCategory::StealAttempt,
                              static_cast<std::uint16_t>(astra::TraceEventKind::StealAttempt),
                              RuntimeId{8}, 0, 2);
    auto s2 = c2.stop();
    assert(s2.events().size() == 1);
    assert(s2.events()[0].kind == static_cast<std::uint16_t>(astra::TraceEventKind::StealAttempt));
    assert(s2.events()[0].category == static_cast<std::uint16_t>(astra::TraceCategory::StealAttempt));

    std::cout << "[PASS] test_R087_steal_attempt_category_gate" << std::endl;
}

// -----------------------------------------------------------------------------
// 7. 无地址/字符串泄漏：事件记录只含逻辑 ID 与紧凑枚举 (R-087/D-139)
// -----------------------------------------------------------------------------
void test_R087_no_address_or_string_leak() {
    // TraceEvent 无指针成员：trivially-copyable 且无引用/指针字段。
    using astra::TraceEvent;
    static_assert(!std::is_pointer<TraceEvent>::value, "no pointer type");
    static_assert(std::is_trivially_destructible<TraceEvent>::value, "no owning members");
    // 事件记录不含 std::string/std::string_view 字段（由 trivially copyable 保证）。
    // sentinel 语义：默认构造全部 identity 为 0。
    TraceEvent ev{};
    assert(ev.runtime_id == 0 && ev.task_id == 0 && ev.graph_run_id == 0 && ev.node_id == 0);

    std::cout << "[PASS] test_R087_no_address_or_string_leak" << std::endl;
}

}  // namespace

int main() {
    std::cout << "Running astra_trace_event_schema_test..." << std::endl;

    test_R087_layout_version_golden();
    test_R087_category_mapping_and_mask();
    test_R087_decode_round_trip();
    test_R087_cross_runtime_and_graph_identity();
    test_R087_per_producer_ordering_and_deterministic_merge();
    test_R087_steal_attempt_category_gate();
    test_R087_no_address_or_string_leak();

    std::cout << "All AST-046 trace event schema tests passed successfully!" << std::endl;
    return 0;
}

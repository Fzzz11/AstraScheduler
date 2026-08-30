#include "trace_collector.hpp"

#include <astra/trace.hpp>

#include <algorithm>
#include <deque>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

namespace astra {

// ============================================================================
// TraceCollector（AST-045 / R-086 / D-138 / D-158 / D-163）
// ----------------------------------------------------------------------------
// - 显式共享 capability：初始 Stopped，可重复执行单一活动 capture generation。
// - start_capture 强异常安全：校验（invalid_argument/length_error）与全部
//   buffer 预分配（bad_alloc）都在任何状态改变前完成，失败保持 Stopped。
// - emit 热路径无锁（仅原子 ticket/generation 握手）、无分配、无 I/O、无阻塞；
//   buffer 满 drop-newest 计 per-producer loss。
// - stop 与活动析构共用 disable/quiesce 协议：active=false 后等待已进入的
//   bounded emit 临界区退出，绝不等待 Task/Worker join/Runtime completion。
// ============================================================================

namespace {

using TraceCategoryRaw = std::uint64_t;

[[nodiscard]] std::size_t capacity_for_kind(const TraceOptions& options, TraceProducerKind kind) noexcept {
    switch (kind) {
        case TraceProducerKind::Worker:
            return options.events_per_worker;
        case TraceProducerKind::ExternalControl:
            return options.external_control_events;
        case TraceProducerKind::Reaper:
            return options.events_per_reaper_producer;
    }
    return 0;
}

}  // namespace

struct TraceSnapshot::Data {
    std::uint32_t schema_version{1};
    std::chrono::steady_clock::time_point origin{};
    std::size_t event_record_size{0};
    TraceCategory categories{TraceCategory::Default};
    std::vector<ProducerReport> producers;
    std::vector<TraceEvent> events;
    std::uint64_t total_dropped{0};
};

TraceSnapshot::TraceSnapshot(std::shared_ptr<const Data> data) noexcept : impl_(std::move(data)) {}

std::uint32_t TraceSnapshot::schema_version() const noexcept { return impl_ ? impl_->schema_version : 0; }
std::chrono::steady_clock::time_point TraceSnapshot::origin() const noexcept {
    return impl_ ? impl_->origin : std::chrono::steady_clock::time_point{};
}
std::size_t TraceSnapshot::event_record_size() const noexcept {
    return impl_ ? impl_->event_record_size : 0;
}
TraceCategory TraceSnapshot::categories() const noexcept {
    return impl_ ? impl_->categories : TraceCategory::None;
}
std::size_t TraceSnapshot::producer_count() const noexcept { return impl_ ? impl_->producers.size() : 0; }
const TraceSnapshot::ProducerReport& TraceSnapshot::producer(std::size_t index) const {
    if (!impl_) {
        throw std::out_of_range("TraceSnapshot::producer index out of range");
    }
    return impl_->producers.at(index);
}
const std::vector<TraceEvent>& TraceSnapshot::events() const noexcept {
    static const std::vector<TraceEvent> kEmpty;
    return impl_ ? impl_->events : kEmpty;
}
std::uint64_t TraceSnapshot::total_dropped_events() const noexcept {
    return impl_ ? impl_->total_dropped : 0;
}

struct TraceCapture::Impl {
    std::mutex stop_mutex;  // 同一 Capture 的重复/并发 stop 共享一次事务
    std::shared_ptr<TraceGeneration> generation;
    bool closed{false};
};

TraceCapture::TraceCapture(std::shared_ptr<TraceCollector> collector, std::shared_ptr<Impl> impl) noexcept
    : collector_(std::move(collector)), impl_(std::move(impl)) {}

TraceCapture::~TraceCapture() noexcept {
    // 活动析构 abort（D-163）：与 stop 同一 disable/quiesce 协议，丢弃该代数据，
    // 使 Collector 回到 Stopped；Stopped/moved-from 析构为 no-op。
    if (impl_ && collector_ && !impl_->closed) {
        collector_->abort_capture(*impl_);
    }
}

TraceCapture::TraceCapture(TraceCapture&& other) noexcept
    : collector_(std::move(other.collector_)),
      impl_(std::move(other.impl_)),
      result_(std::move(other.result_)) {}

TraceCapture& TraceCapture::operator=(TraceCapture&& other) noexcept {
    if (this != &other) {
        if (impl_ && collector_ && !impl_->closed) {
            collector_->abort_capture(*impl_);
        }
        collector_ = std::move(other.collector_);
        impl_ = std::move(other.impl_);
        result_ = std::move(other.result_);
    }
    return *this;
}

bool TraceCapture::valid() const noexcept { return impl_ != nullptr; }

bool TraceCapture::recording() const noexcept {
    return impl_ && !impl_->closed && collector_ && collector_->recording();
}

TraceSnapshot TraceCapture::stop() {
    if (!impl_ || !collector_) {
        throw std::logic_error("TraceCapture::stop called on empty/moved-from capture");
    }
    std::lock_guard<std::mutex> lock(impl_->stop_mutex);
    if (impl_->closed) {
        if (result_) {
            // 重复/并发 stop 共享同一 immutable backing（D-163）。
            return TraceSnapshot(result_);
        }
        throw std::logic_error("TraceCapture generation was discarded");
    }
    TraceSnapshot snapshot = collector_->stop_capture(*impl_);
    impl_->closed = true;
    result_ = snapshot.impl_;
    return snapshot;
}

struct TraceCollector::Impl {
    mutable std::mutex mutex;
    std::deque<std::unique_ptr<TraceSlot>> slots;  // 地址稳定，供 emit 热路径直接使用
    std::atomic<std::shared_ptr<TraceGeneration>> current;
    std::uint64_t next_producer_id{1};
    TraceSlot* external_slot{nullptr};
    TraceSlot* reaper_slot{nullptr};

    [[nodiscard]] bool recording_locked() const noexcept {
        const auto gen = current.load(std::memory_order_acquire);
        return gen && gen->active.load(std::memory_order_acquire);
    }

    // 注册 producer；Recording 中立即为当前代分配 buffer（D-158 附加扩展点）。
    // 分配失败在槽位提交前抛出，保持 Collector 状态一致。
    TraceSlot* open_slot_locked(TraceProducerKind kind, RuntimeId runtime_id,
                                std::uint32_t worker_index, bool singleton) {
        if (singleton) {
            TraceSlot* existing = (kind == TraceProducerKind::ExternalControl) ? external_slot : reaper_slot;
            if (existing) {
                return existing;
            }
        }
        auto slot = std::make_unique<TraceSlot>();
        slot->kind = kind;
        slot->runtime_id = runtime_id;
        slot->worker_index = worker_index;
        slot->producer_id = next_producer_id++;

        if (recording_locked()) {
            const auto gen = current.load(std::memory_order_acquire);
            auto entry = std::make_shared<TraceBufferEntry>();
            entry->generation = gen;
            entry->buffer = std::make_shared<TraceBuffer>(capacity_for_kind(gen->options, kind));
            slot->entry.store(std::move(entry), std::memory_order_release);
        }

        TraceSlot* raw = slot.get();
        slot->index = slots.size();
        slots.push_back(std::move(slot));
        if (kind == TraceProducerKind::ExternalControl) {
            external_slot = raw;
        } else if (kind == TraceProducerKind::Reaper) {
            reaper_slot = raw;
        }
        return raw;
    }
};

TraceCollector::TraceCollector() : impl_(std::make_unique<Impl>()) {}
TraceCollector::~TraceCollector() {
    // Collector 销毁时若仍有活动 generation：执行 disable/quiesce 后丢弃，
    // 绝不在析构中导出或阻塞于 Runtime。
    if (impl_) {
        const auto gen = impl_->current.load(std::memory_order_acquire);
        if (gen && gen->active.load(std::memory_order_acquire)) {
            gen->active.store(false, std::memory_order_release);
            while (gen->inflight.load(std::memory_order_acquire) != 0) {
                std::this_thread::yield();
            }
        }
    }
}

TraceCapture TraceCollector::start_capture(TraceOptions options) {
    auto& impl = *impl_;
    std::lock_guard<std::mutex> lock(impl.mutex);

    if (impl.recording_locked()) {
        // 同一 Collector 一次只允许一代 Recording；并发第二次 start 确定性失败。
        throw std::logic_error("TraceCollector already has an active capture generation");
    }

    // --- 校验：任何状态改变前（R-086 / D-158）---
    if (options.events_per_worker == 0 || options.external_control_events == 0 ||
        options.events_per_reaper_producer == 0) {
        throw std::invalid_argument("TraceOptions capacities must be greater than 0");
    }
    if ((static_cast<TraceCategoryRaw>(options.categories) &
         ~static_cast<TraceCategoryRaw>(TraceCategory::All)) != 0) {
        throw std::invalid_argument("TraceOptions::categories contains unknown bits");
    }

    std::size_t worker_slots = 0;
    std::size_t external_slots = 0;
    std::size_t reaper_slots = 0;
    for (const auto& slot : impl.slots) {
        switch (slot->kind) {
            case TraceProducerKind::Worker:
                ++worker_slots;
                break;
            case TraceProducerKind::ExternalControl:
                ++external_slots;
                break;
            case TraceProducerKind::Reaper:
                ++reaper_slots;
                break;
        }
    }

    std::size_t total = 0;
    std::size_t part = 0;
    auto add_part = [&](std::size_t count, std::size_t capacity) {
        if (__builtin_mul_overflow(count, capacity, &part) ||
            __builtin_add_overflow(total, part, &total)) {
            throw std::length_error("TraceOptions total buffer size overflows");
        }
    };
    add_part(worker_slots, options.events_per_worker);
    add_part(external_slots, options.external_control_events);
    add_part(reaper_slots, options.events_per_reaper_producer);

    // --- 预分配：bad_alloc 直接传播，保持 Stopped 且上一 snapshot 不受影响 ---
    auto generation = std::make_shared<TraceGeneration>();
    generation->options = options;
    generation->mask = options.categories;
    generation->origin = std::chrono::steady_clock::now();

    std::vector<std::pair<TraceSlot*, std::shared_ptr<TraceBufferEntry>>> prepared;
    prepared.reserve(impl.slots.size());
    for (auto& slot : impl.slots) {
        auto entry = std::make_shared<TraceBufferEntry>();
        entry->generation = generation;
        entry->buffer = std::make_shared<TraceBuffer>(capacity_for_kind(options, slot->kind));
        prepared.emplace_back(slot.get(), std::move(entry));
    }

    // --- 提交：发布新 Recording generation ---
    auto capture_impl = std::make_shared<TraceCapture::Impl>();
    capture_impl->generation = generation;
    for (auto& [slot, entry] : prepared) {
        slot->entry.store(std::move(entry), std::memory_order_release);
    }
    impl.current.store(std::move(generation), std::memory_order_release);

    return TraceCapture(shared_from_this(), std::move(capture_impl));
}

bool TraceCollector::recording() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->recording_locked();
}

TraceSnapshot TraceCollector::stop_capture(TraceCapture::Impl& capture) {
    auto& impl = *impl_;
    std::lock_guard<std::mutex> lock(impl.mutex);

    auto generation = capture.generation;
    if (!generation || impl.current.load(std::memory_order_acquire) != generation) {
        // 该代已被 abort 或随 Collector 销毁被取代：无可领取结果。
        capture.generation.reset();
        capture.closed = true;
        return TraceSnapshot{};
    }

    // 线性化禁止新事件，随后等待已进入的 bounded emit 临界区退出。
    generation->active.store(false, std::memory_order_release);
    while (generation->inflight.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }

    auto data = std::make_shared<TraceSnapshot::Data>();
    data->schema_version = 1;
    data->origin = generation->origin;
    data->event_record_size = sizeof(TraceEvent);
    data->categories = generation->mask;

    std::uint64_t total_dropped = 0;
    for (const auto& slot : impl.slots) {
        const auto entry = slot->entry.load(std::memory_order_acquire);
        if (!entry || entry->generation != generation) {
            continue;  // 该代期间未分配 buffer 的槽位
        }
        const TraceBuffer& buffer = *entry->buffer;
        const auto committed = std::min<std::uint64_t>(
            buffer.next_ticket.load(std::memory_order_acquire),
            static_cast<std::uint64_t>(buffer.capacity));
        const auto dropped = buffer.dropped.load(std::memory_order_acquire);

        TraceSnapshot::ProducerReport report;
        report.producer_id = slot->producer_id;
        report.kind = slot->kind;
        report.capacity = buffer.capacity;
        report.dropped_events = dropped;
        data->producers.push_back(report);
        total_dropped += dropped;

        data->events.insert(data->events.end(), buffer.slots.begin(),
                            buffer.slots.begin() + static_cast<std::ptrdiff_t>(committed));
    }
    data->total_dropped = total_dropped;

    capture.generation.reset();
    capture.closed = true;
    impl.current.store(std::shared_ptr<TraceGeneration>{}, std::memory_order_release);
    return TraceSnapshot(std::move(data));
}

void TraceCollector::abort_capture(TraceCapture::Impl& capture) noexcept {
    auto& impl = *impl_;
    std::lock_guard<std::mutex> lock(impl.mutex);
    if (capture.closed) {
        return;
    }
    auto generation = capture.generation;
    capture.generation.reset();
    capture.closed = true;
    if (!generation || impl.current.load(std::memory_order_acquire) != generation) {
        return;
    }
    // 活动析构 abort：disable/quiesce 后丢弃该代全部数据（D-163）。
    generation->active.store(false, std::memory_order_release);
    while (generation->inflight.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    impl.current.store(std::shared_ptr<TraceGeneration>{}, std::memory_order_release);
}

// ============================================================================
// detail seam
// ============================================================================

namespace detail {

struct CollectorAccess {
    static TraceCollector::Impl& impl(TraceCollector& collector) { return *collector.impl_; }
};

void trace_attach_runtime(const std::shared_ptr<TraceCollector>& collector,
                          RuntimeId runtime_id, std::size_t worker_count,
                          std::vector<TraceSlot*>* worker_slots,
                          TraceSlot** external_slot) {
    if (!collector) {
        return;
    }
    auto& impl = CollectorAccess::impl(*collector);
    std::lock_guard<std::mutex> lock(impl.mutex);
    // Reaper coordinator 向附着 Collector 注册独立 producer（D-138）；
    // external/control 共享入口为每 Collector 单例。
    impl.open_slot_locked(TraceProducerKind::Reaper, runtime_id, 0, /*singleton=*/true);
    TraceSlot* ext = impl.open_slot_locked(TraceProducerKind::ExternalControl, runtime_id, 0,
                                           /*singleton=*/true);
    if (external_slot) {
        *external_slot = ext;
    }
    for (std::size_t w = 0; w < worker_count; ++w) {
        TraceSlot* slot = impl.open_slot_locked(TraceProducerKind::Worker, runtime_id,
                                                static_cast<std::uint32_t>(w),
                                                /*singleton=*/false);
        if (worker_slots) {
            worker_slots->push_back(slot);
        }
    }
}

TraceSlot* trace_open_worker_producer(TraceCollector& collector, RuntimeId runtime_id,
                                      std::uint32_t worker_index) {
    auto& impl = CollectorAccess::impl(collector);
    std::lock_guard<std::mutex> lock(impl.mutex);
    return impl.open_slot_locked(TraceProducerKind::Worker, runtime_id, worker_index, /*singleton=*/false);
}

TraceSlot* trace_open_external_producer(TraceCollector& collector) {
    auto& impl = CollectorAccess::impl(collector);
    std::lock_guard<std::mutex> lock(impl.mutex);
    return impl.open_slot_locked(TraceProducerKind::ExternalControl, RuntimeId{}, 0, /*singleton=*/true);
}

TraceSlot* trace_open_reaper_producer(TraceCollector& collector) {
    auto& impl = CollectorAccess::impl(collector);
    std::lock_guard<std::mutex> lock(impl.mutex);
    return impl.open_slot_locked(TraceProducerKind::Reaper, RuntimeId{}, 0, /*singleton=*/true);
}

namespace {

// emit 核心路径（R-086）：generation 握手 + category mask + ticket buffer。
// 无分配、无 I/O、无阻塞；Stopped/disabled/无 buffer 为 no-op 不计 drop。
void emit_event(TraceCollector& collector, TraceSlot* slot, TraceCategory category,
                const TraceEvent& event) noexcept {
    auto& impl = CollectorAccess::impl(collector);
    const auto generation = impl.current.load(std::memory_order_acquire);
    if (!generation || !generation->active.load(std::memory_order_acquire)) {
        return;  // no capture / Stopped：fast no-op，不算 drop
    }
    generation->inflight.fetch_add(1, std::memory_order_acq_rel);

    const auto entry = slot->entry.load(std::memory_order_acquire);
    TraceBuffer* buffer = nullptr;
    if (entry && entry->generation == generation &&
        generation->active.load(std::memory_order_acquire)) {
        buffer = entry->buffer.get();  // 代际隔离：仅当前代 buffer 可写
    }
    if (!buffer) {
        generation->inflight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    if ((static_cast<TraceCategoryRaw>(generation->mask) &
         static_cast<TraceCategoryRaw>(category)) == 0) {
        generation->inflight.fetch_sub(1, std::memory_order_acq_rel);
        return;  // category disabled：fast no-op，不算 drop
    }

    const auto ticket = buffer->next_ticket.fetch_add(1, std::memory_order_relaxed);
    if (ticket >= static_cast<std::uint64_t>(buffer->capacity)) {
        buffer->dropped.fetch_add(1, std::memory_order_relaxed);  // drop-newest
        generation->inflight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    TraceEvent stamped = event;
    stamped.schema_version = 1;
    stamped.category = static_cast<std::uint16_t>(static_cast<TraceCategoryRaw>(category) & 0xFFFFu);
    const auto now = std::chrono::steady_clock::now();
    stamped.timestamp_ns = now >= generation->origin
                               ? static_cast<std::uint64_t>(
                                     std::chrono::duration_cast<std::chrono::nanoseconds>(now - generation->origin).count())
                               : 0;
    stamped.producer_id = slot->producer_id;
    stamped.local_sequence = ticket;
    buffer->slots[static_cast<std::size_t>(ticket)] = stamped;

    generation->inflight.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace

void trace_emit(TraceCollector& collector, TraceSlot* slot, TraceCategory category,
                std::uint16_t kind, RuntimeId runtime_id, std::uint32_t worker_id,
                std::uint64_t task_id) noexcept {
    if (!slot) {
        return;
    }
    TraceEvent event{};
    event.kind = kind;
    event.runtime_id = runtime_id.value();
    event.worker_id = worker_id;
    event.task_id = task_id;
    emit_event(collector, slot, category, event);
}

void trace_emit_desc(TraceCollector& collector, TraceSlot* slot, const TraceEmitDesc& desc) noexcept {
    if (!slot) {
        return;
    }
    TraceEvent event{};
    event.kind = static_cast<std::uint16_t>(desc.kind);
    event.runtime_id = desc.runtime_id.value();
    event.task_id = desc.task_sequence;
    event.target_runtime_id = desc.target_runtime_id.value();
    event.target_task_id = desc.target_task_sequence;
    event.graph_run_id = desc.graph_run_sequence;
    event.node_id = desc.node_id;
    event.worker_id = desc.worker_id;
    event.segment_sequence = desc.segment_sequence;
    event.priority = desc.priority;
    event.source = desc.source;
    event.task_state = desc.task_state;
    event.outcome = desc.outcome;
    event.reason = desc.reason;
    event.deadline_disposition = desc.deadline_disposition;
    emit_event(collector, slot, category_for_kind(desc.kind), event);
}

}  // namespace detail

std::vector<TraceEvent> trace_ordered_events(const TraceSnapshot& snapshot) {
    std::vector<TraceEvent> ordered = snapshot.events();
    std::stable_sort(ordered.begin(), ordered.end(), [](const TraceEvent& a, const TraceEvent& b) {
        if (a.timestamp_ns != b.timestamp_ns) {
            return a.timestamp_ns < b.timestamp_ns;
        }
        if (a.producer_id != b.producer_id) {
            return a.producer_id < b.producer_id;
        }
        return a.local_sequence < b.local_sequence;
    });
    return ordered;
}

}  // namespace astra

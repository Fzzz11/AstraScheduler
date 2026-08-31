#ifndef ASTRA_TRACE_HPP
#define ASTRA_TRACE_HPP

// AstraScheduler 有界可重复 Trace capture（AST-045 / R-086 / D-138 / D-158 / D-163）。
// 线程安全共享 TraceCollector：显式 start_capture/stop、固定容量、重复代际、
// drop-newest 计 loss；emit 无分配/文件 I/O/callback/阻塞，Runtime 热路径安全。
// 完整 versioned TraceEvent EventKind 族由 R-087（AST-046）扩展。
// 【通俗说明】这是"飞行记录仪"：submit/claim/挂起/恢复/抢锁等关键事件带
// 时间戳写入内存里的定长缓冲区，事后离线导出成 Chrome Trace JSON（用
// Chrome 的 tracing 查看器打开就是时间线）。设计取舍：事件写入绝不阻塞
// 业务线程——缓冲区满了就丢弃新事件并计数（trace_complete=false 明确告诉
// 你丢了多少），绝不为了完整性阻塞调度。用一个小容量的 buffer 就能验证
// 丢失语义，用大容量才谈性能结论。

#include <astra/export.hpp>
#include <astra/id.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace astra {

class TraceCollector;  // forward：TraceCapture/TraceSnapshot 均引用共享 capability

namespace detail { struct CollectorAccess; }  // 内部 seam 访问器（AST-045）

// Trace category bitmask（D-158）。Default ≠ All：逐次 StealAttempt 与其他
// 明确标记 Verbose 的高频事件默认关闭，用户可显式加入；category 关闭不算 drop。
/** @brief Trace 事件分类的 bitmask。 */
enum class TraceCategory : std::uint64_t {
    None = 0,
    TaskLifecycle = 0x1,
    QueueScheduling = 0x2,
    StealSuccess = 0x4,
    StealAttempt = 0x8,          // Verbose：默认关闭
    WaitAwait = 0x10,
    Coroutine = 0x20,
    Graph = 0x40,
    Timer = 0x80,
    Deadline = 0x100,
    RuntimeLifecycle = 0x200,
    Verbose = 0x400,             // 其他明确标记 Verbose 的高频事件
    Default = 0x3F7,             // 上述除 StealAttempt/Verbose 外全部启用
    All = 0x7FF,                 // 全部已知 category
};

[[nodiscard]] constexpr TraceCategory operator|(TraceCategory a, TraceCategory b) noexcept {
    return static_cast<TraceCategory>(static_cast<std::uint64_t>(a) | static_cast<std::uint64_t>(b));
}
[[nodiscard]] constexpr TraceCategory operator&(TraceCategory a, TraceCategory b) noexcept {
    return static_cast<TraceCategory>(static_cast<std::uint64_t>(a) & static_cast<std::uint64_t>(b));
}
[[nodiscard]] constexpr TraceCategory operator~(TraceCategory a) noexcept {
    return static_cast<TraceCategory>(~static_cast<std::uint64_t>(a));
}
[[nodiscard]] constexpr bool has_category(TraceCategory mask, TraceCategory cat) noexcept {
    return (mask & cat) != TraceCategory::None;
}

// 固定有界默认容量与 category 集（D-158）。capacity 按 event slots 而非 bytes 定义。
/** @brief 一代 Trace capture 的容量和分类配置。 */
struct TraceOptions {
    std::size_t events_per_worker = 16'384;
    std::size_t external_control_events = 65'536;
    std::size_t events_per_reaper_producer = 4'096;
    TraceCategory categories = TraceCategory::Default;
};

// Producer buffer 类别（D-138：per-Worker SPSC ring / 共享 external/control MPMC
// 入口 / 每 Reaper 独立 producer）。
/** @brief Trace 事件 producer buffer 的来源类型。 */
enum class TraceProducerKind : std::uint8_t {
    Worker,
    ExternalControl,
    Reaper,
};

// 固定大小 trivially-copyable 事件记录（R-087 / D-139 / D-153）。
// 缺失 identity/枚举使用零值 invalid sentinel；不保存 raw pointer、
// 用户字符串或异常文本。跨 producer 只按 (timestamp_ns, producer_id,
// local_sequence) 形成导出确定全序，不声称存在全局线性化顺序。
/** @brief 固定大小、可复制的版本化 Trace 事件记录。 */
struct TraceEvent {
    std::uint32_t schema_version{1};
    std::uint16_t category{0};             // TraceCategory raw bit
    std::uint16_t kind{0};                 // TraceEventKind（显式版本化值）
    std::uint64_t timestamp_ns{0};         // capture-relative steady timestamp
    std::uint64_t producer_id{0};
    std::uint64_t local_sequence{0};       // 每 producer buffer 内严格递增
    std::uint64_t runtime_id{0};           // source RuntimeId value（wait/await edge 的 source 侧）
    std::uint64_t task_id{0};              // source TaskId sequence（0 = 无 source Task identity）
    std::uint64_t target_runtime_id{0};    // wait/await edge target RuntimeId（0 = 无 target）
    std::uint64_t target_task_id{0};       // wait/await edge target TaskId sequence
    std::uint64_t graph_run_id{0};         // GraphRunId sequence（0 = 非 Graph；graph wait 的 target）
    std::uint32_t node_id{0};              // graph-local NodeId
    std::uint32_t worker_id{0};            // 0 = 非 Worker producer
    std::uint32_t segment_sequence{0};     // coroutine segment 序（0 = 首段前无）
    std::uint16_t priority{0};             // Priority raw（0 = unspecified）
    std::uint16_t source{0};               // claim/steal/wait source raw
    std::uint16_t task_state{0};           // TaskState raw（0 = invalid sentinel）
    std::uint16_t outcome{0};              // Task outcome raw
    std::uint16_t reason{0};               // rejection/cancel/wait end reason raw
    std::uint16_t deadline_disposition{0}; // 0 = none / 1 = met / 2 = missed
};

// Versioned EventKind 族（R-087 / D-139）：显式数值，新增向后兼容；
// 重解释旧值必须升级 major schema。
/** @brief Trace 事件的稳定、版本化 kind 枚举。 */
enum class TraceEventKind : std::uint16_t {
    Admission = 1,
    Rejected = 2,
    TaskReady = 3,
    TaskClaimed = 4,
    TaskFirstStart = 5,
    TaskSegmentEnd = 6,
    TaskTerminal = 7,
    CancelRequested = 8,
    LocalClaim = 9,
    GlobalClaim = 10,
    StealSuccess = 11,
    StealAttempt = 12,        // Verbose：默认关闭
    WorkerPark = 13,
    WorkerWake = 14,
    CoroutineSuspend = 15,
    CoroutineResume = 16,
    CoroutineYield = 17,
    TimerRegister = 18,
    TimerFire = 19,
    TimerCancel = 20,
    GraphAccepted = 21,
    GraphTerminal = 22,
    NodeDependencyRelease = 23,
    DeadlineMet = 24,
    DeadlineMissed = 25,
    WaitBegin = 26,
    WaitEnd = 27,
    AwaitArmed = 28,
    AwaitTriggered = 29,
    AwaitResumed = 30,
    RuntimeHandoff = 31,
    RuntimeJoinReady = 32,
    RuntimeJoined = 33,
    FinalizationBegin = 34,
    FinalizationEscalate = 35,
    CoordinatorExit = 36,
    FinalizationComplete = 37,
    UnobservedFailure = 38,   // R-060：仅在活动 Trace 可用时尽力发出的诊断事件
};

// EventKind → TraceCategory 固定映射（D-158 category 集合）。
[[nodiscard]] constexpr TraceCategory category_for_kind(TraceEventKind kind) noexcept {
    switch (kind) {
        case TraceEventKind::Admission:
        case TraceEventKind::Rejected:
        case TraceEventKind::TaskReady:
        case TraceEventKind::TaskClaimed:
        case TraceEventKind::TaskFirstStart:
        case TraceEventKind::TaskSegmentEnd:
        case TraceEventKind::TaskTerminal:
        case TraceEventKind::CancelRequested:
            return TraceCategory::TaskLifecycle;
        case TraceEventKind::LocalClaim:
        case TraceEventKind::GlobalClaim:
        case TraceEventKind::StealSuccess:
            return TraceCategory::QueueScheduling;
        case TraceEventKind::StealAttempt:
            return TraceCategory::StealAttempt;
        case TraceEventKind::WorkerPark:
        case TraceEventKind::WorkerWake:
            return TraceCategory::QueueScheduling;
        case TraceEventKind::CoroutineSuspend:
        case TraceEventKind::CoroutineResume:
        case TraceEventKind::CoroutineYield:
            return TraceCategory::Coroutine;
        case TraceEventKind::TimerRegister:
        case TraceEventKind::TimerFire:
        case TraceEventKind::TimerCancel:
            return TraceCategory::Timer;
        case TraceEventKind::GraphAccepted:
        case TraceEventKind::GraphTerminal:
        case TraceEventKind::NodeDependencyRelease:
            return TraceCategory::Graph;
        case TraceEventKind::DeadlineMet:
        case TraceEventKind::DeadlineMissed:
            return TraceCategory::Deadline;
        case TraceEventKind::WaitBegin:
        case TraceEventKind::WaitEnd:
        case TraceEventKind::AwaitArmed:
        case TraceEventKind::AwaitTriggered:
        case TraceEventKind::AwaitResumed:
            return TraceCategory::WaitAwait;
        case TraceEventKind::RuntimeHandoff:
        case TraceEventKind::RuntimeJoinReady:
        case TraceEventKind::RuntimeJoined:
            return TraceCategory::RuntimeLifecycle;
        case TraceEventKind::FinalizationBegin:
        case TraceEventKind::FinalizationEscalate:
        case TraceEventKind::CoordinatorExit:
        case TraceEventKind::FinalizationComplete:
        case TraceEventKind::UnobservedFailure:
            return TraceCategory::RuntimeLifecycle;
    }
    return TraceCategory::None;
}


// 不可变、可复制的 capture 结果（D-163）：复制只共享同一只读 backing，
// 可在 Collector 与 Runtime 销毁后继续离线导出。
/**
 * @brief 不可变、可复制的 Trace capture 结果。
 * @note 复制只共享只读 backing，可在 Collector 销毁后继续导出。
 */
class ASTRA_EXPORT TraceSnapshot {
public:
    struct ProducerReport {
        std::uint64_t producer_id{0};
        TraceProducerKind kind{TraceProducerKind::Worker};
        std::size_t capacity{0};
        std::uint64_t dropped_events{0};
    };

    TraceSnapshot() noexcept = default;

    // 空/默认构造为 false。复制只共享只读 backing。
    [[nodiscard]] explicit operator bool() const noexcept { return impl_ != nullptr; }
    [[nodiscard]] std::uint32_t schema_version() const noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point origin() const noexcept;
    [[nodiscard]] std::size_t event_record_size() const noexcept;
    [[nodiscard]] TraceCategory categories() const noexcept;
    [[nodiscard]] std::size_t producer_count() const noexcept;
    // 须 index < producer_count()；越界抛 out_of_range。
    [[nodiscard]] const ProducerReport& producer(std::size_t index) const;
    // capture 内原始事件；跨 producer 未排序。导出请用 trace_ordered_events()。
    [[nodiscard]] const std::vector<TraceEvent>& events() const noexcept;
    [[nodiscard]] std::uint64_t total_dropped_events() const noexcept;

private:
    struct Data;
    std::shared_ptr<const Data> impl_;

    explicit TraceSnapshot(std::shared_ptr<const Data> data) noexcept;

    friend class TraceCollector;
    friend class TraceCapture;
};

// 导出确定全序（D-139）：按 (timestamp_ns, producer_id, local_sequence) 排序。
// (producer_id, local_sequence) 唯一 ⇒ 全序确定，相同 snapshot 重放结果一致。
/**
 * @brief 返回按时间戳和 producer 序号确定排序的 Trace 事件副本。
 * @param snapshot 要排序的 Trace capture 结果。
 */
[[nodiscard]] ASTRA_EXPORT std::vector<TraceEvent> trace_ordered_events(const TraceSnapshot& snapshot);

// move-only 活动 capture capability（D-138 / D-163）：显式 stop 提交 Snapshot；
// 活动析构 noexcept abort 丢弃该代并使 Collector 回到 Stopped。
/**
 * @brief 一代活动 Trace capture 的 move-only 控制权。
 * @note stop() 会提交不可变快照；未 stop 的析构会 abort 当前代。
 */
class ASTRA_EXPORT TraceCapture {
public:
    TraceCapture() noexcept = default;
    ~TraceCapture() noexcept;
    TraceCapture(TraceCapture&& other) noexcept;
    TraceCapture& operator=(TraceCapture&& other) noexcept;
    TraceCapture(const TraceCapture&) = delete;
    TraceCapture& operator=(const TraceCapture&) = delete;

    // 关闭本次 capture 并返回不可变 Snapshot。重复/并发 stop 共享同一 backing。
    // 空/moved-from 抛 logic_error。析构未 stop 则 abort 丢弃该代（D-163）。
    [[nodiscard]] TraceSnapshot stop();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool recording() const noexcept;

private:
    struct Impl;
    std::shared_ptr<TraceCollector> collector_;
    std::shared_ptr<Impl> impl_;
    std::shared_ptr<const TraceSnapshot::Data> result_;

    TraceCapture(std::shared_ptr<TraceCollector> collector, std::shared_ptr<Impl> impl) noexcept;

    friend class TraceCollector;
};

// 线程安全共享 TraceCollector（D-138）：用户显式创建并以 shared_ptr 附加到
// 一个或多个 Runtime；初始 Stopped，可重复执行单一活动 capture generation。
// 线程安全共享收集器。以 shared_ptr 挂到 SchedulerOptions::trace_collector。
// 初始 Stopped，同时只允许一代活动 capture。
/**
 * @brief 线程安全共享的 Trace 收集器。
 * @note 同时只允许一代活动 capture，可附加到多个 Runtime。
 */
class ASTRA_EXPORT TraceCollector : public std::enable_shared_from_this<TraceCollector> {
public:
    TraceCollector();
    ~TraceCollector();
    TraceCollector(const TraceCollector&) = delete;
    TraceCollector& operator=(const TraceCollector&) = delete;

    /**
     * @brief 校验配置并启动一代有界 Trace capture。
     * @param options capture 容量和事件分类配置。
     */
    // 校验（状态改变前）→ 预分配全部 registered producer buffer → 发布新 Recording
    // generation 并返回 Capture。零容量/未知 bit 抛 invalid_argument，总 buffer
    // 溢出抛 length_error，分配失败保持 Stopped 并重抛 bad_alloc；已有 Recording
    // 时并发第二次 start 确定性失败。
    [[nodiscard]] TraceCapture start_capture(TraceOptions options = {});

    [[nodiscard]] bool recording() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    [[nodiscard]] TraceSnapshot stop_capture(TraceCapture::Impl& capture);
    void abort_capture(TraceCapture::Impl& capture) noexcept;

    friend class TraceCapture;
    friend struct detail::CollectorAccess;
};

}  // namespace astra

#endif  // ASTRA_TRACE_HPP

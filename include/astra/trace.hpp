#ifndef ASTRA_TRACE_HPP
#define ASTRA_TRACE_HPP

// AstraScheduler 有界可重复 Trace capture（AST-045 / R-086 / D-138 / D-158 / D-163）。
// 线程安全共享 TraceCollector：显式 start_capture/stop、固定容量、重复代际、
// drop-newest 计 loss；emit 无分配/文件 I/O/callback/阻塞，Runtime 热路径安全。
// 完整 versioned TraceEvent EventKind 族由 R-087（AST-046）扩展。

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
struct TraceOptions {
    std::size_t events_per_worker = 16'384;
    std::size_t external_control_events = 65'536;
    std::size_t events_per_reaper_producer = 4'096;
    TraceCategory categories = TraceCategory::Default;
};

// Producer buffer 类别（D-138：per-Worker SPSC ring / 共享 external/control MPMC
// 入口 / 每 Reaper 独立 producer）。
enum class TraceProducerKind : std::uint8_t {
    Worker,
    ExternalControl,
    Reaper,
};

// 固定大小 trivially-copyable 事件记录（R-087 的核心 identity 字段；
// 完整枚举与 Graph/Segment 字段由 AST-046 版本化扩展）。缺失 identity 为零值 sentinel。
struct TraceEvent {
    std::uint32_t schema_version{1};
    std::uint16_t category{0};        // TraceCategory raw bit
    std::uint16_t kind{0};            // EventKind（R-087 版本化）
    std::uint64_t timestamp_ns{0};    // capture-relative steady timestamp
    std::uint64_t producer_id{0};
    std::uint64_t local_sequence{0};  // 每 producer 严格递增
    std::uint64_t runtime_id{0};
    std::uint64_t worker_id{0};
    std::uint64_t task_id{0};
};

// 不可变、可复制的 capture 结果（D-163）：复制只共享同一只读 backing，
// 可在 Collector 与 Runtime 销毁后继续离线导出。
class ASTRA_EXPORT TraceSnapshot {
public:
    struct ProducerReport {
        std::uint64_t producer_id{0};
        TraceProducerKind kind{TraceProducerKind::Worker};
        std::size_t capacity{0};
        std::uint64_t dropped_events{0};
    };

    TraceSnapshot() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept { return impl_ != nullptr; }
    [[nodiscard]] std::uint32_t schema_version() const noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point origin() const noexcept;
    [[nodiscard]] std::size_t event_record_size() const noexcept;
    [[nodiscard]] TraceCategory categories() const noexcept;
    [[nodiscard]] std::size_t producer_count() const noexcept;
    [[nodiscard]] const ProducerReport& producer(std::size_t index) const;
    [[nodiscard]] const std::vector<TraceEvent>& events() const noexcept;
    [[nodiscard]] std::uint64_t total_dropped_events() const noexcept;

private:
    struct Data;
    std::shared_ptr<const Data> impl_;

    explicit TraceSnapshot(std::shared_ptr<const Data> data) noexcept;

    friend class TraceCollector;
    friend class TraceCapture;
};

// move-only 活动 capture capability（D-138 / D-163）：显式 stop 提交 Snapshot；
// 活动析构 noexcept abort 丢弃该代并使 Collector 回到 Stopped。
class ASTRA_EXPORT TraceCapture {
public:
    TraceCapture() noexcept = default;
    ~TraceCapture() noexcept;
    TraceCapture(TraceCapture&& other) noexcept;
    TraceCapture& operator=(TraceCapture&& other) noexcept;
    TraceCapture(const TraceCapture&) = delete;
    TraceCapture& operator=(const TraceCapture&) = delete;

    // 首次调用线性化关闭该 capture 并返回 immutable Snapshot；同一 Capture
    // 重复/并发 stop 共享同一 backing。empty/moved-from 抛 std::logic_error。
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
class ASTRA_EXPORT TraceCollector : public std::enable_shared_from_this<TraceCollector> {
public:
    TraceCollector();
    ~TraceCollector();
    TraceCollector(const TraceCollector&) = delete;
    TraceCollector& operator=(const TraceCollector&) = delete;

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

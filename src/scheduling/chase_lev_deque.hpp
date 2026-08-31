#ifndef ASTRA_SRC_CHASE_LEV_DEQUE_HPP
#define ASTRA_SRC_CHASE_LEV_DEQUE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#if defined(__SANITIZE_THREAD__)
#include <sanitizer/tsan_interface.h>
#define ASTRA_TSAN_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#include <sanitizer/tsan_interface.h>
#define ASTRA_TSAN_ENABLED 1
#endif
#endif

namespace astra::detail {
namespace {

inline void tsan_acquire(const void* addr) noexcept {
#ifdef ASTRA_TSAN_ENABLED
    __tsan_acquire(const_cast<void*>(addr));
#else
    (void)addr;
#endif
}

inline void tsan_release(const void* addr) noexcept {
#ifdef ASTRA_TSAN_ENABLED
    __tsan_release(const_cast<void*>(addr));
#else
    (void)addr;
#endif
}

}  // namespace
}  // namespace astra::detail

// ============================================================================
// Chase-Lev work-stealing deque —— 本调度器的"本地任务队列"。
//
// 【它是什么】
//   1980 年 Chase & Lev 提出的无锁双端队列：队尾只有一个线程（owner
//   worker）推/弹，队首可以被任意数量的"小偷"（其他 worker）并发偷取。
//   这是 work-stealing 调度器的标准构件。
//
// 【为什么几乎不用锁】
//   owner 的 push/pop 只碰 bottom（自己的游标），小偷只碰 top（CAS 抢）；
//   两边只在"队列可能空了/只剩一个元素"时用一次 CAS 决胜。最坏情况
//   小偷失败重试，绝不会损坏队列。
//
// 【两种实现并存的原因】
//   ChaseLevDeque 是生产实现（按 Lê et al. 2013 的弱内存序规范，在
//   x86/ARM 上都正确）；ChaseLevSeqCstOracle 是全 seq_cst 的参考实现，
//   用来做差分验证——同一个操作序列喂给两个实现，行为必须一致。
//   生产版扩容失败会返回 false（调用方回退到全局队列），参考版则
//   无界增长（测试规模可控）。
//
// 【内存序为什么这样配】
//   push 的"写 cell -> release 写 bottom"保证小偷看到新 bottom 时一定
//   看得到数据；steal 的"acquire 读 top/bottom -> 读 cell"与之配对；
//   最后一元素的决胜用 seq_cst CAS 保证 owner/thief 恰好一人成功。
//   修改任何一处内存序前，请先读 Lê et al. 2013 的证明。
// ============================================================================
namespace astra::detail {

enum class DequeResultStatus : std::uint8_t {
    Success,
    Empty,
    Retry,
};

// =============================================================================
// ChaseLevSeqCstOracle (R-066 / D-097)
// 全序一致（seq_cst）Reference Oracle，用于与生产 portable 算法进行差分验证
// =============================================================================
template <typename T>
class ChaseLevSeqCstOracle {
private:
    // AST-053（TSan 证据）：动态 grow 的 buffer 必须经原子指针发布（steal 端
    // acquire 读取）且旧 buffer 永久保留（R-067 retention），否则并发 steal
    // 读 cells_ 与 owner 的 unique_ptr 换装构成 data race。
    struct OracleBuffer {
        std::size_t capacity;
        std::unique_ptr<std::atomic<T>[]> cells;
        explicit OracleBuffer(std::size_t cap)
            : capacity(cap), cells(std::make_unique<std::atomic<T>[]>(cap)) {}
    };

public:
    explicit ChaseLevSeqCstOracle(std::size_t initial_capacity = 64)
        : top_(0), bottom_(0) {
        auto buf = std::make_unique<OracleBuffer>(initial_capacity);
        published_.store(buf.get(), std::memory_order_relaxed);
        retired_.push_back(std::move(buf));
    }

    ~ChaseLevSeqCstOracle() = default;

    ChaseLevSeqCstOracle(const ChaseLevSeqCstOracle&) = delete;
    ChaseLevSeqCstOracle& operator=(const ChaseLevSeqCstOracle&) = delete;

    void push(T item) {
        OracleBuffer* buf = published_.load(std::memory_order_acquire);
        std::int64_t b = bottom_.load(std::memory_order_seq_cst);
        std::int64_t t = top_.load(std::memory_order_seq_cst);
        if (b - t >= static_cast<std::int64_t>(buf->capacity)) {
            buf = grow(b, t, buf);
        }
        buf->cells[static_cast<std::size_t>(b) & (buf->capacity - 1)].store(item, std::memory_order_seq_cst);
        bottom_.store(b + 1, std::memory_order_seq_cst);
    }

    DequeResultStatus pop(T& out) {
        OracleBuffer* buf = published_.load(std::memory_order_acquire);
        std::int64_t b = bottom_.load(std::memory_order_seq_cst) - 1;
        bottom_.store(b, std::memory_order_seq_cst);
        std::int64_t t = top_.load(std::memory_order_seq_cst);

        if (t <= b) {
            T item = buf->cells[static_cast<std::size_t>(b) & (buf->capacity - 1)].load(std::memory_order_seq_cst);
            if (t == b) {
                // 争抢最后一个元素
                if (top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_seq_cst)) {
                    bottom_.store(b + 1, std::memory_order_seq_cst);
                    out = item;
                    return DequeResultStatus::Success;
                }
                bottom_.store(b + 1, std::memory_order_seq_cst);
                return DequeResultStatus::Empty;
            }
            out = item;
            return DequeResultStatus::Success;
        }

        bottom_.store(b + 1, std::memory_order_seq_cst);
        return DequeResultStatus::Empty;
    }

    DequeResultStatus steal(T& out) {
        OracleBuffer* buf = published_.load(std::memory_order_acquire);
        std::int64_t t = top_.load(std::memory_order_seq_cst);
        std::int64_t b = bottom_.load(std::memory_order_seq_cst);

        if (t < b) {
            T item = buf->cells[static_cast<std::size_t>(t) & (buf->capacity - 1)].load(std::memory_order_seq_cst);
            if (top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_seq_cst)) {
                out = item;
                return DequeResultStatus::Success;
            }
            return DequeResultStatus::Retry;
        }

        return DequeResultStatus::Empty;
    }

    bool empty() const noexcept {
        OracleBuffer* buf = published_.load(std::memory_order_acquire);
        std::int64_t b = bottom_.load(std::memory_order_seq_cst);
        std::int64_t t = top_.load(std::memory_order_seq_cst);
        return b <= t;
    }

    std::size_t size() const noexcept {
        OracleBuffer* buf = published_.load(std::memory_order_acquire);
        std::int64_t b = bottom_.load(std::memory_order_seq_cst);
        std::int64_t t = top_.load(std::memory_order_seq_cst);
        return (b > t) ? static_cast<std::size_t>(b - t) : 0;
    }

private:
    OracleBuffer* grow(std::int64_t b, std::int64_t t, OracleBuffer* old_buf) {
        auto new_buf = std::make_unique<OracleBuffer>(old_buf->capacity * 2);
        for (std::int64_t i = t; i < b; ++i) {
            T val = old_buf->cells[static_cast<std::size_t>(i) & (old_buf->capacity - 1)].load(std::memory_order_seq_cst);
            new_buf->cells[static_cast<std::size_t>(i) & (new_buf->capacity - 1)].store(val, std::memory_order_seq_cst);
        }
        OracleBuffer* raw = new_buf.get();
        retired_.push_back(std::move(new_buf));  // R-067 retention：旧 buffer 永久保留
        published_.store(raw, std::memory_order_release);
        return raw;
    }

    std::atomic<OracleBuffer*> published_{nullptr};
    std::vector<std::unique_ptr<OracleBuffer>> retired_;
    std::atomic<std::int64_t> top_;
    std::atomic<std::int64_t> bottom_;
};

// =============================================================================
// ChaseLevDeque (R-066 / D-098)
// 生产 Portable C++20 Chase-Lev Deque
// 严格遵循 Lê et al. 2013 portable memory ordering 规范
// =============================================================================
template <typename T>
class ChaseLevDeque {
public:
    struct Buffer {
        std::size_t capacity;
        std::unique_ptr<std::atomic<T>[]> cells;

        explicit Buffer(std::size_t cap)
            : capacity(cap), cells(std::make_unique<std::atomic<T>[]>(cap)) {}

        void store_cell(std::uint64_t i, T item, std::memory_order order) noexcept {
            cells[static_cast<std::size_t>(i) & (capacity - 1)].store(item, order);
        }

        T load_cell(std::uint64_t i, std::memory_order order) const noexcept {
            return cells[static_cast<std::size_t>(i) & (capacity - 1)].load(order);
        }
    };

    explicit ChaseLevDeque(std::size_t initial_capacity = 64)
        : top_(0), bottom_(0) {
        auto buf = std::make_unique<Buffer>(initial_capacity);
        active_buffer_.store(buf.get(), std::memory_order_relaxed);
        history_buffers_.push_back(std::move(buf));
    }

    ~ChaseLevDeque() = default;

    ChaseLevDeque(const ChaseLevDeque&) = delete;
    ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;

    // Owner Bottom Push (D-098 / D-099 / D-100 / D-101):
    // 高水位先 quiescent rebase；relaxed load bottom -> acquire load top ->
    // relaxed store cell -> release fence -> relaxed store bottom。
    // 扩容失败返回 false，调用方回退至 Global Injection Queue (D-100)。
    bool push(T item) {
        std::uint64_t b = bottom_.load(std::memory_order_relaxed);
        std::uint64_t t = top_.load(std::memory_order_acquire);
        if (b >= kDefaultRebaseHighWatermark || t >= kDefaultRebaseHighWatermark) {
            (void)maybe_quiescent_rebase();
            b = bottom_.load(std::memory_order_relaxed);
            t = top_.load(std::memory_order_acquire);
        }
        Buffer* buf = active_buffer_.load(std::memory_order_relaxed);
        if (buf == nullptr || b < t) {
            return false;
        }

        // 始终保留一个空 cell（D-099 / D-103）
        if (b - t >= static_cast<std::uint64_t>(buf->capacity - 1)) {
            buf = grow(b, t, buf);
            if (buf == nullptr) {
                return false;
            }
        }

        buf->store_cell(b, item, std::memory_order_relaxed);
        tsan_release(buf);
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
        return true;
    }

    // Owner Bottom Pop (D-098 / D-103):
    // bottom==0 时不得 unsigned underflow；空结果在高水位触发 rebase。
    DequeResultStatus pop(T& out) {
        std::uint64_t b = bottom_.load(std::memory_order_relaxed);
        if (b == 0) {
            (void)maybe_quiescent_rebase();
            return DequeResultStatus::Empty;
        }
        Buffer* buf = active_buffer_.load(std::memory_order_relaxed);
        --b;
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        tsan_acquire(buf);
        tsan_release(buf);
        std::uint64_t t = top_.load(std::memory_order_relaxed);

        if (t <= b) {
            T item = buf->load_cell(b, std::memory_order_relaxed);
            if (t == b) {
                if (top_.compare_exchange_strong(
                        t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                    bottom_.store(b + 1, std::memory_order_relaxed);
                    out = item;
                    return DequeResultStatus::Success;
                }
                bottom_.store(b + 1, std::memory_order_relaxed);
                (void)maybe_quiescent_rebase();
                return DequeResultStatus::Empty;
            }
            out = item;
            return DequeResultStatus::Success;
        }

        bottom_.store(b + 1, std::memory_order_relaxed);
        (void)maybe_quiescent_rebase();
        return DequeResultStatus::Empty;
    }

    // Thief Top Steal (D-098 / D-101 / D-102):
    // maintenance 期间返回 Retry；active-thief guard 双检后才进入 cell。
    DequeResultStatus steal(T& out) {
        if (maintenance_.load(std::memory_order_acquire)) {
            return DequeResultStatus::Retry;
        }
        active_thieves_.fetch_add(1, std::memory_order_acq_rel);
        if (maintenance_.load(std::memory_order_acquire)) {
            active_thieves_.fetch_sub(1, std::memory_order_release);
            return DequeResultStatus::Retry;
        }

        std::uint64_t t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::uint64_t b = bottom_.load(std::memory_order_acquire);
        DequeResultStatus status = DequeResultStatus::Empty;
        if (t < b) {
            Buffer* buf = active_buffer_.load(std::memory_order_acquire);
            T item = buf->load_cell(t, std::memory_order_relaxed);
            if (top_.compare_exchange_strong(
                    t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                tsan_acquire(buf);
                out = item;
                status = DequeResultStatus::Success;
            } else {
                status = DequeResultStatus::Retry;
            }
        }
        active_thieves_.fetch_sub(1, std::memory_order_release);
        return status;
    }

    bool empty() const noexcept {
        std::uint64_t b = bottom_.load(std::memory_order_relaxed);
        std::uint64_t t = top_.load(std::memory_order_acquire);
        return b <= t;
    }

    std::size_t size() const noexcept {
        std::uint64_t b = bottom_.load(std::memory_order_relaxed);
        std::uint64_t t = top_.load(std::memory_order_acquire);
        return (b > t) ? static_cast<std::size_t>(b - t) : 0;
    }

    std::size_t history_buffer_count() const noexcept {
        return history_buffers_.size();
    }

    std::size_t current_capacity() const noexcept {
        Buffer* buf = active_buffer_.load(std::memory_order_relaxed);
        return buf ? buf->capacity : 0;
    }

    static constexpr bool is_lock_free() noexcept {
        return std::atomic<std::uint64_t>::is_always_lock_free &&
               std::atomic<Buffer*>::is_always_lock_free &&
               std::atomic<T>::is_always_lock_free;
    }

    // Quiescent Rebase (R-068 / D-101):
    // 设置 maintenance，等待 active thief 归零后把 live interval 规范化到 0。
    bool maybe_quiescent_rebase(
        std::uint64_t high_watermark = kDefaultRebaseHighWatermark) noexcept {
        std::uint64_t b = bottom_.load(std::memory_order_relaxed);
        std::uint64_t t = top_.load(std::memory_order_relaxed);
        if (b < high_watermark && t < high_watermark) {
            return false;
        }

        maintenance_.store(true, std::memory_order_release);
        while (active_thieves_.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }

        b = bottom_.load(std::memory_order_relaxed);
        t = top_.load(std::memory_order_acquire);
        Buffer* buf = active_buffer_.load(std::memory_order_acquire);
        if (buf == nullptr || b < t) {
            maintenance_.store(false, std::memory_order_release);
            return false;
        }

        const std::uint64_t live = b - t;
        if (live > 0) {
            try {
                std::vector<T> snapshot;
                snapshot.reserve(static_cast<std::size_t>(live));
                for (std::uint64_t i = 0; i < live; ++i) {
                    snapshot.push_back(buf->load_cell(t + i, std::memory_order_relaxed));
                }
                for (std::uint64_t i = 0; i < live; ++i) {
                    buf->store_cell(i, snapshot[static_cast<std::size_t>(i)],
                                    std::memory_order_relaxed);
                }
            } catch (...) {
                maintenance_.store(false, std::memory_order_release);
                return false;
            }
        }

        tsan_release(buf);
        std::atomic_thread_fence(std::memory_order_release);
        top_.store(0, std::memory_order_relaxed);
        bottom_.store(live, std::memory_order_relaxed);
        maintenance_.store(false, std::memory_order_release);
        return true;
    }

    void set_test_indices(std::uint64_t t, std::uint64_t b) noexcept {
        top_.store(t, std::memory_order_relaxed);
        bottom_.store(b, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t top_for_testing() const noexcept {
        return top_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t bottom_for_testing() const noexcept {
        return bottom_.load(std::memory_order_relaxed);
    }

    void set_maintenance_for_testing(bool enabled) noexcept {
        maintenance_.store(enabled, std::memory_order_release);
    }

    void set_inject_growth_failure(bool inject) noexcept {
        inject_growth_failure_.store(inject, std::memory_order_relaxed);
    }

private:
    static constexpr std::uint64_t kDefaultRebaseHighWatermark = (UINT64_C(1) << 58);

    Buffer* grow(std::uint64_t b, std::uint64_t t, Buffer* old_buf) {
        if (inject_growth_failure_.load(std::memory_order_relaxed)) {
            return nullptr;
        }
        // Checked capacity doubling (R-068 / D-103)
        if (old_buf->capacity > (static_cast<std::size_t>(-1) / 2) ||
            old_buf->capacity >= (static_cast<std::size_t>(1) << 62)) {
            return nullptr;
        }
        try {
            std::size_t new_cap = old_buf->capacity * 2;
            auto new_buf = std::make_unique<Buffer>(new_cap);
            for (std::uint64_t i = t; i < b; ++i) {
                T val = old_buf->load_cell(i, std::memory_order_relaxed);
                new_buf->store_cell(i, val, std::memory_order_relaxed);
            }
            Buffer* raw_ptr = new_buf.get();
            history_buffers_.push_back(std::move(new_buf));
            active_buffer_.store(raw_ptr, std::memory_order_release);
            return raw_ptr;
        } catch (...) {
            return nullptr;
        }
    }

    std::atomic<std::uint64_t> top_;
    std::atomic<std::uint64_t> bottom_;
    std::atomic<Buffer*> active_buffer_;
    std::vector<std::unique_ptr<Buffer>> history_buffers_;
    std::atomic<bool> inject_growth_failure_{false};
    std::atomic<bool> maintenance_{false};
    std::atomic<std::uint32_t> active_thieves_{0};
};

}  // namespace astra::detail

#endif  // ASTRA_SRC_CHASE_LEV_DEQUE_HPP

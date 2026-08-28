#ifndef ASTRA_SRC_CHASE_LEV_DEQUE_HPP
#define ASTRA_SRC_CHASE_LEV_DEQUE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

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
public:
    explicit ChaseLevSeqCstOracle(std::size_t initial_capacity = 64)
        : capacity_(initial_capacity),
          cells_(std::make_unique<std::atomic<T>[]>(initial_capacity)),
          top_(0),
          bottom_(0) {}

    ~ChaseLevSeqCstOracle() = default;

    ChaseLevSeqCstOracle(const ChaseLevSeqCstOracle&) = delete;
    ChaseLevSeqCstOracle& operator=(const ChaseLevSeqCstOracle&) = delete;

    void push(T item) {
        std::int64_t b = bottom_.load(std::memory_order_seq_cst);
        std::int64_t t = top_.load(std::memory_order_seq_cst);
        if (b - t >= static_cast<std::int64_t>(capacity_)) {
            grow(b, t);
        }
        cells_[static_cast<std::size_t>(b) & (capacity_ - 1)].store(item, std::memory_order_seq_cst);
        bottom_.store(b + 1, std::memory_order_seq_cst);
    }

    DequeResultStatus pop(T& out) {
        std::int64_t b = bottom_.load(std::memory_order_seq_cst) - 1;
        bottom_.store(b, std::memory_order_seq_cst);
        std::int64_t t = top_.load(std::memory_order_seq_cst);

        if (t <= b) {
            T item = cells_[static_cast<std::size_t>(b) & (capacity_ - 1)].load(std::memory_order_seq_cst);
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
        std::int64_t t = top_.load(std::memory_order_seq_cst);
        std::int64_t b = bottom_.load(std::memory_order_seq_cst);

        if (t < b) {
            T item = cells_[static_cast<std::size_t>(t) & (capacity_ - 1)].load(std::memory_order_seq_cst);
            if (top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_seq_cst)) {
                out = item;
                return DequeResultStatus::Success;
            }
            return DequeResultStatus::Retry;
        }

        return DequeResultStatus::Empty;
    }

    bool empty() const noexcept {
        std::int64_t b = bottom_.load(std::memory_order_seq_cst);
        std::int64_t t = top_.load(std::memory_order_seq_cst);
        return b <= t;
    }

    std::size_t size() const noexcept {
        std::int64_t b = bottom_.load(std::memory_order_seq_cst);
        std::int64_t t = top_.load(std::memory_order_seq_cst);
        return (b > t) ? static_cast<std::size_t>(b - t) : 0;
    }

private:
    void grow(std::int64_t b, std::int64_t t) {
        std::size_t new_cap = capacity_ * 2;
        auto new_cells = std::make_unique<std::atomic<T>[]>(new_cap);
        for (std::int64_t i = t; i < b; ++i) {
            T val = cells_[static_cast<std::size_t>(i) & (capacity_ - 1)].load(std::memory_order_seq_cst);
            new_cells[static_cast<std::size_t>(i) & (new_cap - 1)].store(val, std::memory_order_seq_cst);
        }
        cells_ = std::move(new_cells);
        capacity_ = new_cap;
    }

    std::size_t capacity_;
    std::unique_ptr<std::atomic<T>[]> cells_;
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

        void store_cell(std::int64_t i, T item, std::memory_order order) noexcept {
            cells[static_cast<std::size_t>(i) & (capacity - 1)].store(item, order);
        }

        T load_cell(std::int64_t i, std::memory_order order) const noexcept {
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

    // Owner Bottom Push (D-098):
    // relaxed load bottom -> acquire load top -> relaxed store cell -> release fence -> relaxed store bottom
    void push(T item) {
        std::int64_t b = bottom_.load(std::memory_order_relaxed);
        std::int64_t t = top_.load(std::memory_order_acquire);
        Buffer* buf = active_buffer_.load(std::memory_order_relaxed);

        if (b - t >= static_cast<std::int64_t>(buf->capacity)) {
            buf = grow(b, t, buf);
        }

        buf->store_cell(b, item, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
    }

    // Owner Bottom Pop (D-098):
    // relaxed store bottom-1 -> seq_cst fence -> relaxed load top -> last-item seq_cst CAS
    DequeResultStatus pop(T& out) {
        std::int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        Buffer* buf = active_buffer_.load(std::memory_order_relaxed);
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::int64_t t = top_.load(std::memory_order_relaxed);

        if (t <= b) {
            T item = buf->load_cell(b, std::memory_order_relaxed);
            if (t == b) {
                // 争抢最后一个元素：以 seq_cst CAS 与并发 thief 决胜，失败 relaxed
                if (top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                    bottom_.store(b + 1, std::memory_order_relaxed);
                    out = item;
                    return DequeResultStatus::Success;
                }
                bottom_.store(b + 1, std::memory_order_relaxed);
                return DequeResultStatus::Empty;
            }
            out = item;
            return DequeResultStatus::Success;
        }

        bottom_.store(b + 1, std::memory_order_relaxed);
        return DequeResultStatus::Empty;
    }

    // Thief Top Steal (D-098):
    // acquire load top -> seq_cst fence -> acquire load bottom -> acquire load buffer -> relaxed load cell -> seq_cst CAS
    DequeResultStatus steal(T& out) {
        std::int64_t t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::int64_t b = bottom_.load(std::memory_order_acquire);

        if (t < b) {
            Buffer* buf = active_buffer_.load(std::memory_order_acquire);
            T item = buf->load_cell(t, std::memory_order_relaxed);
            if (top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                out = item;
                return DequeResultStatus::Success;
            }
            return DequeResultStatus::Retry;
        }

        return DequeResultStatus::Empty;
    }

    bool empty() const noexcept {
        std::int64_t b = bottom_.load(std::memory_order_relaxed);
        std::int64_t t = top_.load(std::memory_order_acquire);
        return b <= t;
    }

    std::size_t size() const noexcept {
        std::int64_t b = bottom_.load(std::memory_order_relaxed);
        std::int64_t t = top_.load(std::memory_order_acquire);
        return (b > t) ? static_cast<std::size_t>(b - t) : 0;
    }

private:
    Buffer* grow(std::int64_t b, std::int64_t t, Buffer* old_buf) {
        std::size_t new_cap = old_buf->capacity * 2;
        auto new_buf = std::make_unique<Buffer>(new_cap);
        for (std::int64_t i = t; i < b; ++i) {
            T val = old_buf->load_cell(i, std::memory_order_relaxed);
            new_buf->store_cell(i, val, std::memory_order_relaxed);
        }
        Buffer* raw_ptr = new_buf.get();
        history_buffers_.push_back(std::move(new_buf));
        active_buffer_.store(raw_ptr, std::memory_order_release);
        return raw_ptr;
    }

    std::atomic<std::int64_t> top_;
    std::atomic<std::int64_t> bottom_;
    std::atomic<Buffer*> active_buffer_;
    std::vector<std::unique_ptr<Buffer>> history_buffers_;
};

}  // namespace astra::detail

#endif  // ASTRA_SRC_CHASE_LEV_DEQUE_HPP

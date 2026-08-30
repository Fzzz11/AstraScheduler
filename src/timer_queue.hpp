#ifndef ASTRA_SRC_TIMER_QUEUE_HPP
#define ASTRA_SRC_TIMER_QUEUE_HPP

#include "await_handshake.hpp"
#include "runtime_metrics.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace astra::detail {

class TimerQueue {
public:
    struct DueItem {
        std::shared_ptr<AwaitHandshake> handshake;
        std::function<void()> resume_action;
        std::chrono::steady_clock::time_point wake_time{};
    };

    explicit TimerQueue(RuntimeMetrics& metrics);

    std::uint64_t add(
        std::chrono::steady_clock::time_point wake_time,
        std::shared_ptr<AwaitHandshake> handshake,
        std::function<void()> resume_action,
        bool& became_earliest);

    void cancel(std::uint64_t timer_id);
    std::vector<DueItem> collect_due(std::chrono::steady_clock::time_point now);
    std::vector<DueItem> cancel_all();
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> earliest_wake_time() const;
    [[nodiscard]] bool empty() const;
    [[nodiscard]] std::size_t size() const;

private:
    struct TimerEntry {
        std::uint64_t timer_id{0};
        std::chrono::steady_clock::time_point wake_time{};
        std::uint64_t sequence{0};
        std::shared_ptr<AwaitHandshake> handshake{nullptr};
        std::function<void()> resume_action{nullptr};
        std::size_t heap_index{0};
    };

    static bool compare_timer_entry(const TimerEntry& a, const TimerEntry& b) noexcept;
    void bubble_up(std::size_t idx);
    void bubble_down(std::size_t idx);

    RuntimeMetrics& metrics_;
    mutable std::mutex mutex_;
    std::atomic<std::uint64_t> next_timer_id_{1};
    std::atomic<std::uint64_t> next_timer_sequence_{1};
    std::vector<std::shared_ptr<TimerEntry>> heap_;
    std::unordered_map<std::uint64_t, std::shared_ptr<TimerEntry>> map_;
};

}  // namespace astra::detail

#endif

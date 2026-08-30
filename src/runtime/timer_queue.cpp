#include "timer_queue.hpp"

namespace astra::detail {

TimerQueue::TimerQueue(RuntimeMetrics& metrics) : metrics_(metrics) {}

bool TimerQueue::compare_timer_entry(const TimerEntry& a, const TimerEntry& b) noexcept {
    if (a.wake_time != b.wake_time) {
        return a.wake_time < b.wake_time;
    }
    return a.sequence < b.sequence;
}

void TimerQueue::bubble_up(std::size_t idx) {
    while (idx > 0) {
        std::size_t parent = (idx - 1) / 2;
        if (compare_timer_entry(*heap_[idx], *heap_[parent])) {
            std::swap(heap_[idx], heap_[parent]);
            heap_[idx]->heap_index = idx;
            heap_[parent]->heap_index = parent;
            idx = parent;
        } else {
            break;
        }
    }
}

void TimerQueue::bubble_down(std::size_t idx) {
    const std::size_t n = heap_.size();
    while (true) {
        std::size_t smallest = idx;
        std::size_t left = 2 * idx + 1;
        std::size_t right = 2 * idx + 2;
        if (left < n && compare_timer_entry(*heap_[left], *heap_[smallest])) {
            smallest = left;
        }
        if (right < n && compare_timer_entry(*heap_[right], *heap_[smallest])) {
            smallest = right;
        }
        if (smallest != idx) {
            std::swap(heap_[idx], heap_[smallest]);
            heap_[idx]->heap_index = idx;
            heap_[smallest]->heap_index = smallest;
            idx = smallest;
        } else {
            break;
        }
    }
}

std::uint64_t TimerQueue::add(
    std::chrono::steady_clock::time_point wake_time,
    std::shared_ptr<AwaitHandshake> handshake,
    std::function<void()> resume_action,
    bool& became_earliest) {
    const std::uint64_t tid = next_timer_id_.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t seq = next_timer_sequence_.fetch_add(1, std::memory_order_relaxed);
    auto entry = std::make_shared<TimerEntry>();
    entry->timer_id = tid;
    entry->wake_time = wake_time;
    entry->sequence = seq;
    entry->handshake = std::move(handshake);
    entry->resume_action = std::move(resume_action);

    if (metrics_.level != MetricsLevel::Off) {
        RuntimeMetrics::saturating_inc(metrics_.shard_for_current().timer_registrations);
    }

    became_earliest = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entry->heap_index = heap_.size();
        heap_.push_back(entry);
        map_[tid] = entry;
        bubble_up(entry->heap_index);
        if (heap_.front()->timer_id == tid) {
            became_earliest = true;
        }
    }
    return tid;
}

void TimerQueue::cancel(std::uint64_t timer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(timer_id);
    if (it == map_.end()) {
        return;
    }
    auto entry = std::move(it->second);
    map_.erase(it);
    if (metrics_.level != MetricsLevel::Off) {
        RuntimeMetrics::saturating_inc(metrics_.shard_for_current().timer_cancellations);
    }
    const std::size_t idx = entry->heap_index;
    const std::size_t last_idx = heap_.size() - 1;
    if (idx == last_idx) {
        heap_.pop_back();
    } else {
        heap_[idx] = std::move(heap_.back());
        heap_.pop_back();
        heap_[idx]->heap_index = idx;
        bubble_down(idx);
        bubble_up(idx);
    }
}

std::vector<TimerQueue::DueItem> TimerQueue::collect_due(std::chrono::steady_clock::time_point now) {
    std::vector<DueItem> due_items;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!heap_.empty() && heap_.front()->wake_time <= now) {
            auto entry = std::move(heap_.front());
            map_.erase(entry->timer_id);
            if (heap_.size() == 1) {
                heap_.pop_back();
            } else {
                heap_[0] = std::move(heap_.back());
                heap_.pop_back();
                heap_[0]->heap_index = 0;
                bubble_down(0);
            }
            due_items.push_back({std::move(entry->handshake), std::move(entry->resume_action), entry->wake_time});
        }
    }
    if (metrics_.level != MetricsLevel::Off && !due_items.empty()) {
        RuntimeMetrics::saturating_add(metrics_.shard_for_current().timer_fires, due_items.size());
        if (metrics_.level == MetricsLevel::Detailed) {
            for (const auto& item : due_items) {
                if (now >= item.wake_time) {
                    const auto lateness = std::chrono::duration_cast<std::chrono::nanoseconds>(now - item.wake_time).count();
                    metrics_.shard_for_current().timer_wake_lateness.record(lateness);
                }
            }
        }
    }
    return due_items;
}

std::vector<TimerQueue::DueItem> TimerQueue::cancel_all() {
    std::vector<DueItem> all_items;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!heap_.empty()) {
            auto entry = std::move(heap_.front());
            map_.erase(entry->timer_id);
            if (heap_.size() == 1) {
                heap_.pop_back();
            } else {
                heap_[0] = std::move(heap_.back());
                heap_.pop_back();
                heap_[0]->heap_index = 0;
                bubble_down(0);
            }
            all_items.push_back({std::move(entry->handshake), std::move(entry->resume_action), entry->wake_time});
        }
    }
    if (metrics_.level != MetricsLevel::Off && !all_items.empty()) {
        RuntimeMetrics::saturating_add(metrics_.shard_for_current().timer_cancellations, all_items.size());
    }
    return all_items;
}

std::optional<std::chrono::steady_clock::time_point> TimerQueue::earliest_wake_time() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (heap_.empty()) {
        return std::nullopt;
    }
    return heap_.front()->wake_time;
}

bool TimerQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return heap_.empty();
}

std::size_t TimerQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return heap_.size();
}

}  // namespace astra::detail

#include "runtime/ready_queues.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace astra::detail {

ReadyQueues::ReadyQueues(std::size_t worker_count, RuntimeMetrics& metrics)
    : metrics_(metrics) {
    local_queues_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        local_queues_.push_back(std::make_unique<LocalQueues>());
    }
}

bool ReadyQueues::EdfEntry::operator>(const EdfEntry& other) const noexcept {
    if (deadline != other.deadline) {
        return deadline > other.deadline;
    }
    return admission_sequence > other.admission_sequence;
}

void ReadyQueues::LocalQueues::push(QueuedTask task, Priority priority) {
    std::lock_guard<std::mutex> lock(mutex);
    bands[static_cast<std::size_t>(priority)].push_back(std::move(task));
}

bool ReadyQueues::choose_local_band(
    std::array<std::deque<QueuedTask>, 4>& bands,
    std::size_t& calendar_index,
    bool from_back,
    QueuedTask& out) {
    const Priority target = kPriorityCalendar[calendar_index % kPriorityCalendarLength];
    auto claim = [&](Priority priority) {
        auto& queue = bands[static_cast<std::size_t>(priority)];
        if (queue.empty()) {
            return false;
        }
        out = from_back ? std::move(queue.back()) : std::move(queue.front());
        if (from_back) {
            queue.pop_back();
        } else {
            queue.pop_front();
        }
        calendar_index = (calendar_index + 1) % kPriorityCalendarLength;
        return true;
    };

    if (claim(target)) {
        return true;
    }
    for (Priority priority : kFallbackPriorityOrder) {
        if (priority != target && claim(priority)) {
            return true;
        }
    }
    return false;
}

bool ReadyQueues::LocalQueues::claim_back(
    std::size_t& calendar_index,
    QueuedTask& out) {
    std::lock_guard<std::mutex> lock(mutex);
    return ReadyQueues::choose_local_band(bands, calendar_index, true, out);
}

bool ReadyQueues::LocalQueues::steal_front(
    std::size_t& calendar_index,
    QueuedTask& out) {
    std::lock_guard<std::mutex> lock(mutex);
    return ReadyQueues::choose_local_band(bands, calendar_index, false, out);
}

bool ReadyQueues::LocalQueues::empty() const {
    std::lock_guard<std::mutex> lock(mutex);
    return bands[0].empty() && bands[1].empty() && bands[2].empty() && bands[3].empty();
}

void ReadyQueues::LocalQueues::cancel_unstarted() noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& queue : bands) {
        std::deque<QueuedTask> resumes;
        while (!queue.empty()) {
            auto task = std::move(queue.front());
            queue.pop_front();
            if (task.invoker && task.invoker->is_resume_segment()) {
                resumes.push_back(std::move(task));
            } else if (task.invoker) {
                task.invoker->cancel_pre_start();
            }
        }
        queue = std::move(resumes);
    }
}

void ReadyQueues::publish(
    std::unique_ptr<TaskInvokerBase> task,
    bool is_external,
    bool use_local_queue,
    std::size_t worker_index) {
    if (!task) {
        return;
    }

    const Priority priority = task->priority();
    const auto deadline = task->deadline();
    const bool is_resume = task->is_resume_segment();
    const std::size_t band = static_cast<std::size_t>(priority);

    if (deadline.has_value() && !is_resume) {
        if (metrics_.level != MetricsLevel::Off) {
            RuntimeMetrics::saturating_inc(metrics_.shard_for_current().deadline_admitted);
        }
        std::lock_guard<std::mutex> lock(global_mutex_);
        const std::uint64_t sequence = ++global_admission_sequence_;
        global_edf_heaps_[band].push_back(
            EdfEntry{*deadline, sequence, {std::move(task), is_external}});
        std::push_heap(
            global_edf_heaps_[band].begin(),
            global_edf_heaps_[band].end(),
            std::greater<EdfEntry>{});
        return;
    }

    if (use_local_queue && worker_index < local_queues_.size()) {
        local_queues_[worker_index]->push({std::move(task), false}, priority);
        return;
    }

    std::lock_guard<std::mutex> lock(global_mutex_);
    global_fifo_queues_[band].push_back({std::move(task), is_external});
}

bool ReadyQueues::claim_global_band_locked(
    std::size_t band_index,
    std::size_t& deadline_burst,
    QueuedTask& out) {
    auto& edf_heap = global_edf_heaps_[band_index];
    auto& fifo_queue = global_fifo_queues_[band_index];
    if (edf_heap.empty() && fifo_queue.empty()) {
        return false;
    }

    if (!edf_heap.empty() && !fifo_queue.empty()) {
        if (deadline_burst < 8) {
            std::pop_heap(edf_heap.begin(), edf_heap.end(), std::greater<EdfEntry>{});
            out = std::move(edf_heap.back().task);
            edf_heap.pop_back();
            ++deadline_burst;
        } else {
            out = std::move(fifo_queue.front());
            fifo_queue.pop_front();
            deadline_burst = 0;
        }
    } else if (!edf_heap.empty()) {
        std::pop_heap(edf_heap.begin(), edf_heap.end(), std::greater<EdfEntry>{});
        out = std::move(edf_heap.back().task);
        edf_heap.pop_back();
        ++deadline_burst;
    } else {
        out = std::move(fifo_queue.front());
        fifo_queue.pop_front();
        deadline_burst = 0;
    }
    return true;
}

bool ReadyQueues::claim_global(
    std::size_t& calendar_index,
    std::array<std::size_t, 4>& deadline_bursts,
    QueuedTask& out) {
    std::lock_guard<std::mutex> lock(global_mutex_);
    const Priority target = kPriorityCalendar[calendar_index % kPriorityCalendarLength];
    auto claim = [&](Priority priority) {
        const auto band = static_cast<std::size_t>(priority);
        if (!claim_global_band_locked(band, deadline_bursts[band], out)) {
            return false;
        }
        calendar_index = (calendar_index + 1) % kPriorityCalendarLength;
        return true;
    };

    bool claimed = claim(target);
    if (!claimed) {
        for (Priority priority : kFallbackPriorityOrder) {
            if (priority != target && claim(priority)) {
                claimed = true;
                break;
            }
        }
    }
    if (claimed) {
        claimed_count_.fetch_add(1, std::memory_order_relaxed);
        record_claim(false);
    }
    return claimed;
}

bool ReadyQueues::claim_local(
    std::size_t worker_index,
    std::size_t& calendar_index,
    QueuedTask& out) {
    if (worker_index >= local_queues_.size() ||
        !local_queues_[worker_index]->claim_back(calendar_index, out)) {
        return false;
    }
    claimed_count_.fetch_add(1, std::memory_order_relaxed);
    record_claim(true);
    return true;
}

bool ReadyQueues::steal(
    std::size_t victim_index,
    std::size_t& calendar_index,
    QueuedTask& out) {
    if (victim_index >= local_queues_.size() ||
        !local_queues_[victim_index]->steal_front(calendar_index, out)) {
        return false;
    }
    claimed_count_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ReadyQueues::record_claim(bool local) noexcept {
    if (metrics_.level == MetricsLevel::Off) {
        return;
    }
    auto& shard = metrics_.shard_for_current();
    RuntimeMetrics::saturating_inc(local ? shard.local_claims : shard.global_claims);
}

void ReadyQueues::complete_claim() noexcept {
    auto count = claimed_count_.load(std::memory_order_relaxed);
    while (count > 0 && !claimed_count_.compare_exchange_weak(
                            count, count - 1, std::memory_order_relaxed)) {
    }
}

std::size_t ReadyQueues::claimed_count() const noexcept {
    return claimed_count_.load(std::memory_order_acquire);
}

void ReadyQueues::cancel_unstarted(AdmissionController& admission) noexcept {
    {
        std::lock_guard<std::mutex> lock(global_mutex_);
        for (auto& heap : global_edf_heaps_) {
            std::vector<EdfEntry> resumes;
            for (auto& entry : heap) {
                if (entry.task.invoker && entry.task.invoker->is_resume_segment()) {
                    resumes.push_back(std::move(entry));
                } else {
                    if (entry.task.invoker) {
                        entry.task.invoker->cancel_pre_start();
                    }
                    if (entry.task.is_external) {
                        admission.release(1);
                    }
                }
            }
            heap = std::move(resumes);
            std::make_heap(heap.begin(), heap.end(), std::greater<EdfEntry>{});
        }

        for (auto& queue : global_fifo_queues_) {
            std::deque<QueuedTask> resumes;
            while (!queue.empty()) {
                auto task = std::move(queue.front());
                queue.pop_front();
                if (task.invoker && task.invoker->is_resume_segment()) {
                    resumes.push_back(std::move(task));
                } else {
                    if (task.invoker) {
                        task.invoker->cancel_pre_start();
                    }
                    if (task.is_external) {
                        admission.release(1);
                    }
                }
            }
            queue = std::move(resumes);
        }
    }

    for (auto& local : local_queues_) {
        local->cancel_unstarted();
    }
}

bool ReadyQueues::global_empty() const {
    std::lock_guard<std::mutex> lock(global_mutex_);
    for (std::size_t index = 0; index < 4; ++index) {
        if (!global_edf_heaps_[index].empty() || !global_fifo_queues_[index].empty()) {
            return false;
        }
    }
    return true;
}

bool ReadyQueues::local_empty(std::size_t worker_index) const {
    return worker_index >= local_queues_.size() || local_queues_[worker_index]->empty();
}

bool ReadyQueues::any_local_work() const {
    for (const auto& local : local_queues_) {
        if (local && !local->empty()) {
            return true;
        }
    }
    return false;
}

bool ReadyQueues::any_queued_work() const {
    return !global_empty() || any_local_work();
}

bool ReadyQueues::any_resume_work_after_immediate_cleanup() const {
    {
        std::lock_guard<std::mutex> lock(global_mutex_);
        for (const auto& queue : global_fifo_queues_) {
            for (const auto& task : queue) {
                if (task.invoker && task.invoker->is_resume_segment()) {
                    return true;
                }
            }
        }
        for (const auto& heap : global_edf_heaps_) {
            for (const auto& entry : heap) {
                if (entry.task.invoker && entry.task.invoker->is_resume_segment()) {
                    return true;
                }
            }
        }
    }
    return any_local_work();
}

std::size_t ReadyQueues::global_size() const {
    std::lock_guard<std::mutex> lock(global_mutex_);
    std::size_t total = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        total += global_edf_heaps_[index].size();
        total += global_fifo_queues_[index].size();
    }
    return total;
}

std::size_t ReadyQueues::worker_count() const noexcept {
    return local_queues_.size();
}

}  // namespace astra::detail

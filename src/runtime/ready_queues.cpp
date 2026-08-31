#include "runtime/ready_queues.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace astra::detail {

LocalDequeBackend ReadyQueues::preferred_local_backend() noexcept {
    return ChaseLevDeque<ReadyLinkedInvoker*>::is_lock_free()
               ? LocalDequeBackend::ChaseLevLockFree
               : LocalDequeBackend::Locked;
}

ReadyQueues::ReadyQueues(
    std::size_t worker_count,
    RuntimeMetrics& metrics,
    LocalDequeBackend local_backend)
    : metrics_(metrics) {
    local_queues_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        local_queues_.push_back(std::make_unique<LocalQueues>(local_backend));
    }
}

bool ReadyQueues::EdfEntry::operator>(const EdfEntry& other) const noexcept {
    if (deadline != other.deadline) {
        return deadline > other.deadline;
    }
    return admission_sequence > other.admission_sequence;
}

ReadyQueues::LocalQueues::LocalQueues(LocalDequeBackend selected_backend)
    : backend(selected_backend) {
    if (uses_chase_lev()) {
        for (auto& band : chase_lev_bands) {
            band = std::make_unique<ChaseLevDeque<ReadyLinkedInvoker*>>();
        }
    }
}

ReadyQueues::IntrusiveFifo::~IntrusiveFifo() {
    while (head != nullptr) {
        ReadyLinkedInvoker* raw = head;
        head = raw->ready_next;
        raw->ready_next = nullptr;
        delete raw;
    }
    tail = nullptr;
    count = 0;
}

ReadyQueues::IntrusiveFifo::IntrusiveFifo(IntrusiveFifo&& other) noexcept
    : head(other.head), tail(other.tail), count(other.count) {
    other.head = nullptr;
    other.tail = nullptr;
    other.count = 0;
}

ReadyQueues::IntrusiveFifo& ReadyQueues::IntrusiveFifo::operator=(
    IntrusiveFifo&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    while (head != nullptr) {
        ReadyLinkedInvoker* raw = head;
        head = raw->ready_next;
        raw->ready_next = nullptr;
        delete raw;
    }
    head = other.head;
    tail = other.tail;
    count = other.count;
    other.head = nullptr;
    other.tail = nullptr;
    other.count = 0;
    return *this;
}

void ReadyQueues::IntrusiveFifo::push_back(
    std::unique_ptr<TaskInvokerBase> task,
    bool is_external) noexcept {
    auto linked = adopt_ready_linked(std::move(task));
    ReadyLinkedInvoker* raw = linked.release();
    raw->ready_next = nullptr;
    raw->ready_is_external = is_external;
    if (tail == nullptr) {
        head = tail = raw;
    } else {
        tail->ready_next = raw;
        tail = raw;
    }
    ++count;
}

bool ReadyQueues::IntrusiveFifo::pop_front(QueuedTask& out) noexcept {
    if (head == nullptr) {
        return false;
    }
    ReadyLinkedInvoker* raw = head;
    head = raw->ready_next;
    if (head == nullptr) {
        tail = nullptr;
    }
    raw->ready_next = nullptr;
    --count;
    out.is_external = raw->ready_is_external;
    raw->ready_is_external = false;
    out.invoker.reset(raw);
    return true;
}

bool ReadyQueues::IntrusiveFifo::any_resume() const noexcept {
    for (ReadyLinkedInvoker* node = head; node != nullptr; node = node->ready_next) {
        if (node->is_resume_segment()) {
            return true;
        }
    }
    return false;
}

ReadyQueues::LocalQueues::~LocalQueues() {
    if (!uses_chase_lev()) {
        return;
    }
    for (auto& band : chase_lev_bands) {
        ReadyLinkedInvoker* raw = nullptr;
        while (true) {
            const auto status = band->steal(raw);
            if (status == DequeResultStatus::Retry) {
                raw = nullptr;
                continue;
            }
            if (status != DequeResultStatus::Success || raw == nullptr) {
                break;
            }
            delete raw;
            raw = nullptr;
        }
    }
}

bool ReadyQueues::LocalQueues::push(QueuedTask& task, Priority priority) {
    const auto band_index = static_cast<std::size_t>(priority);
    if (!uses_chase_lev()) {
        std::lock_guard<std::mutex> lock(locked_mutex);
        locked_bands[band_index].push_back(std::move(task));
        return true;
    }

    auto linked = adopt_ready_linked(std::move(task.invoker));
    ReadyLinkedInvoker* raw = linked.get();
    raw->ready_next = nullptr;
    raw->ready_is_external = task.is_external;
    if (!chase_lev_bands[band_index]->push(raw)) {
        task.invoker = std::move(linked);
        return false;
    }
    (void)linked.release();
    return true;
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
    if (uses_chase_lev()) {
        return claim_chase_lev(calendar_index, true, out);
    }
    std::lock_guard<std::mutex> lock(locked_mutex);
    return ReadyQueues::choose_local_band(locked_bands, calendar_index, true, out);
}

bool ReadyQueues::LocalQueues::steal_front(
    std::size_t& calendar_index,
    QueuedTask& out) {
    if (uses_chase_lev()) {
        return claim_chase_lev(calendar_index, false, out);
    }
    std::lock_guard<std::mutex> lock(locked_mutex);
    return ReadyQueues::choose_local_band(locked_bands, calendar_index, false, out);
}

bool ReadyQueues::LocalQueues::empty() const {
    if (uses_chase_lev()) {
        return chase_lev_bands[0]->empty() && chase_lev_bands[1]->empty() &&
               chase_lev_bands[2]->empty() && chase_lev_bands[3]->empty();
    }
    std::lock_guard<std::mutex> lock(locked_mutex);
    return locked_bands[0].empty() && locked_bands[1].empty() &&
           locked_bands[2].empty() && locked_bands[3].empty();
}

bool ReadyQueues::LocalQueues::claim_chase_lev(
    std::size_t& calendar_index,
    bool owner,
    QueuedTask& out) {
    const Priority target = kPriorityCalendar[calendar_index % kPriorityCalendarLength];
    auto claim = [&](Priority priority) -> DequeResultStatus {
        auto& band = *chase_lev_bands[static_cast<std::size_t>(priority)];
        ReadyLinkedInvoker* raw = nullptr;
        DequeResultStatus status = DequeResultStatus::Empty;
        if (owner) {
            status = band.pop(raw);
        } else {
            constexpr std::size_t kMaxStealRetries = 3;
            for (std::size_t retry = 0; retry < kMaxStealRetries; ++retry) {
                status = band.steal(raw);
                if (status != DequeResultStatus::Retry) {
                    break;
                }
            }
        }
        if (status != DequeResultStatus::Success || raw == nullptr) {
            return status == DequeResultStatus::Success ? DequeResultStatus::Empty
                                                        : status;
        }
        out.is_external = raw->ready_is_external;
        raw->ready_is_external = false;
        raw->ready_next = nullptr;
        out.invoker.reset(raw);
        calendar_index = (calendar_index + 1) % kPriorityCalendarLength;
        return DequeResultStatus::Success;
    };

    const auto target_status = claim(target);
    if (target_status == DequeResultStatus::Success) {
        return true;
    }
    if (!owner && target_status == DequeResultStatus::Retry) {
        return false;
    }
    for (Priority priority : kFallbackPriorityOrder) {
        if (priority == target) {
            continue;
        }
        const auto status = claim(priority);
        if (status == DequeResultStatus::Success) {
            return true;
        }
        if (!owner && status == DequeResultStatus::Retry) {
            return false;
        }
    }
    return false;
}

void ReadyQueues::LocalQueues::cancel_unstarted(
    std::vector<QueuedTask>& resumes) noexcept {
    if (uses_chase_lev()) {
        for (auto& band : chase_lev_bands) {
            while (true) {
                ReadyLinkedInvoker* raw = nullptr;
                const auto status = band->steal(raw);
                if (status == DequeResultStatus::Retry) {
                    continue;
                }
                if (status == DequeResultStatus::Empty || raw == nullptr) {
                    break;
                }
                QueuedTask task;
                task.is_external = raw->ready_is_external;
                raw->ready_is_external = false;
                raw->ready_next = nullptr;
                task.invoker.reset(raw);
                if (task.invoker && task.invoker->is_resume_segment()) {
                    resumes.push_back(std::move(task));
                } else if (task.invoker) {
                    task.invoker->cancel_pre_start();
                }
            }
        }
        return;
    }

    std::lock_guard<std::mutex> lock(locked_mutex);
    for (auto& queue : locked_bands) {
        std::deque<QueuedTask> retained_resumes;
        while (!queue.empty()) {
            auto task = std::move(queue.front());
            queue.pop_front();
            if (task.invoker && task.invoker->is_resume_segment()) {
                retained_resumes.push_back(std::move(task));
            } else if (task.invoker) {
                task.invoker->cancel_pre_start();
            }
        }
        queue = std::move(retained_resumes);
    }
}

void ReadyQueues::LocalQueues::set_growth_failure_for_testing(bool inject) noexcept {
    if (!uses_chase_lev()) {
        return;
    }
    for (auto& band : chase_lev_bands) {
        band->set_inject_growth_failure(inject);
    }
}

void ReadyQueues::LocalQueues::set_band_maintenance_for_testing(
    Priority priority,
    bool enabled) noexcept {
    if (!uses_chase_lev()) {
        return;
    }
    chase_lev_bands[static_cast<std::size_t>(priority)]->set_maintenance_for_testing(
        enabled);
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
        QueuedTask local_task{std::move(task), false};
        if (local_queues_[worker_index]->push(local_task, priority)) {
            return;
        }
        task = std::move(local_task.invoker);
    }

    std::lock_guard<std::mutex> lock(global_mutex_);
    global_fifo_queues_[band].push_back(std::move(task), is_external);
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
        if (deadline_burst < kEdfDeadlineBurstLimit) {
            std::pop_heap(edf_heap.begin(), edf_heap.end(), std::greater<EdfEntry>{});
            out = std::move(edf_heap.back().task);
            edf_heap.pop_back();
            ++deadline_burst;
        } else {
            (void)fifo_queue.pop_front(out);
            deadline_burst = 0;
        }
    } else if (!edf_heap.empty()) {
        std::pop_heap(edf_heap.begin(), edf_heap.end(), std::greater<EdfEntry>{});
        out = std::move(edf_heap.back().task);
        edf_heap.pop_back();
        ++deadline_burst;
    } else {
        (void)fifo_queue.pop_front(out);
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
            IntrusiveFifo retained;
            QueuedTask task;
            while (queue.pop_front(task)) {
                if (task.invoker && task.invoker->is_resume_segment()) {
                    retained.push_back(std::move(task.invoker), task.is_external);
                } else {
                    if (task.invoker) {
                        task.invoker->cancel_pre_start();
                    }
                    if (task.is_external) {
                        admission.release(1);
                    }
                }
            }
            queue = std::move(retained);
        }
    }

    std::vector<QueuedTask> local_resumes;
    for (auto& local : local_queues_) {
        local->cancel_unstarted(local_resumes);
    }
    if (!local_resumes.empty()) {
        std::lock_guard<std::mutex> lock(global_mutex_);
        for (auto& task : local_resumes) {
            const auto band = static_cast<std::size_t>(task.invoker->priority());
            global_fifo_queues_[band].push_back(std::move(task.invoker), task.is_external);
        }
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
            if (queue.any_resume()) {
                return true;
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

void ReadyQueues::set_local_growth_failure_for_testing(
    std::size_t worker_index,
    bool inject) noexcept {
    if (worker_index < local_queues_.size()) {
        local_queues_[worker_index]->set_growth_failure_for_testing(inject);
    }
}

void ReadyQueues::set_local_band_maintenance_for_testing(
    std::size_t worker_index,
    Priority priority,
    bool enabled) noexcept {
    if (worker_index < local_queues_.size()) {
        local_queues_[worker_index]->set_band_maintenance_for_testing(
            priority, enabled);
    }
}

}  // namespace astra::detail

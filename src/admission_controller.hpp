#ifndef ASTRA_SRC_ADMISSION_CONTROLLER_HPP
#define ASTRA_SRC_ADMISSION_CONTROLLER_HPP

#include "runtime_metrics.hpp"

#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>
#include <astra/status.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace astra::detail {

class AdmissionController {
public:
    AdmissionController(
        std::size_t capacity,
        ExternalBackpressure backpressure,
        const std::atomic<std::uint16_t>& packed_status,
        RuntimeMetrics& metrics);

    AdmissionDecision acquire(std::size_t count, bool block, bool is_internal);
    void release(std::size_t count);
    void wake_all();
    [[nodiscard]] std::size_t pending() const;

private:
    static SchedulerStatus unpack(std::uint16_t val) noexcept;

    std::size_t capacity_;
    ExternalBackpressure backpressure_;
    const std::atomic<std::uint16_t>& packed_status_;
    RuntimeMetrics& metrics_;
    mutable std::mutex mutex_;
    std::condition_variable slot_cv_;
    std::size_t pending_{0};
};

}  // namespace astra::detail

#endif

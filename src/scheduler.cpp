#include <astra/scheduler.hpp>

#include <atomic>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace astra {

std::size_t recommended_worker_count() noexcept {
    const unsigned int count = std::thread::hardware_concurrency();
    return count == 0 ? 1u : static_cast<std::size_t>(count);
}

namespace {

void validate_options(const SchedulerOptions& options) {
    if (options.worker_count == 0) {
        throw std::invalid_argument("SchedulerOptions::worker_count must be greater than 0");
    }
    if (options.external_pending_capacity == 0) {
        throw std::invalid_argument("SchedulerOptions::external_pending_capacity must be greater than 0");
    }
    if (options.max_helping_depth == 0) {
        throw std::invalid_argument("SchedulerOptions::max_helping_depth must be greater than 0");
    }
    if (options.local_burst_limit == 0) {
        throw std::invalid_argument("SchedulerOptions::local_burst_limit must be greater than 0");
    }
    if (options.steal_probe_limit == 0) {
        throw std::invalid_argument("SchedulerOptions::steal_probe_limit must be greater than 0");
    }
    if (options.external_backpressure != ExternalBackpressure::Reject &&
        options.external_backpressure != ExternalBackpressure::Block) {
        throw std::invalid_argument("SchedulerOptions::external_backpressure contains unknown enum value");
    }
    if (options.metrics_level != MetricsLevel::Off &&
        options.metrics_level != MetricsLevel::Basic &&
        options.metrics_level != MetricsLevel::Detailed) {
        throw std::invalid_argument("SchedulerOptions::metrics_level contains unknown enum value");
    }
}

RuntimeId allocate_runtime_id() {
    static std::atomic<std::uint64_t> global_runtime_sequence{0};
    std::uint64_t current = global_runtime_sequence.load(std::memory_order_relaxed);
    while (true) {
        if (current == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("RuntimeId sequence exhausted");
        }
        if (global_runtime_sequence.compare_exchange_weak(
                current, current + 1, std::memory_order_relaxed)) {
            return RuntimeId{current + 1};
        }
    }
}

}  // namespace

struct ASTRA_NO_EXPORT Scheduler::Impl {
    RuntimeId runtime_id;
    SchedulerOptions options;
    SchedulerCapabilities capabilities;
    // 单字原子状态，保证 status() 线性化读取成对快照，不发生跨维度撕裂（D-160）。
    std::atomic<std::uint16_t> packed_status;

    Impl(RuntimeId id, SchedulerOptions opts, SchedulerCapabilities caps)
        : runtime_id(id),
          options(std::move(opts)),
          capabilities(caps),
          packed_status(pack(SchedulerState::Running, ShutdownMode::None)) {}

    static constexpr std::uint16_t pack(SchedulerState state, ShutdownMode mode) noexcept {
        return static_cast<std::uint16_t>((static_cast<std::uint8_t>(state) << 8) |
                                          static_cast<std::uint8_t>(mode));
    }

    static constexpr SchedulerStatus unpack(std::uint16_t val) noexcept {
        const auto state = static_cast<SchedulerState>((val >> 8) & 0xFF);
        const auto mode = static_cast<ShutdownMode>(val & 0xFF);
        return SchedulerStatus{state, mode};
    }

    SchedulerStatus get_status() const noexcept {
        const std::uint16_t val = packed_status.load(std::memory_order_acquire);
        return unpack(val);
    }
};

Scheduler::Scheduler(SchedulerOptions options) {
    validate_options(options);
    const RuntimeId id = allocate_runtime_id();
    const SchedulerCapabilities caps{LocalDequeBackend::None};
    impl_ = std::make_shared<Impl>(id, std::move(options), caps);
}

Scheduler::~Scheduler() = default;

Scheduler::Scheduler(const Scheduler&) = default;
Scheduler& Scheduler::operator=(const Scheduler&) = default;

Scheduler::Scheduler(Scheduler&&) noexcept = default;
Scheduler& Scheduler::operator=(Scheduler&&) noexcept = default;

bool Scheduler::valid() const noexcept {
    return static_cast<bool>(impl_);
}

RuntimeId Scheduler::runtime_id() const noexcept {
    return impl_ ? impl_->runtime_id : RuntimeId{};
}

SchedulerStatus Scheduler::status() const {
    if (!impl_) {
        throw std::logic_error("operating on empty/moved-from Scheduler");
    }
    return impl_->get_status();
}

SchedulerCapabilities Scheduler::capabilities() const {
    if (!impl_) {
        throw std::logic_error("operating on empty/moved-from Scheduler");
    }
    return impl_->capabilities;
}

}  // namespace astra

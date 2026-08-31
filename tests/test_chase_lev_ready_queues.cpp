#include <astra/capabilities.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>

#include "runtime/admission_controller.hpp"
#include "runtime/ready_queues.hpp"
#include "runtime/runtime_metrics.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#define TEST_ASSERT(cond)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(                                                     \
                stderr, "Assertion failed: %s at %s:%d\n", #cond, __FILE__, \
                __LINE__);                                                    \
            std::abort();                                                     \
        }                                                                     \
    } while (false)

namespace {

constexpr bool kExpectedLocalDequeLockFree =
    std::atomic<std::int64_t>::is_always_lock_free &&
    std::atomic<void*>::is_always_lock_free;
constexpr astra::LocalDequeBackend kExpectedLocalDequeBackend =
    kExpectedLocalDequeLockFree
        ? astra::LocalDequeBackend::ChaseLevLockFree
        : astra::LocalDequeBackend::Locked;

class TestInvoker : public astra::detail::TaskInvokerBase {
public:
    explicit TestInvoker(int identity) : identity_(identity) {}

    void execute() override {}
    void cancel_pre_start() noexcept override { cancelled_ = true; }
    [[nodiscard]] int identity() const noexcept { return identity_; }
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }

private:
    int identity_;
    bool cancelled_{false};
};

class ResumeInvoker final : public TestInvoker {
public:
    using TestInvoker::TestInvoker;
    [[nodiscard]] bool is_resume_segment() const noexcept override { return true; }
};

void test_preferred_backend_matches_invoker_cell_atomics() {
    const auto expected =
        astra::detail::ChaseLevDeque<astra::detail::TaskInvokerBase*>::is_lock_free()
            ? astra::LocalDequeBackend::ChaseLevLockFree
            : astra::LocalDequeBackend::Locked;
    TEST_ASSERT(astra::detail::ReadyQueues::preferred_local_backend() == expected);
}

void test_capability_matches_production_backend() {
    astra::Scheduler scheduler;
    const auto capabilities = scheduler.capabilities();
    TEST_ASSERT(capabilities.local_deque_backend() == kExpectedLocalDequeBackend);
    TEST_ASSERT(
        capabilities.lock_free_local_deque() == kExpectedLocalDequeLockFree);
    scheduler.shutdown();
    TEST_ASSERT(scheduler.capabilities() == capabilities);
}

void test_owner_lifo_and_growth_execute_each_task_once() {
    astra::SchedulerOptions options{};
    options.worker_count = 1;
    astra::Scheduler scheduler(options);

    constexpr int kTaskCount = 256;
    std::vector<int> execution_order;
    execution_order.reserve(kTaskCount);

    auto root = scheduler.submit([&] {
        for (int value = 0; value < kTaskCount; ++value) {
            (void)scheduler.submit([&, value] {
                execution_order.push_back(value);
            });
        }
    });

    root.wait();
    scheduler.shutdown();

    TEST_ASSERT(execution_order.size() == static_cast<std::size_t>(kTaskCount));
    for (int index = 0; index < kTaskCount; ++index) {
        TEST_ASSERT(execution_order[static_cast<std::size_t>(index)] ==
                    kTaskCount - index - 1);
    }
}

void test_thief_claims_owner_local_work() {
    astra::SchedulerOptions options{};
    options.worker_count = 2;
    astra::Scheduler scheduler(options);

    constexpr int kTaskCount = 256;
    std::thread::id owner_thread;
    std::atomic<int> executed{0};
    std::atomic<int> stolen{0};

    auto root = scheduler.submit([&] {
        owner_thread = std::this_thread::get_id();
        for (int index = 0; index < kTaskCount; ++index) {
            (void)scheduler.submit([&] {
                if (std::this_thread::get_id() != owner_thread) {
                    stolen.fetch_add(1, std::memory_order_relaxed);
                }
                executed.fetch_add(1, std::memory_order_relaxed);
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });

    root.wait();
    scheduler.shutdown();

    TEST_ASSERT(executed.load(std::memory_order_relaxed) == kTaskCount);
    TEST_ASSERT(stolen.load(std::memory_order_relaxed) > 0);
}

void test_growth_failure_falls_back_to_global_without_loss() {
    if (!kExpectedLocalDequeLockFree) {
        return;
    }

    astra::detail::RuntimeMetrics metrics;
    metrics.init(astra::MetricsLevel::Off, 1, astra::RuntimeId{1});
    astra::detail::ReadyQueues queues(
        1, metrics, astra::LocalDequeBackend::ChaseLevLockFree);

    constexpr int kInitialUsableCapacity = 63;
    for (int identity = 0; identity < kInitialUsableCapacity; ++identity) {
        queues.publish(std::make_unique<TestInvoker>(identity), false, true, 0);
    }

    queues.set_local_growth_failure_for_testing(0, true);
    queues.publish(
        std::make_unique<TestInvoker>(kInitialUsableCapacity), false, true, 0);
    TEST_ASSERT(queues.global_size() == 1);

    std::vector<bool> claimed(kInitialUsableCapacity + 1, false);
    astra::detail::ReadyQueues::QueuedTask task;
    std::size_t global_calendar = 0;
    std::array<std::size_t, 4> deadline_bursts{0, 0, 0, 0};
    TEST_ASSERT(queues.claim_global(global_calendar, deadline_bursts, task));
    auto* fallback = dynamic_cast<TestInvoker*>(task.invoker.get());
    TEST_ASSERT(fallback != nullptr);
    claimed[static_cast<std::size_t>(fallback->identity())] = true;
    queues.complete_claim();

    std::size_t local_calendar = 0;
    while (queues.claim_local(0, local_calendar, task)) {
        auto* local = dynamic_cast<TestInvoker*>(task.invoker.get());
        TEST_ASSERT(local != nullptr);
        const auto identity = static_cast<std::size_t>(local->identity());
        TEST_ASSERT(identity < claimed.size());
        TEST_ASSERT(!claimed[identity]);
        claimed[identity] = true;
        queues.complete_claim();
    }

    for (bool was_claimed : claimed) {
        TEST_ASSERT(was_claimed);
    }
}

void test_immediate_cleanup_keeps_resume_on_local() {
    if (!kExpectedLocalDequeLockFree) {
        return;
    }

    astra::detail::RuntimeMetrics metrics;
    metrics.init(astra::MetricsLevel::Off, 1, astra::RuntimeId{1});
    astra::detail::ReadyQueues queues(
        1, metrics, astra::LocalDequeBackend::ChaseLevLockFree);
    std::atomic<std::uint16_t> packed_status{0};
    astra::detail::AdmissionController admission(
        8, astra::ExternalBackpressure::Block, packed_status, metrics);

    queues.publish(std::make_unique<ResumeInvoker>(1), false, true, 0);
    queues.publish(std::make_unique<TestInvoker>(2), false, true, 0);
    queues.cancel_unstarted(admission);

    TEST_ASSERT(queues.global_empty());
    astra::detail::ReadyQueues::QueuedTask task;
    std::size_t local_calendar = 0;
    TEST_ASSERT(queues.claim_local(0, local_calendar, task));
    auto* resume = dynamic_cast<ResumeInvoker*>(task.invoker.get());
    TEST_ASSERT(resume != nullptr);
    TEST_ASSERT(resume->identity() == 1);
    TEST_ASSERT(!resume->cancelled());
    queues.complete_claim();
    TEST_ASSERT(!queues.claim_local(0, local_calendar, task));
}

}  // namespace

int main() {
    std::printf("Running astra_chase_lev_ready_queues_test...\n");
    test_preferred_backend_matches_invoker_cell_atomics();
    test_capability_matches_production_backend();
    test_owner_lifo_and_growth_execute_each_task_once();
    test_thief_claims_owner_local_work();
    test_growth_failure_falls_back_to_global_without_loss();
    test_immediate_cleanup_keeps_resume_on_local();
    std::printf("All production Chase-Lev ReadyQueues tests passed!\n");
    return 0;
}

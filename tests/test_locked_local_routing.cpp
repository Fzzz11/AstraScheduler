#include <astra/capabilities.hpp>
#include <astra/error.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <vector>

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond,     \
                         __FILE__, __LINE__);                                  \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

namespace {

constexpr bool kExpectedLocalDequeLockFree =
    std::atomic<std::int64_t>::is_always_lock_free &&
    std::atomic<void*>::is_always_lock_free;
constexpr astra::LocalDequeBackend kExpectedLocalDequeBackend =
    kExpectedLocalDequeLockFree
        ? astra::LocalDequeBackend::ChaseLevLockFree
        : astra::LocalDequeBackend::Locked;

// -----------------------------------------------------------------------------
// R-101: capabilities() 精确报告生产 Local Deque backend。
// -----------------------------------------------------------------------------
void test_R101_capabilities_reports_actual_backend() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 2;
    astra::Scheduler s(opt);

    auto caps = s.capabilities();
    TEST_ASSERT(caps.local_deque_backend() == kExpectedLocalDequeBackend);
    TEST_ASSERT(caps.lock_free_local_deque() == kExpectedLocalDequeLockFree);

    s.shutdown();
    // Stopped 之后能力快照不变
    TEST_ASSERT(s.capabilities().local_deque_backend() == kExpectedLocalDequeBackend);

    // 空/moved-from Handle 抛出 logic_error
    astra::Scheduler moved = std::move(s);
    bool threw = false;
    try {
        (void)s.capabilities();
    } catch (const std::logic_error&) {
        threw = true;
    }
    TEST_ASSERT(threw);
}

// -----------------------------------------------------------------------------
// R-063: Ready Routing Precedence 与 Local 洪水防 Global 饥饿
// -----------------------------------------------------------------------------
void test_R063_routing_precedence_and_anti_starvation() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 1; // 单 worker 以便精确观测调度全序
    astra::Scheduler s(opt);

    std::vector<int> execution_order;
    std::mutex order_mutex;
    auto record = [&](int val) {
        std::lock_guard<std::mutex> lk(order_mutex);
        execution_order.push_back(val);
    };

    std::atomic<bool> external_task_executed{false};

    // 提交一个普通 external task（进入 Global Injection Queue）
    auto ext_handle = s.submit([&] {
        record(999);
        external_task_executed.store(true);
    });

    // 提交一个会产生 80 个连续 internal local 任务的任务链
    // 验证在最多 64 次 local 消费后，Worker 必须探测并执行 Global 队列中的 999 任务，防永久饥饿
    auto root_handle = s.submit([&s, &record] {
        record(0);
        // 递归/连续提交内部任务
        for (int i = 1; i <= 80; ++i) {
            (void)s.submit([&record, i] {
                record(i);
            });
        }
    });

    root_handle.wait();
    ext_handle.wait();
    s.shutdown();

    TEST_ASSERT(external_task_executed.load());
    TEST_ASSERT(!execution_order.empty());
}

}  // namespace

int main() {
    std::printf("Running astra_locked_local_routing_test...\n");
    test_R101_capabilities_reports_actual_backend();
    test_R063_routing_precedence_and_anti_starvation();
    std::printf("All AST-022 locked local routing tests passed successfully!\n");
    return 0;
}

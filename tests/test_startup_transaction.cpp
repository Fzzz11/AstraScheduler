#include <astra/capabilities.hpp>
#include <astra/error.hpp>
#include <astra/id.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>
#include <astra/status.hpp>
#include "reaper_registry.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

// AST-005 测试套件：Scheduler startup transaction 与 Finalization gate 排序
// 覆盖 primary 规则：
// - R-023: Reaper handoff 能力先于 Worker 启动；准备失败时回滚且无活动 Worker
// - R-024: 运行期 handoff 不获取可失败资源，noexcept 且不分配内存
// - R-097: Scheduler 构造为同步强事务，与 Finalization close 唯一全序

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond,     \
                         __FILE__, __LINE__);                                  \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

#define TEST_THROWS(expr, ExceptionType)                                       \
    do {                                                                       \
        bool caught = false;                                                   \
        try {                                                                  \
            (void)(expr);                                                      \
        } catch (const ExceptionType&) {                                       \
            caught = true;                                                     \
        } catch (...) {                                                        \
        }                                                                      \
        if (!caught) {                                                         \
            std::fprintf(stderr,                                               \
                         "Expected exception %s not thrown: %s at %s:%d\n",    \
                         #ExceptionType, #expr, __FILE__, __LINE__);           \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

// -----------------------------------------------------------------------------
// R-097 编译期契约：scheduler_creation_rejected 异常类型
// -----------------------------------------------------------------------------
static_assert(std::is_base_of_v<std::runtime_error, astra::scheduler_creation_rejected>,
              "scheduler_creation_rejected must inherit from std::runtime_error");
static_assert(noexcept(std::declval<astra::scheduler_creation_rejected>().reason()),
              "scheduler_creation_rejected::reason() must be noexcept");

void test_R097_scheduler_creation_rejected_type() {
    astra::detail::ReaperRegistry::instance().reset_for_testing();
    astra::scheduler_creation_rejected ex(astra::SchedulerCreationError::FinalizationStarted);
    TEST_ASSERT(ex.reason() == astra::SchedulerCreationError::FinalizationStarted);
    std::string_view msg = ex.what();
    TEST_ASSERT(!msg.empty());
    TEST_ASSERT(msg.find("Finalization") != std::string_view::npos);
}

// -----------------------------------------------------------------------------
// R-097: 同步强事务正常启动与多 Worker 配置
// -----------------------------------------------------------------------------
void test_R097_startup_transaction_success() {
    astra::detail::ReaperRegistry::instance().reset_for_testing();
    {
        astra::SchedulerOptions opt{};
        opt.worker_count = 1;
        astra::Scheduler scheduler(opt);
        TEST_ASSERT(scheduler.valid());
        TEST_ASSERT(scheduler.runtime_id().valid());
        const astra::SchedulerStatus st = scheduler.status();
        TEST_ASSERT(st.state == astra::SchedulerState::Running);
        TEST_ASSERT(st.shutdown_mode == astra::ShutdownMode::None);
        TEST_ASSERT(astra::detail::ReaperRegistry::instance().registered_count() == 1);
    }
    // 析构后注销
    TEST_ASSERT(astra::detail::ReaperRegistry::instance().registered_count() == 0);

    {
        astra::SchedulerOptions opt{};
        opt.worker_count = 4;
        astra::Scheduler scheduler(opt);
        TEST_ASSERT(scheduler.valid());
        TEST_ASSERT(astra::detail::ReaperRegistry::instance().registered_count() == 1);
    }
    TEST_ASSERT(astra::detail::ReaperRegistry::instance().registered_count() == 0);
}

// -----------------------------------------------------------------------------
// R-023 / R-097: 非法配置在无任何 Worker 或 Runtime 副作用前失败
// -----------------------------------------------------------------------------
void test_R023_R097_invalid_options_zero_side_effects() {
    astra::detail::ReaperRegistry::instance().reset_for_testing();
    astra::SchedulerOptions opt{};
    opt.worker_count = 0;
    TEST_THROWS(astra::Scheduler(opt), std::invalid_argument);
    TEST_ASSERT(astra::detail::ReaperRegistry::instance().registered_count() == 0);

    opt.worker_count = 4;
    opt.external_pending_capacity = 0;
    TEST_THROWS(astra::Scheduler(opt), std::invalid_argument);
    TEST_ASSERT(astra::detail::ReaperRegistry::instance().registered_count() == 0);
}

// -----------------------------------------------------------------------------
// R-023: Reaper handoff 能力预留失败时，回滚启动，不创建 Worker 且不发布 Running
// -----------------------------------------------------------------------------
void test_R023_reaper_handoff_setup_before_workers_failure_rollback() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();
    registry.inject_handoff_reservation_failure(true);

    astra::SchedulerOptions opt{};
    opt.worker_count = 4;

    TEST_THROWS(astra::Scheduler(opt), std::bad_alloc);
    // 保证无残留注册，无活跃 Worker
    TEST_ASSERT(registry.registered_count() == 0);

    registry.inject_handoff_reservation_failure(false);
    // 恢复后可正常构造
    {
        astra::Scheduler scheduler(opt);
        TEST_ASSERT(scheduler.valid());
        TEST_ASSERT(registry.registered_count() == 1);
    }
    TEST_ASSERT(registry.registered_count() == 0);
}

// -----------------------------------------------------------------------------
// R-023: Worker 线程创建中断失败时，完整回滚并 join 已启动的 Worker
// -----------------------------------------------------------------------------
void test_R023_worker_thread_creation_failure_rollback() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();
    
    // 模拟在创建第 2 个 Worker 时抛出系统资源耗尽异常
    registry.inject_worker_creation_failure_at(2);

    astra::SchedulerOptions opt{};
    opt.worker_count = 4;

    TEST_THROWS(astra::Scheduler(opt), std::system_error);
    // 事务回滚保证：第 1 个 worker 被正确 join，Reaper 注册已撤销
    TEST_ASSERT(registry.registered_count() == 0);

    registry.inject_worker_creation_failure_at(0);
    // 恢复正常
    {
        astra::Scheduler scheduler(opt);
        TEST_ASSERT(scheduler.valid());
        TEST_ASSERT(registry.registered_count() == 1);
    }
    TEST_ASSERT(registry.registered_count() == 0);
}

// -----------------------------------------------------------------------------
// R-024: 运行期 handoff 能力插槽已预留，无分配无线程创建
// -----------------------------------------------------------------------------
void test_R024_handoff_pre_reserved_slot() {
    astra::detail::HandoffCapabilitySlot slot;
    slot.runtime_id = astra::RuntimeId{42};
    TEST_ASSERT(!slot.handoff_executed.load());
    TEST_ASSERT(!slot.join_ready.load());
    // 运行期操作是原子修改，noexcept
    static_assert(noexcept(slot.handoff_executed.store(true)));
    static_assert(noexcept(slot.join_ready.store(true)));
}

// -----------------------------------------------------------------------------
// R-097: Finalization 已经启动时，拒绝创建 Scheduler 并抛出 scheduler_creation_rejected
// -----------------------------------------------------------------------------
void test_R097_finalization_closed_rejection() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();
    registry.close_registration();
    TEST_ASSERT(!registry.is_registration_open());

    astra::SchedulerOptions opt{};
    opt.worker_count = 4;

    bool rejected = false;
    try {
        astra::Scheduler s(opt);
    } catch (const astra::scheduler_creation_rejected& e) {
        rejected = true;
        TEST_ASSERT(e.reason() == astra::SchedulerCreationError::FinalizationStarted);
    }
    TEST_ASSERT(rejected);
    TEST_ASSERT(registry.registered_count() == 0);

    registry.reset_for_testing();
}

// -----------------------------------------------------------------------------
// R-097: Running publication 与 Finalization close 启动竞态测试（唯一全序）
// -----------------------------------------------------------------------------
void test_R097_finalization_race_during_startup() {
    for (int iter = 0; iter < 50; ++iter) {
        auto& registry = astra::detail::ReaperRegistry::instance();
        registry.reset_for_testing();

        std::atomic<bool> start_flag{false};
        std::atomic<int> success_count{0};
        std::atomic<int> rejected_count{0};

        std::thread creator([&] {
            while (!start_flag.load(std::memory_order_acquire)) {}
            try {
                astra::SchedulerOptions opt{};
                opt.worker_count = 2;
                astra::Scheduler s(opt);
                if (s.valid() && (s.status().state == astra::SchedulerState::Running ||
                                  s.status().state == astra::SchedulerState::Stopping ||
                                  s.status().state == astra::SchedulerState::Stopped)) {
                    success_count.fetch_add(1);
                }
            } catch (const astra::scheduler_creation_rejected& e) {
                if (e.reason() == astra::SchedulerCreationError::FinalizationStarted) {
                    rejected_count.fetch_add(1);
                }
            }
        });

        std::thread finalizer([&] {
            while (!start_flag.load(std::memory_order_acquire)) {}
            registry.close_registration();
        });

        start_flag.store(true, std::memory_order_release);
        creator.join();
        finalizer.join();

        // 结果只能是二者之一（成功发布 Running 纳入核算，或者拒绝回滚）
        TEST_ASSERT(success_count.load() + rejected_count.load() == 1);
    }
    astra::detail::ReaperRegistry::instance().reset_for_testing();
}

int main() {
    std::printf("Running astra_startup_transaction_test...\n");
    test_R097_scheduler_creation_rejected_type();
    test_R097_startup_transaction_success();
    test_R023_R097_invalid_options_zero_side_effects();
    test_R023_reaper_handoff_setup_before_workers_failure_rollback();
    test_R023_worker_thread_creation_failure_rollback();
    test_R024_handoff_pre_reserved_slot();
    test_R097_finalization_closed_rejection();
    test_R097_finalization_race_during_startup();
    std::printf("All AST-005 startup transaction tests passed successfully!\n");
    return 0;
}

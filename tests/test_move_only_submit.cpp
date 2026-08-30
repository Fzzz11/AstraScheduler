#include <astra/error.hpp>
#include <astra/id.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>
#include <astra/status.hpp>
#include <astra/task_handle.hpp>
#include "lifecycle/reaper_registry.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

// AST-009 测试套件：实现 move-only submit 与共享 TaskHandle 基础面
// 覆盖 primary 规则：
// - R-048: TaskHandle 是共享任务 capability（可复制/移动、关联同一 TaskId、Handle 丢弃不取消任务）
// - R-058: submit 结果类型与基础结果 API 受限（void/可移动对象/move-only、拒绝裸引用/immovable、lvalue get() const &）
// - R-102: submit decay-own 并一次性 rvalue 调用 move-only 工作（operator()&&、move-only 参数、std::ref、stop_token fallback）

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond,     \
                         __FILE__, __LINE__);                                  \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

namespace {

// -----------------------------------------------------------------------------
// R-048: 共享 TaskHandle、同一 TaskId 与丢弃 Handle 后任务仍能完成
// -----------------------------------------------------------------------------
void test_R048_shared_task_handle_and_lifetime() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();

    astra::SchedulerOptions opt{};
    opt.worker_count = 2;
    astra::Scheduler s(opt);

    std::atomic<int> execution_count{0};

    // 1. 提交任务并复制 TaskHandle
    auto h1 = s.submit([&] {
        execution_count.fetch_add(1);
        return 42;
    });

    TEST_ASSERT(h1.valid());
    const auto tid = h1.task_id();
    TEST_ASSERT(tid != astra::TaskId{});

    // 复制 Handle：共享关联同一 TaskId
    auto h2 = h1;
    auto h3 = h2;
    TEST_ASSERT(h2.valid());
    TEST_ASSERT(h3.valid());
    TEST_ASSERT(h2.task_id() == tid);
    TEST_ASSERT(h3.task_id() == tid);

    // 多个 Handle 调用 get() 获取同一结果且只执行一次
    TEST_ASSERT(h1.get() == 42);
    TEST_ASSERT(h2.get() == 42);
    TEST_ASSERT(h3.get() == 42);
    TEST_ASSERT(execution_count.load() == 1);

    // 2. 移动 Handle：源 Handle 变为空
    auto h_moved = std::move(h3);
    TEST_ASSERT(h_moved.valid());
    TEST_ASSERT(h_moved.task_id() == tid);
    TEST_ASSERT(!h3.valid());

    bool threw_on_empty = false;
    try {
        (void)h3.task_id();
    } catch (const std::logic_error&) {
        threw_on_empty = true;
    }
    TEST_ASSERT(threw_on_empty);

    threw_on_empty = false;
    try {
        (void)h3.get();
    } catch (const std::logic_error&) {
        threw_on_empty = true;
    }
    TEST_ASSERT(threw_on_empty);

    // 3. 丢弃全部 Handle 后已接受的任务仍能完成
    std::atomic<bool> dropped_task_completed{false};
    {
        auto h_temp = s.submit([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            dropped_task_completed.store(true);
        });
        // h_temp 在作用域结束时析构丢弃
    }

    // 等待任务后台完成
    while (!dropped_task_completed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_ASSERT(dropped_task_completed.load());
}

// -----------------------------------------------------------------------------
// R-058: submit 结果类型与基础结果 API 受限（void/copyable/move-only 与异常传播）
// -----------------------------------------------------------------------------
void test_R058_result_types_and_exceptions() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();

    astra::SchedulerOptions opt{};
    opt.worker_count = 2;
    astra::Scheduler s(opt);

    // 1. TaskHandle<void>
    std::atomic<bool> void_ran{false};
    auto h_void = s.submit([&] {
        void_ran.store(true);
    });
    h_void.get();
    TEST_ASSERT(void_ran.load());

    // 2. TaskHandle<T> (copyable)
    auto h_str = s.submit([] {
        return std::string("hello AstraScheduler");
    });
    TEST_ASSERT(h_str.get() == "hello AstraScheduler");

    // 3. TaskHandle<T> (move-only object result)
    auto h_unique = s.submit([] {
        return std::make_unique<int>(100);
    });
    TEST_ASSERT(h_unique.get() != nullptr);
    TEST_ASSERT(*h_unique.get() == 100);

    // 4. Exception outcome 传播
    auto h_ex = s.submit([]() -> int {
        throw std::runtime_error("task failure simulation");
    });
    bool threw_expected_exception = false;
    try {
        (void)h_ex.get();
    } catch (const std::runtime_error& ex) {
        if (std::string(ex.what()) == "task failure simulation") {
            threw_expected_exception = true;
        }
    }
    TEST_ASSERT(threw_expected_exception);

    // 5. 编译期约束断言：
    // - 裸引用类型必须在编译期被拒绝（D-074）
    static_assert(astra::detail::InvocationTraits<int& (*)()>::returns_reference == true);
    static_assert(astra::detail::InvocationTraits<const int& (*)()>::returns_reference == true);
    static_assert(astra::detail::InvocationTraits<int&& (*)()>::returns_reference == true);

    // - 非引用对象与 void 类型正常接受
    static_assert(astra::detail::InvocationTraits<int (*)()>::returns_reference == false);
    static_assert(astra::detail::InvocationTraits<void (*)()>::returns_reference == false);
}

// -----------------------------------------------------------------------------
// R-102: move-only Callable/参数与一次性 rvalue 调用
// -----------------------------------------------------------------------------
struct MoveOnlyFunctor {
    std::unique_ptr<int> ptr;

    explicit MoveOnlyFunctor(int val) : ptr(std::make_unique<int>(val)) {}
    MoveOnlyFunctor(const MoveOnlyFunctor&) = delete;
    MoveOnlyFunctor& operator=(const MoveOnlyFunctor&) = delete;
    MoveOnlyFunctor(MoveOnlyFunctor&&) noexcept = default;
    MoveOnlyFunctor& operator=(MoveOnlyFunctor&&) noexcept = default;

    int operator()() && {
        return *ptr + 10;
    }
};

void test_R102_move_only_callable_and_arguments() {
    auto& registry = astra::detail::ReaperRegistry::instance();
    registry.reset_for_testing();

    astra::SchedulerOptions opt{};
    opt.worker_count = 2;
    astra::Scheduler s(opt);

    // 1. 提交拥有 operator()() && 的 move-only functor
    MoveOnlyFunctor functor(32);
    auto h1 = s.submit(std::move(functor));
    TEST_ASSERT(h1.get() == 42);

    // 2. 提交带 move-only 参数 (std::unique_ptr) 的 Callable
    auto arg_ptr = std::make_unique<int>(55);
    auto h2 = s.submit([](std::unique_ptr<int> p) {
        return *p * 2;
    }, std::move(arg_ptr));
    TEST_ASSERT(h2.get() == 110);

    // 3. 显式 std::ref 传递引用并在任务中修改
    int external_var = 10;
    auto h3 = s.submit([](int& ref) {
        ref += 5;
    }, std::ref(external_var));
    h3.get();
    TEST_ASSERT(external_var == 15);

    // 4. std::stop_token 自动注入 fallback (D-059)
    std::atomic<bool> token_received{false};
    auto h4 = s.submit([&](std::stop_token st, int x) {
        (void)st;
        token_received.store(true);
        return x + 1;
    }, 99);
    TEST_ASSERT(h4.get() == 100);
    TEST_ASSERT(token_received.load());
}

}  // namespace

int main() {
    std::printf("Running astra_move_only_submit_test...\n");
    test_R048_shared_task_handle_and_lifetime();
    test_R058_result_types_and_exceptions();
    test_R102_move_only_callable_and_arguments();
    std::printf("All AST-009 move-only submit & TaskHandle tests passed successfully!\n");
    return 0;
}

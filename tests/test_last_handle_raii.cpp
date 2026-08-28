#include <astra/error.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
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

// -----------------------------------------------------------------------------
// R-103: 非最后副本销毁不得触发关停，最后 Handle 释放才触发 RAII
// -----------------------------------------------------------------------------
void test_R103_non_last_handle_destruction_does_not_shutdown() {
    astra::Scheduler s1;
    {
        astra::Scheduler s2 = s1;
        {
            astra::Scheduler s3 = s2;
            TEST_ASSERT(s3.valid());
            TEST_ASSERT(s3.status().state == astra::SchedulerState::Running);
        }
        // s3 销毁后，Runtime 依然处于 Running
        TEST_ASSERT(s2.valid());
        TEST_ASSERT(s2.status().state == astra::SchedulerState::Running);
    }
    // s2 销毁后，Runtime 依然处于 Running
    TEST_ASSERT(s1.valid());
    TEST_ASSERT(s1.status().state == astra::SchedulerState::Running);

    auto h = s1.submit([]() { return 100; });
    TEST_ASSERT(h.get() == 100);
}

// -----------------------------------------------------------------------------
// R-103: 空/moved-from Handle 操作除 valid/destruction 外抛 logic_error
// -----------------------------------------------------------------------------
void test_R103_empty_moved_from_throws_logic_error() {
    astra::Scheduler s1;
    astra::Scheduler s2 = std::move(s1);

    TEST_ASSERT(!s1.valid());
    TEST_ASSERT(s2.valid());

    bool caught_status = false;
    try {
        (void)s1.status();
    } catch (const std::logic_error&) {
        caught_status = true;
    }
    TEST_ASSERT(caught_status);

    bool caught_submit = false;
    try {
        (void)s1.submit([]() {});
    } catch (const std::logic_error&) {
        caught_submit = true;
    }
    TEST_ASSERT(caught_submit);
}

// -----------------------------------------------------------------------------
// R-105: 最后一个非 Worker Handle 析构是 noexcept 同步完成边界
// -----------------------------------------------------------------------------
void test_R105_last_non_worker_handle_destructor_is_synchronous_noexcept() {
    static_assert(noexcept(std::declval<astra::Scheduler>().~Scheduler()),
                  "~Scheduler must be declared noexcept");

    std::atomic<bool> task_completed{false};
    {
        astra::Scheduler s;
        (void)s.submit([&task_completed]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            task_completed.store(true);
            // 抛出异常验证析构路径不向外传播异常（noexcept）
            throw std::runtime_error("simulated task failure inside worker");
        });
    } // 最后一个 Handle 离开作用域，RAII 同步等待 drain 闭包与 worker join 完成

    TEST_ASSERT(task_completed.load());
}

}  // namespace

int main() {
    std::printf("Running astra_last_handle_raii_test...\n");
    test_R103_non_last_handle_destruction_does_not_shutdown();
    test_R103_empty_moved_from_throws_logic_error();
    test_R105_last_non_worker_handle_destructor_is_synchronous_noexcept();
    std::printf("All AST-017 last handle RAII tests passed successfully!\n");
    return 0;
}

#include <astra/scheduler.hpp>

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

struct CustomException : public std::runtime_error {
    CustomException() : std::runtime_error("Custom task exception") {}
};

// -----------------------------------------------------------------------------
// R-049 & R-051: Terminal Value Outcome 与 Succeeded TaskState 一致发布
// -----------------------------------------------------------------------------
void test_R049_R051_value_outcome_and_state_consistency() {
    astra::Scheduler s;

    auto h1 = s.submit([]() {
        return std::string("AstraScheduler Outcome");
    });

    auto h2 = h1; // 共享副本
    auto h3 = h1;

    TEST_ASSERT(h1.valid() && h2.valid() && h3.valid());
    TEST_ASSERT(h1.task_id() == h2.task_id());

    // 观察 TaskState 与 get() 结果一致性
    h1.wait();
    TEST_ASSERT(h1.state() == astra::TaskState::Succeeded);
    TEST_ASSERT(h2.state() == astra::TaskState::Succeeded);
    TEST_ASSERT(h3.state() == astra::TaskState::Succeeded);

    // R-051: get() const & 返回共享 const T& 引用
    const std::string& ref1 = h1.get();
    const std::string& ref2 = h2.get();
    const std::string& ref3 = h3.get();

    TEST_ASSERT(ref1 == "AstraScheduler Outcome");
    TEST_ASSERT(&ref1 == &ref2); // 指向同一不可变存储地址
    TEST_ASSERT(&ref2 == &ref3);

    // 重复调用 get() 幂等返回同一引用
    TEST_ASSERT(&h1.get() == &ref1);

    // void 任务
    auto h_void = s.submit([]() {});
    h_void.wait();
    TEST_ASSERT(h_void.state() == astra::TaskState::Succeeded);
    h_void.get(); // 返回 void
}

// -----------------------------------------------------------------------------
// R-050: 异常重复传播与原动态类型重抛
// -----------------------------------------------------------------------------
void test_R050_exception_repeated_propagation() {
    astra::Scheduler s;

    auto h1 = s.submit([]() -> int {
        throw CustomException();
    });

    auto h2 = h1;

    h1.wait();
    TEST_ASSERT(h1.state() == astra::TaskState::Failed);
    TEST_ASSERT(h2.state() == astra::TaskState::Failed);

    // 多次 get() 均按原动态类型抛出 CustomException
    int caught_count = 0;
    for (int i = 0; i < 3; ++i) {
        try {
            (void)h1.get();
        } catch (const CustomException& ex) {
            ++caught_count;
            TEST_ASSERT(std::string(ex.what()) == "Custom task exception");
        }
    }
    TEST_ASSERT(caught_count == 3);

    // 副本 h2 get() 同样抛出
    try {
        (void)h2.get();
        TEST_ASSERT(false && "Should have thrown CustomException");
    } catch (const CustomException&) {
        // Expected
    }

    // 状态依旧保持 Failed
    TEST_ASSERT(h1.state() == astra::TaskState::Failed);
}

// -----------------------------------------------------------------------------
// R-050 & R-054: task_cancelled 逃出执行边界转为 Cancelled Outcome 并重复抛出
// -----------------------------------------------------------------------------
void test_R050_R054_task_cancelled_propagation() {
    astra::Scheduler s;

    // 1. 普通 Callable 主动抛出 task_cancelled
    auto h1 = s.submit([]() {
        throw astra::task_cancelled{};
    });

    h1.wait();
    TEST_ASSERT(h1.state() == astra::TaskState::Cancelled);

    int cancel_caught = 0;
    for (int i = 0; i < 3; ++i) {
        try {
            h1.get();
        } catch (const astra::task_cancelled& ex) {
            ++cancel_caught;
            TEST_ASSERT(std::string(ex.what()) == "AstraScheduler task was cancelled");
        }
    }
    TEST_ASSERT(cancel_caught == 3);

    // 2. 使用 throw_if_stop_requested(token) 辅助函数协作取消
    std::promise<void> start_promise;
    std::shared_future<void> started = start_promise.get_future().share();

    auto h2 = s.submit([&start_promise, started](std::stop_token token) {
        start_promise.set_value();
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        astra::throw_if_stop_requested(token);
    });

    started.wait();
    TEST_ASSERT(h2.state() == astra::TaskState::Running);

    h2.request_cancel();
    h2.wait();
    TEST_ASSERT(h2.state() == astra::TaskState::Cancelled);

    bool threw_cancelled = false;
    try {
        h2.get();
    } catch (const astra::task_cancelled&) {
        threw_cancelled = true;
    }
    TEST_ASSERT(threw_cancelled);
}

// -----------------------------------------------------------------------------
// R-055 & R-056: wait 与 wait_for 观察行为
// -----------------------------------------------------------------------------
void test_R055_R056_wait_and_wait_for() {
    astra::Scheduler s;

    std::promise<void> release_promise;
    std::shared_future<void> release = release_promise.get_future().share();

    auto h = s.submit([release]() {
        release.wait();
        return 999;
    });

    // 1. 有界等待 wait_for 超时返回 TimedOut
    auto wait_res = h.wait_for(std::chrono::milliseconds(20));
    TEST_ASSERT(wait_res == astra::WaitResult::TimedOut);
    TEST_ASSERT(h.state() == astra::TaskState::Running || h.state() == astra::TaskState::Ready);

    // 2. 负数或零 duration 即时观察
    auto instant_res = h.wait_for(std::chrono::milliseconds(-10));
    TEST_ASSERT(instant_res == astra::WaitResult::TimedOut);

    // 3. 释放任务
    release_promise.set_value();

    // 4. wait() 阻塞直到完成
    h.wait();
    TEST_ASSERT(h.state() == astra::TaskState::Succeeded);
    TEST_ASSERT(h.wait_for(std::chrono::milliseconds(0)) == astra::WaitResult::Completed);
    TEST_ASSERT(h.get() == 999);
}

// -----------------------------------------------------------------------------
// R-057: TaskHandle 空状态契约与生命周期快照
// -----------------------------------------------------------------------------
void test_R057_empty_handle_contract() {
    astra::TaskHandle<int> empty_h;
    TEST_ASSERT(!empty_h.valid());

    // 空对象调用 task_id/state/get/wait/wait_for 必须抛 std::logic_error
    auto assert_logic_error = [](auto&& fn) {
        bool thrown = false;
        try {
            fn();
        } catch (const std::logic_error&) {
            thrown = true;
        }
        TEST_ASSERT(thrown);
    };

    assert_logic_error([&]() { (void)empty_h.task_id(); });
    assert_logic_error([&]() { (void)empty_h.state(); });
    assert_logic_error([&]() { (void)empty_h.get(); });
    assert_logic_error([&]() { empty_h.wait(); });
    assert_logic_error([&]() { (void)empty_h.wait_for(std::chrono::milliseconds(1)); });

    // request_cancel 为安全 no-op
    empty_h.request_cancel();

    // moved-from 为空
    astra::Scheduler s;
    auto valid_h = s.submit([]() { return 1; });
    TEST_ASSERT(valid_h.valid());

    astra::TaskHandle<int> target = std::move(valid_h);
    TEST_ASSERT(!valid_h.valid());
    TEST_ASSERT(target.valid());
    assert_logic_error([&]() { (void)valid_h.state(); });
    TEST_ASSERT(target.get() == 1);
}

// -----------------------------------------------------------------------------
// R-057 & R-049: 多线程并发观察同一 TaskHandle 副本
// -----------------------------------------------------------------------------
void test_R057_concurrent_observers() {
    astra::Scheduler s;

    auto h = s.submit([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 12345;
    });

    constexpr int kNumThreads = 8;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back([h, &success_count]() {
            h.wait();
            TEST_ASSERT(h.state() == astra::TaskState::Succeeded);
            if (h.get() == 12345) {
                success_count.fetch_add(1);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }
    TEST_ASSERT(success_count.load() == kNumThreads);
}

// -----------------------------------------------------------------------------
// R-060: 未观察异常析构不抛出、不调用终止处理
// -----------------------------------------------------------------------------
void test_R060_unobserved_exception_safe_destruction() {
    astra::Scheduler s;

    {
        // 提交一个必定失败的任务，但在完成前后丢弃所有 TaskHandle，不调用 get()
        auto h = s.submit([]() -> int {
            throw std::runtime_error("unobserved error");
        });
        h.wait();
        TEST_ASSERT(h.state() == astra::TaskState::Failed);
    } // h 在此处析构

    // 运行至此未发生 std::terminate，通过
    TEST_ASSERT(true);
}

}  // namespace

int main() {
    std::printf("Running astra_task_outcome_state_test...\n");
    test_R049_R051_value_outcome_and_state_consistency();
    test_R050_exception_repeated_propagation();
    test_R050_R054_task_cancelled_propagation();
    test_R055_R056_wait_and_wait_for();
    test_R057_empty_handle_contract();
    test_R057_concurrent_observers();
    test_R060_unobserved_exception_safe_destruction();
    std::printf("All AST-011 task outcome and state tests passed successfully!\n");
    return 0;
}

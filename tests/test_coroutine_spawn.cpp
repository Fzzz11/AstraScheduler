#include <astra/coroutine.hpp>
#include <astra/error.hpp>
#include <astra/scheduler.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

struct FrameTracker {
    static inline std::atomic<int> alive_count{0};
    static inline std::atomic<int> construct_count{0};
    static inline std::atomic<int> destruct_count{0};

    static void reset() {
        alive_count.store(0);
        construct_count.store(0);
        destruct_count.store(0);
    }

    FrameTracker() {
        construct_count.fetch_add(1);
        alive_count.fetch_add(1);
    }

    ~FrameTracker() {
        destruct_count.fetch_add(1);
        alive_count.fetch_sub(1);
    }
};

// 辅助协程：返回值
astra::Task<int> make_coro_value(std::atomic<bool>& body_started, int val) {
    FrameTracker tracker;
    body_started.store(true);
    co_return val;
}

// 辅助协程：void
astra::Task<void> make_coro_void(std::atomic<bool>& body_started) {
    FrameTracker tracker;
    body_started.store(true);
    co_return;
}

// 辅助协程：抛异常
astra::Task<int> make_coro_throw(std::atomic<bool>& body_started) {
    FrameTracker tracker;
    body_started.store(true);
    throw std::runtime_error("coroutine error");
    co_return 0;
}

// 1. cold-before-spawn 验证（R-073 / D-114）
void test_R073_cold_before_spawn() {
    FrameTracker::reset();
    std::atomic<bool> body_started{false};

    {
        // 构造协程对象，但不 spawn
        auto task = make_coro_value(body_started, 42);
        TEST_ASSERT(task.valid());
        TEST_ASSERT(static_cast<bool>(task));
        // cold 语义：函数体尚未执行，局部变量未构造（或在 initial_suspend 挂起）
        TEST_ASSERT(!body_started.load());
    }
    // task 析构时自动 destroy 挂起的 frame
    TEST_ASSERT(!body_started.load());
    TEST_ASSERT(FrameTracker::alive_count.load() == 0);
}

// 2. move-only 与 empty Task 验证（R-073 / D-114）
void test_R073_move_only_and_empty_task() {
    std::atomic<bool> body_started{false};

    astra::Task<int> empty_task;
    TEST_ASSERT(!empty_task.valid());
    TEST_ASSERT(!empty_task);

    auto t1 = make_coro_value(body_started, 100);
    TEST_ASSERT(t1.valid());

    auto t2 = std::move(t1);
    TEST_ASSERT(!t1.valid());
    TEST_ASSERT(t2.valid());

    // 赋值给自身或移动赋值
    t1 = std::move(t2);
    TEST_ASSERT(t1.valid());
    TEST_ASSERT(!t2.valid());
}

// 3. 空 Task spawn / try_spawn 抛 logic_error（R-073 / D-115）
void test_R073_spawn_empty_task_throws() {
    astra::Scheduler scheduler;
    astra::Task<int> empty_task;

    bool threw = false;
    try {
        (void)scheduler.spawn(std::move(empty_task));
    } catch (const std::logic_error&) {
        threw = true;
    }
    TEST_ASSERT(threw);

    threw = false;
    try {
        (void)scheduler.try_spawn(std::move(empty_task));
    } catch (const std::logic_error&) {
        threw = true;
    }
    TEST_ASSERT(threw);
}

// 4. 成功 spawn 转移所有权并执行（R-073 / D-114 / D-115）
void test_R073_successful_spawn_and_value_return() {
    FrameTracker::reset();
    astra::Scheduler scheduler;
    std::atomic<bool> body_started{false};

    auto task = make_coro_value(body_started, 12345);
    TEST_ASSERT(task.valid());

    auto handle = scheduler.spawn(std::move(task));
    // 成功 spawn 后源 Task 必须被清空
    TEST_ASSERT(!task.valid());
    TEST_ASSERT(handle.valid());

    int val = handle.get();
    TEST_ASSERT(val == 12345);
    TEST_ASSERT(body_started.load());
    TEST_ASSERT(handle.state() == astra::TaskState::Succeeded);
    TEST_ASSERT(FrameTracker::alive_count.load() == 0);
}

// 5. void 协程 spawn 验证
void test_R073_successful_spawn_void() {
    FrameTracker::reset();
    astra::Scheduler scheduler;
    std::atomic<bool> body_started{false};

    auto task = make_coro_void(body_started);
    auto handle = scheduler.spawn(std::move(task));
    TEST_ASSERT(!task.valid());

    handle.get();
    TEST_ASSERT(body_started.load());
    TEST_ASSERT(handle.state() == astra::TaskState::Succeeded);
    TEST_ASSERT(FrameTracker::alive_count.load() == 0);
}

// 6. 协程内抛异常传播至 TaskHandle（R-073 / D-114 / D-115）
void test_R073_coroutine_exception_propagation() {
    FrameTracker::reset();
    astra::Scheduler scheduler;
    std::atomic<bool> body_started{false};

    auto task = make_coro_throw(body_started);
    auto handle = scheduler.spawn(std::move(task));
    TEST_ASSERT(!task.valid());

    bool threw = false;
    try {
        handle.get();
    } catch (const std::runtime_error& ex) {
        threw = true;
        TEST_ASSERT(std::string(ex.what()) == "coroutine error");
    }
    TEST_ASSERT(threw);
    TEST_ASSERT(handle.state() == astra::TaskState::Failed);
    TEST_ASSERT(FrameTracker::alive_count.load() == 0);
}

// 7. rejection 强异常安全：admission 拒绝后源 Task 保持有效可重试或析构（R-073 / D-115）
void test_R073_admission_rejection_retains_task_ownership() {
    FrameTracker::reset();
    std::atomic<bool> body_started{false};

    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    opts.external_pending_capacity = 1;
    astra::Scheduler scheduler(opts);

    // 填满容量：提交一个阻塞任务
    std::atomic<bool> blocker_running{false};
    std::atomic<bool> can_finish{false};
    auto blocker = scheduler.submit([&] {
        blocker_running.store(true);
        while (!can_finish.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    while (!blocker_running.load()) {
        std::this_thread::yield();
    }

    // 占用 external pending slot
    auto pending_task = scheduler.submit([&] {});

    // 现在 external pending capacity 耗尽，调用 try_spawn 必须被拒绝
    auto coro_task = make_coro_value(body_started, 999);
    TEST_ASSERT(coro_task.valid());

    auto result = scheduler.try_spawn(std::move(coro_task));
    TEST_ASSERT(std::holds_alternative<astra::SubmissionError>(result));
    TEST_ASSERT(std::get<astra::SubmissionError>(result) == astra::SubmissionError::CapacityExhausted);

    // 关键强保证（D-115）：被拒绝后，coro_task 依然有效且拥有原 frame，body 未执行！
    TEST_ASSERT(coro_task.valid());
    TEST_ASSERT(!body_started.load());

    // 释放 blocker
    can_finish.store(true);
    blocker.wait();
    pending_task.wait();

    // 再次重试 spawn 应当成功！
    auto handle = scheduler.spawn(std::move(coro_task));
    TEST_ASSERT(!coro_task.valid());
    TEST_ASSERT(handle.get() == 999);
    TEST_ASSERT(body_started.load());
}

}  // namespace

int main() {
    std::printf("Running astra_coroutine_spawn_test...\n");
    test_R073_cold_before_spawn();
    test_R073_move_only_and_empty_task();
    test_R073_spawn_empty_task_throws();
    test_R073_successful_spawn_and_value_return();
    test_R073_successful_spawn_void();
    test_R073_coroutine_exception_propagation();
    test_R073_admission_rejection_retains_task_ownership();
    std::printf("All AST-032 Coroutine spawn tests passed successfully!\n");
    return 0;
}

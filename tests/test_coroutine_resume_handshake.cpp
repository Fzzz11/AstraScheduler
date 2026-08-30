#include "task/await_handshake.hpp"
#include "task/coroutine_resume.hpp"

#include <astra/coroutine.hpp>
#include <astra/error.hpp>
#include <astra/scheduler.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
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

struct FrameTracker {
    static inline std::atomic<int> alive_count{0};
    static inline std::atomic<int> construct_count{0};
    static inline std::atomic<int> destruct_count{0};

    static void reset() {
        alive_count.store(0);
        construct_count.store(0);
        destruct_count.store(0);
    }

    static void wait_alive_zero() {
        for (int i = 0; i < 5000 && alive_count.load() != 0; ++i) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
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

// -----------------------------------------------------------------------------
// 辅助内建握手测试 Awaiter（验证 D-118 arm-trigger 协议）
// -----------------------------------------------------------------------------
struct TestEventAwaiter {
    struct EventState {
        astra::detail::AwaitHandshake handshake;
        std::atomic<bool> completed{false};
        std::function<void()> trigger_fn;
        std::mutex mtx;

        void signal() {
            std::function<void()> fn;
            {
                std::lock_guard<std::mutex> lk(mtx);
                completed.store(true, std::memory_order_release);
                fn = std::move(trigger_fn);
            }
            if (fn) {
                fn();
            }
        }
    };

    std::shared_ptr<EventState> event_state;
    bool trigger_before_arm{false};

    explicit TestEventAwaiter(std::shared_ptr<EventState> st, bool trigger_early = false)
        : event_state(std::move(st)), trigger_before_arm(trigger_early) {}

    bool await_ready() const noexcept {
        return event_state->completed.load(std::memory_order_acquire);
    }

    template <typename PromiseType>
    bool await_suspend(std::coroutine_handle<PromiseType> coro) {
        auto state = coro.promise().shared_state;
        auto rescheduler = state->get_rescheduler();

        auto post_action = [coro, state, rescheduler]() mutable {
            if (rescheduler) {
                auto invoker = std::make_unique<astra::detail::CoroutineResumeInvokerModel<typename PromiseType::value_type>>(
                    coro, std::move(state));
                rescheduler(std::move(invoker));
            }
        };

        {
            std::lock_guard<std::mutex> lk(event_state->mtx);
            event_state->trigger_fn = [&hs = event_state->handshake, post_action]() mutable {
                hs.trigger(std::move(post_action));
            };
        }

        // 若测试预设 trigger_before_arm，在 arm 前触发信号
        if (trigger_before_arm) {
            event_state->signal();
        }

        // 提交 Suspended 状态
        state->transition_to_suspended();
        state->mark_resume_handoff();

        // Arm 握手
        event_state->handshake.arm(std::move(post_action));
        return true;
    }

    void await_resume() const noexcept {}
};

// -----------------------------------------------------------------------------
// 1. completion-before-arm 验证（R-074 / D-118）
// -----------------------------------------------------------------------------
astra::Task<int> coro_test_completion_before_arm(std::shared_ptr<TestEventAwaiter::EventState> event) {
    FrameTracker tracker;
    co_await TestEventAwaiter(event, true /* trigger before arm */);
    co_return 42;
}

void test_R074_completion_before_arm() {
    FrameTracker::reset();
    astra::Scheduler scheduler;
    auto event = std::make_shared<TestEventAwaiter::EventState>();

    auto handle = scheduler.spawn(coro_test_completion_before_arm(event));
    TEST_ASSERT(handle.valid());

    int result = handle.get();
    TEST_ASSERT(result == 42);
    TEST_ASSERT(handle.state() == astra::TaskState::Succeeded);
    FrameTracker::wait_alive_zero();
    TEST_ASSERT(FrameTracker::alive_count.load() == 0);
    TEST_ASSERT(FrameTracker::construct_count.load() == FrameTracker::destruct_count.load());
}

// -----------------------------------------------------------------------------
// 2. arm-before-completion 验证（R-074 / D-118）
// -----------------------------------------------------------------------------
astra::Task<int> coro_test_arm_before_completion(std::shared_ptr<TestEventAwaiter::EventState> event) {
    FrameTracker tracker;
    co_await TestEventAwaiter(event, false /* trigger after arm */);
    co_return 99;
}

void test_R074_arm_before_completion() {
    FrameTracker::reset();
    astra::Scheduler scheduler;
    auto event = std::make_shared<TestEventAwaiter::EventState>();

    auto handle = scheduler.spawn(coro_test_arm_before_completion(event));
    TEST_ASSERT(handle.valid());

    // 等待协程进入 Suspended 状态
    while (handle.state() != astra::TaskState::Suspended) {
        std::this_thread::yield();
    }

    // 由外部线程触发完成
    event->signal();

    int result = handle.get();
    TEST_ASSERT(result == 99);
    TEST_ASSERT(handle.state() == astra::TaskState::Succeeded);
    FrameTracker::wait_alive_zero();
    TEST_ASSERT(FrameTracker::alive_count.load() == 0);
}

// -----------------------------------------------------------------------------
// 3. 多段挂起与恢复测试（Multi-step suspension & resumption）
// -----------------------------------------------------------------------------
astra::Task<int> coro_test_multistep(
    std::shared_ptr<TestEventAwaiter::EventState> ev1,
    std::shared_ptr<TestEventAwaiter::EventState> ev2,
    std::shared_ptr<TestEventAwaiter::EventState> ev3) {
    FrameTracker tracker;
    co_await TestEventAwaiter(ev1);
    co_await TestEventAwaiter(ev2);
    co_await TestEventAwaiter(ev3);
    co_return 777;
}

void test_R074_multistep_suspension_resumption() {
    FrameTracker::reset();
    astra::Scheduler scheduler;
    auto ev1 = std::make_shared<TestEventAwaiter::EventState>();
    auto ev2 = std::make_shared<TestEventAwaiter::EventState>();
    auto ev3 = std::make_shared<TestEventAwaiter::EventState>();

    auto handle = scheduler.spawn(coro_test_multistep(ev1, ev2, ev3));

    while (handle.state() != astra::TaskState::Suspended) {
        std::this_thread::yield();
    }
    ev1->signal();

    while (handle.state() != astra::TaskState::Suspended) {
        std::this_thread::yield();
    }
    ev2->signal();

    while (handle.state() != astra::TaskState::Suspended) {
        std::this_thread::yield();
    }
    ev3->signal();

    int result = handle.get();
    TEST_ASSERT(result == 777);
    TEST_ASSERT(handle.state() == astra::TaskState::Succeeded);
    FrameTracker::wait_alive_zero();
    TEST_ASSERT(FrameTracker::alive_count.load() == 0);
}

// -----------------------------------------------------------------------------
// 4. 高并发 arm/trigger 竞争压力测试（无 lost wake / 无 double resume / 恰好一次 destroy）
// -----------------------------------------------------------------------------
void test_R074_concurrent_arm_trigger_race() {
    FrameTracker::reset();
    astra::Scheduler scheduler;

    constexpr int kIterations = 100;
    std::vector<astra::TaskHandle<int>> handles;
    handles.reserve(kIterations);

    std::vector<std::shared_ptr<TestEventAwaiter::EventState>> events;
    events.reserve(kIterations);

    for (int i = 0; i < kIterations; ++i) {
        auto ev = std::make_shared<TestEventAwaiter::EventState>();
        events.push_back(ev);
        handles.push_back(scheduler.spawn(coro_test_arm_before_completion(ev)));
    }

    // 并发触发事件
    std::vector<std::thread> trigger_threads;
    for (int t = 0; t < 4; ++t) {
        trigger_threads.emplace_back([&events, t] {
            for (std::size_t i = t; i < events.size(); i += 4) {
                events[i]->signal();
            }
        });
    }

    for (auto& th : trigger_threads) {
        th.join();
    }

    for (auto& h : handles) {
        int r = h.get();
        TEST_ASSERT(r == 99);
        TEST_ASSERT(h.state() == astra::TaskState::Succeeded);
    }

    FrameTracker::wait_alive_zero();
    TEST_ASSERT(FrameTracker::alive_count.load() == 0);
    TEST_ASSERT(FrameTracker::construct_count.load() == FrameTracker::destruct_count.load());
}

}  // namespace

int main() {
    std::printf("Running astra_coroutine_resume_handshake_test...\n");
    test_R074_completion_before_arm();
    test_R074_arm_before_completion();
    test_R074_multistep_suspension_resumption();
    test_R074_concurrent_arm_trigger_race();
    std::printf("All AST-033 Coroutine resume handshake tests passed successfully!\n");
    return 0;
}

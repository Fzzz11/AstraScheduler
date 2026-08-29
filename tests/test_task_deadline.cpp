#include "astra/coroutine.hpp"
#include "astra/graph.hpp"
#include "astra/scheduler.hpp"
#include "astra/task_handle.hpp"
#include "astra/task_options.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// -----------------------------------------------------------------------------
// 1. TaskDeadline factory, saturating, and comparison tests (R-082 / D-132)
// -----------------------------------------------------------------------------
void test_R082_deadline_type_and_factory() {
    const auto now = std::chrono::steady_clock::now();
    const auto dl_at = astra::TaskDeadline::at(now + 100ms);
    assert(dl_at.time_point() == now + 100ms);

    const auto t0 = std::chrono::steady_clock::now();
    const auto dl_after = astra::TaskDeadline::after(50ms);
    const auto t1 = std::chrono::steady_clock::now();
    assert(dl_after.time_point() >= t0);
    assert(dl_after.time_point() <= t1 + 100ms);

    // 负 duration 形成过去的绝对时间点并正常构造
    const auto dl_past = astra::TaskDeadline::after(-100ms);
    assert(dl_past.time_point() < std::chrono::steady_clock::now());

    // 极值 saturating 保护
    const auto dl_max = astra::TaskDeadline::after(std::chrono::hours(1000000));
    assert(dl_max.time_point() > std::chrono::steady_clock::now());

    // TaskOptions 默认值与比较
    astra::TaskOptions def_opts;
    assert(def_opts.priority == astra::Priority::Normal);
    assert(!def_opts.deadline.has_value());

    astra::TaskOptions custom_opts{astra::Priority::High, dl_at};
    assert(custom_opts.priority == astra::Priority::High);
    assert(custom_opts.deadline.has_value());
    assert(custom_opts.deadline->time_point() == now + 100ms);

    astra::TaskOptions copy_opts = custom_opts;
    assert(copy_opts == custom_opts);
}

// -----------------------------------------------------------------------------
// 2. On-time start evaluation (Met) vs Missed start evaluation (Missed) (R-082 / D-132)
// -----------------------------------------------------------------------------
void test_R082_on_time_and_missed_disposition() {
    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    astra::Scheduler sched(opts);

    // 2.1 On-time: 充裕的截止时间 (5秒后)，首次开始必定 Met
    auto dl_future = astra::TaskDeadline::after(5s);
    auto h_met = sched.submit(astra::TaskOptions{astra::Priority::Normal, dl_future}, [] {
        return 42;
    });

    assert(h_met.get() == 42);
    assert(h_met.deadline().has_value());
    assert(h_met.deadline_disposition() == astra::DeadlineDisposition::Met);

    // 2.2 Missed: 过去的截止时间 (已经过期 100ms)，首次开始必定 Missed
    // 但任务必须正常执行，不取消，不改变返回值
    auto dl_past = astra::TaskDeadline::after(-100ms);
    auto h_missed = sched.submit(astra::TaskOptions{astra::Priority::High, dl_past}, [] {
        return 100;
    });

    assert(h_missed.get() == 100);
    assert(h_missed.deadline().has_value());
    assert(h_missed.deadline_disposition() == astra::DeadlineDisposition::Missed);
    assert(h_missed.state() == astra::TaskState::Succeeded);

    // 2.3 无 Deadline 的任务 disposition 为 None
    auto h_none = sched.submit([] { return 1; });
    assert(h_none.get() == 1);
    assert(!h_none.deadline().has_value());
    assert(h_none.deadline_disposition() == astra::DeadlineDisposition::None);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 3. Delayed admission: absolute deadline is fixed at factory call (R-082 / D-132)
// -----------------------------------------------------------------------------
void test_R082_delayed_admission_fixed_deadline() {
    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    astra::Scheduler sched(opts);

    // 3.1 构造 TaskOptions 时设置 10ms 相对截止时间
    const auto opts_delay = astra::TaskOptions{astra::Priority::Normal, astra::TaskDeadline::after(10ms)};
    const auto fixed_target = opts_delay.deadline->time_point();

    // 3.2 延迟 30ms 之后才提交到调度器（此时 deadline 已过期）
    std::this_thread::sleep_for(30ms);

    auto h = sched.submit(opts_delay, [] {
        return 999;
    });

    assert(h.get() == 999);
    assert(h.deadline().has_value());
    // 绝对时间点必须等于最初 factory 构造时的绝对时间点，未在 submit/admission 时被重新计时
    assert(h.deadline()->time_point() == fixed_target);
    assert(h.deadline_disposition() == astra::DeadlineDisposition::Missed);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 4. Cancellation before first start: disposition remains None (R-082 / D-132)
// -----------------------------------------------------------------------------
void test_R082_cancellation_before_start_no_disposition() {
    astra::SchedulerOptions opts;
    opts.worker_count = 1;
    astra::Scheduler sched(opts);

    // 4.1 用 blocker 占住 worker
    std::atomic<bool> blocker_running{false};
    std::atomic<bool> can_finish{false};
    auto blocker = sched.submit([&] {
        blocker_running.store(true);
        while (!can_finish.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
    });

    while (!blocker_running.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // 4.2 提交带有 Deadline 的任务，并在其开始前立即取消
    auto h_cancel = sched.submit(astra::TaskOptions{astra::Priority::Normal, astra::TaskDeadline::after(1s)}, [] {
        return 0;
    });
    h_cancel.request_cancel();

    // 4.3 释放 blocker 并等待
    can_finish.store(true, std::memory_order_release);
    blocker.wait();
    h_cancel.wait();

    assert(h_cancel.state() == astra::TaskState::Cancelled);
    // 未曾进入 Running 状态，disposition 保持 None
    assert(h_cancel.deadline_disposition() == astra::DeadlineDisposition::None);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 5. Coroutine first start vs resumption: no re-evaluation on resume (R-082 / D-132)
// -----------------------------------------------------------------------------
void test_R082_coroutine_resume_no_reevaluation() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    auto coro_task = [](astra::Scheduler& s) -> astra::Task<int> {
        // 挂起睡眠 30ms，睡眠醒来时原本的 10ms deadline 早已过期
        co_await astra::sleep_for(30ms);
        co_return 777;
    };

    // 设置 500ms 宽裕的截止时间，首次进入 Running 必定 Met
    auto h_coro = sched.spawn(
        astra::TaskOptions{astra::Priority::Normal, astra::TaskDeadline::after(500ms)},
        coro_task(sched));

    assert(h_coro.get() == 777);
    // 首次 start 判定为 Met，后续 resume 不重新判定
    assert(h_coro.deadline_disposition() == astra::DeadlineDisposition::Met);

    sched.shutdown();
}

// -----------------------------------------------------------------------------
// 6. Non-inheritance of Deadline by child tasks (R-082 / D-132)
// -----------------------------------------------------------------------------
void test_R082_no_deadline_inheritance() {
    astra::SchedulerOptions opts;
    opts.worker_count = 2;
    astra::Scheduler sched(opts);

    auto parent = sched.submit(
        astra::TaskOptions{astra::Priority::High, astra::TaskDeadline::after(1s)},
        [&] {
            // 内部提交无 options 的子任务
            auto child = sched.submit([] { return 123; });
            // 子任务不应继承 parent 的 deadline
            assert(!child.deadline().has_value());
            assert(child.deadline_disposition() == astra::DeadlineDisposition::None);
            assert(child.get() == 123);
            return true;
        });

    assert(parent.get() == true);
    assert(parent.deadline_disposition() == astra::DeadlineDisposition::Met);

    sched.shutdown();
}

}  // namespace

int main() {
    std::cout << "Running astra_task_deadline_test..." << std::endl;

    std::cout << "Running test 1 (type and factory)..." << std::endl;
    test_R082_deadline_type_and_factory();

    std::cout << "Running test 2 (on-time and missed disposition)..." << std::endl;
    test_R082_on_time_and_missed_disposition();

    std::cout << "Running test 3 (delayed admission fixed deadline)..." << std::endl;
    test_R082_delayed_admission_fixed_deadline();

    std::cout << "Running test 4 (cancellation before start)..." << std::endl;
    test_R082_cancellation_before_start_no_disposition();

    std::cout << "Running test 5 (coroutine resume no re-evaluation)..." << std::endl;
    test_R082_coroutine_resume_no_reevaluation();

    std::cout << "Running test 6 (no deadline inheritance)..." << std::endl;
    test_R082_no_deadline_inheritance();

    std::cout << "All AST-040 task deadline tests passed successfully!" << std::endl;
    return 0;
}

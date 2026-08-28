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
#include <variant>

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
// R-061: Reject 策略下容量耗尽立即拒绝（submit 抛异常，try_submit 返回枚举）
// -----------------------------------------------------------------------------
void test_R061_reject_capacity_exhaustion() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 1;
    opt.external_pending_capacity = 2;
    opt.external_backpressure = astra::ExternalBackpressure::Reject;

    astra::Scheduler s(opt);

    std::promise<void> worker_release_promise;
    std::shared_future<void> worker_release = worker_release_promise.get_future().share();
    std::atomic<bool> worker_started{false};

    // 1. 提交第 1 个任务并让 Worker 阻塞在其中（进入 Running 状态并已释放 slot）
    s.submit([&worker_started, worker_release]() {
        worker_started.store(true);
        worker_release.wait();
    });

    while (!worker_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 2. 提交 2 个 pending 外部任务，恰好占满容量配额（capacity = 2）
    auto h1 = s.submit([]() { return 1; });
    auto h2 = s.submit([]() { return 2; });

    TEST_ASSERT(astra::detail::external_pending_count(s) == 2);

    // 3. 第 3 个任务在 Reject 策略下必须立即被拒绝
    bool submit_threw_capacity_exhausted = false;
    try {
        s.submit([]() { return 3; });
    } catch (const astra::submission_rejected& e) {
        if (e.reason() == astra::SubmissionError::CapacityExhausted) {
            submit_threw_capacity_exhausted = true;
        }
    }
    TEST_ASSERT(submit_threw_capacity_exhausted);

    // 4. try_submit 必须返回 SubmissionError::CapacityExhausted 变体
    auto try_res = s.try_submit([]() { return 4; });
    TEST_ASSERT(std::holds_alternative<astra::SubmissionError>(try_res));
    TEST_ASSERT(std::get<astra::SubmissionError>(try_res) == astra::SubmissionError::CapacityExhausted);

    // 5. 释放 Worker，等待所有任务完成，容量配额应正确归零
    worker_release_promise.set_value();
    TEST_ASSERT(h1.get() == 1);
    TEST_ASSERT(h2.get() == 2);

    while (astra::detail::external_pending_count(s) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_ASSERT(astra::detail::external_pending_count(s) == 0);

    // 6. 容量释放后可以再次正常提交
    auto h3 = s.submit([]() { return 42; });
    TEST_ASSERT(h3.get() == 42);
}

// -----------------------------------------------------------------------------
// R-061 / D-086: Block 策略下普通非 Worker 线程阻塞等待并在 slot 释放后唤醒
// -----------------------------------------------------------------------------
void test_R061_block_backpressure_ordinary_thread() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 1;
    opt.external_pending_capacity = 1;
    opt.external_backpressure = astra::ExternalBackpressure::Block;

    astra::Scheduler s(opt);

    std::promise<void> worker_release_promise;
    std::shared_future<void> worker_release = worker_release_promise.get_future().share();
    std::atomic<bool> worker_started{false};

    // 1. 让 Worker 执行第 1 个阻塞任务
    s.submit([&worker_started, worker_release]() {
        worker_started.store(true);
        worker_release.wait();
    });

    while (!worker_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 2. 占满 pending slot（capacity = 1）
    auto h1 = s.submit([]() { return 100; });
    TEST_ASSERT(astra::detail::external_pending_count(s) == 1);

    // 3. 在后台普通线程中发起第 2 个 pending 提交（将因 Block 策略阻塞）
    std::atomic<bool> background_submit_started{false};
    std::atomic<bool> background_submit_finished{false};
    std::thread background_thread([&]() {
        background_submit_started.store(true);
        auto h2 = s.submit([]() { return 200; });
        background_submit_finished.store(true);
        TEST_ASSERT(h2.get() == 200);
    });

    while (!background_submit_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 稍作等待，确认后台线程处于阻塞状态（未完成）
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    TEST_ASSERT(!background_submit_finished.load());

    // 4. 释放 Worker，使第 1 个任务结束并启动 h1，从而释放 slot
    worker_release_promise.set_value();
    TEST_ASSERT(h1.get() == 100);

    // 5. 后台线程应被唤醒并成功完成提交与执行
    background_thread.join();
    TEST_ASSERT(background_submit_finished.load());
}

// -----------------------------------------------------------------------------
// R-061 / D-085: 跨 Runtime Worker 向满容量 Runtime 提交永不 Block，立即拒绝
// -----------------------------------------------------------------------------
void test_R061_worker_cross_runtime_never_blocks() {
    astra::SchedulerOptions opt_a{};
    opt_a.worker_count = 1;
    astra::Scheduler runtime_a(opt_a);

    astra::SchedulerOptions opt_b{};
    opt_b.worker_count = 1;
    opt_b.external_pending_capacity = 1;
    opt_b.external_backpressure = astra::ExternalBackpressure::Block; // 配置为 Block
    astra::Scheduler runtime_b(opt_b);

    std::promise<void> b_worker_release_promise;
    std::shared_future<void> b_worker_release = b_worker_release_promise.get_future().share();
    std::atomic<bool> b_worker_started{false};

    // 阻塞 Runtime B Worker
    runtime_b.submit([&b_worker_started, b_worker_release]() {
        b_worker_started.store(true);
        b_worker_release.wait();
    });

    while (!b_worker_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 占满 Runtime B 的 pending slot
    auto hb1 = runtime_b.submit([]() { return 10; });
    TEST_ASSERT(astra::detail::external_pending_count(runtime_b) == 1);

    // 从 Runtime A 的 Worker 线程向 Runtime B 发起 submit
    std::atomic<bool> cross_worker_executed{false};
    std::atomic<bool> cross_worker_rejected{false};

    runtime_a.submit([&]() {
        cross_worker_executed.store(true);
        try {
            // 虽然 Runtime B 配置了 Block，但对于来自 Runtime A 的 Worker 线程绝不能阻塞自锁，必须立即拒绝
            runtime_b.submit([]() { return 20; });
        } catch (const astra::submission_rejected& e) {
            if (e.reason() == astra::SubmissionError::CapacityExhausted) {
                cross_worker_rejected.store(true);
            }
        }
    });

    while (!cross_worker_executed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    while (!cross_worker_rejected.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    TEST_ASSERT(cross_worker_rejected.load());

    // 恢复并清理
    b_worker_release_promise.set_value();
    TEST_ASSERT(hb1.get() == 10);
}

// -----------------------------------------------------------------------------
// R-061 / D-083: 同 Runtime Internal Submission 豁免 External 配额限制
// -----------------------------------------------------------------------------
void test_R061_internal_submission_exempt_from_external_capacity() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 2;
    opt.external_pending_capacity = 1;
    opt.external_backpressure = astra::ExternalBackpressure::Reject;

    astra::Scheduler s(opt);

    std::promise<void> worker_release_promise;
    std::shared_future<void> worker_release = worker_release_promise.get_future().share();
    std::atomic<bool> worker_started{false};
    std::atomic<bool> internal_submit_succeeded{false};

    // 1. 在 Worker 内部提交自身同 Runtime 任务
    s.submit([&, worker_release]() {
        worker_started.store(true);

        // 即使 external slot 已满，Worker 内部的 Internal Submission 不占用 external slot，依然成功
        auto inner_h = s.submit([]() { return 999; });
        if (inner_h.get() == 999) {
            internal_submit_succeeded.store(true);
        }

        worker_release.wait();
    });

    while (!worker_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 2. 占满外部 pending slot（capacity = 1）
    auto h_ext = s.submit([]() { return 1; });
    TEST_ASSERT(astra::detail::external_pending_count(s) == 1);

    // 3. 释放 worker，内部提交应已成功完成
    worker_release_promise.set_value();
    TEST_ASSERT(h_ext.get() == 1);

    while (!internal_submit_succeeded.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_ASSERT(internal_submit_succeeded.load());
}

// -----------------------------------------------------------------------------
// R-061 / D-086: Block 等待者在 Scheduler 停机/析构时被唤醒并以 lifecycle rejection 拒绝
// -----------------------------------------------------------------------------
void test_R061_block_waiter_wakes_on_shutdown_rejection() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 1;
    opt.external_pending_capacity = 1;
    opt.external_backpressure = astra::ExternalBackpressure::Block;

    auto s = std::make_unique<astra::Scheduler>(opt);

    std::promise<void> worker_release_promise;
    std::shared_future<void> worker_release = worker_release_promise.get_future().share();
    std::atomic<bool> worker_started{false};

    // 占领 worker
    s->submit([&worker_started, worker_release]() {
        worker_started.store(true);
        worker_release.wait();
    });

    while (!worker_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 填满 slot
    s->submit([]() { return 1; });

    // 阻塞在 submit 上的后台线程
    std::atomic<bool> waiter_started{false};
    std::atomic<bool> waiter_rejected{false};
    std::thread waiter_thread([&]() {
        waiter_started.store(true);
        try {
            s->submit([]() { return 2; });
        } catch (const astra::submission_rejected& e) {
            if (e.reason() == astra::SubmissionError::Stopping ||
                e.reason() == astra::SubmissionError::Stopped) {
                waiter_rejected.store(true);
            }
        }
    });

    while (!waiter_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // 启动异步线程在稍后释放 worker，允许 Scheduler 顺利完成优雅析构
    std::thread release_thread([worker_release_promise = std::move(worker_release_promise)]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        worker_release_promise.set_value();
    });

    // 销毁 Scheduler 触发 Shutdown，关闭 gate 并唤醒 waiter
    s.reset();

    release_thread.join();
    waiter_thread.join();
    TEST_ASSERT(waiter_rejected.load());
}

// -----------------------------------------------------------------------------
// R-062 / D-089: 强异常安全事务——构造异常完全回滚 slot 与计数，不泄漏资源
// -----------------------------------------------------------------------------
struct ThrowOnCopy {
    ThrowOnCopy() = default;
    ThrowOnCopy(const ThrowOnCopy&) {
        throw std::runtime_error("simulated copy constructor exception");
    }
    ThrowOnCopy(ThrowOnCopy&&) = default;
};

void test_R062_strong_exception_safety_transaction_rollback() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 1;
    opt.external_pending_capacity = 1;
    opt.external_backpressure = astra::ExternalBackpressure::Reject;

    astra::Scheduler s(opt);

    ThrowOnCopy obj;
    bool exception_propagated = false;
    try {
        // 将按值捕获 ThrowOnCopy，在 TaskInvoker 构造期间抛出异常
        s.submit([obj]() {
            (void)obj;
        });
    } catch (const std::runtime_error& e) {
        if (std::string(e.what()) == "simulated copy constructor exception") {
            exception_propagated = true;
        }
    }
    TEST_ASSERT(exception_propagated);

    // 验证 slot 完全回滚，pending_count 为 0
    TEST_ASSERT(astra::detail::external_pending_count(s) == 0);

    // 后续提交不受影响，slot 可正常使用
    auto h = s.submit([]() { return 12345; });
    TEST_ASSERT(h.get() == 12345);
}

// -----------------------------------------------------------------------------
// R-062 / D-088: try_submit 规范行为矩阵（成功/容量/状态/空 Handle）
// -----------------------------------------------------------------------------
void test_R062_try_submit_result_matrix() {
    astra::Scheduler s;

    // 1. 成功提交返回 index 0 的 TaskHandle<T>
    auto res_int = s.try_submit([]() { return 777; });
    TEST_ASSERT(std::holds_alternative<astra::TaskHandle<int>>(res_int));
    TEST_ASSERT(std::get<astra::TaskHandle<int>>(res_int).get() == 777);

    auto res_void = s.try_submit([]() {});
    TEST_ASSERT(std::holds_alternative<astra::TaskHandle<void>>(res_void));
    std::get<astra::TaskHandle<void>>(res_void).get();

    // 2. 空 Handle 抛 std::logic_error
    astra::Scheduler empty_sched(std::move(s));
    TEST_ASSERT(!s.valid());

    bool logic_error_thrown = false;
    try {
        s.try_submit([]() {});
    } catch (const std::logic_error&) {
        logic_error_thrown = true;
    }
    TEST_ASSERT(logic_error_thrown);
}

}  // namespace

int main() {
    std::printf("Running astra_admission_backpressure_test...\n");
    test_R061_reject_capacity_exhaustion();
    test_R061_block_backpressure_ordinary_thread();
    test_R061_worker_cross_runtime_never_blocks();
    test_R061_internal_submission_exempt_from_external_capacity();
    test_R061_block_waiter_wakes_on_shutdown_rejection();
    test_R062_strong_exception_safety_transaction_rollback();
    test_R062_try_submit_result_matrix();
    std::printf("All AST-010 admission backpressure tests passed successfully!\n");
    return 0;
}

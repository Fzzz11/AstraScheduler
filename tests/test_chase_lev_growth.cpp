#include "../src/chase_lev_deque.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
// 1. R-067 / D-099: 极小初始容量（2）强制多次扩容，保留旧 Buffer 且并发 Steal 无 UAF
// -----------------------------------------------------------------------------
void test_R067_growth_preserves_old_buffers_without_uaf() {
    constexpr int kTotalItems = 500;
    constexpr int kThieves = 4;

    // 初始容量设为极小的 2，强制频繁触发连续 doubling 扩容
    astra::detail::ChaseLevDeque<int> deque(2);
    TEST_ASSERT(deque.history_buffer_count() == 1);
    TEST_ASSERT(deque.current_capacity() == 2);

    std::atomic<bool> producer_done{false};
    std::vector<std::atomic<int>> item_claim_count(kTotalItems);
    for (int i = 0; i < kTotalItems; ++i) {
        item_claim_count[i].store(0, std::memory_order_relaxed);
    }

    std::vector<std::thread> thieves;
    thieves.reserve(kThieves);

    // 1. 先批量 push 100 个元素触发多次确定性扩容（从容量 2 扩容到 128）
    for (int i = 0; i < 100; ++i) {
        bool ok = deque.push(i);
        TEST_ASSERT(ok);
    }
    TEST_ASSERT(deque.history_buffer_count() >= 6);
    TEST_ASSERT(deque.current_capacity() >= 128);

    // 2. 启动并发 Thieves 窃取
    for (int t = 0; t < kThieves; ++t) {
        thieves.emplace_back([&] {
            while (!producer_done.load(std::memory_order_acquire) || !deque.empty()) {
                int item = -1;
                auto res = deque.steal(item);
                if (res == astra::detail::DequeResultStatus::Success) {
                    TEST_ASSERT(item >= 0 && item < kTotalItems);
                    item_claim_count[item].fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // 3. Owner 线程继续并发 push 剩余任务
    for (int i = 100; i < kTotalItems; ++i) {
        bool ok = deque.push(i);
        TEST_ASSERT(ok);
        if (i % 4 == 0) {
            int popped = -1;
            if (deque.pop(popped) == astra::detail::DequeResultStatus::Success) {
                TEST_ASSERT(popped >= 0 && popped < kTotalItems);
                item_claim_count[popped].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    producer_done.store(true, std::memory_order_release);

    int remaining = -1;
    while (deque.pop(remaining) == astra::detail::DequeResultStatus::Success) {
        TEST_ASSERT(remaining >= 0 && remaining < kTotalItems);
        item_claim_count[remaining].fetch_add(1, std::memory_order_relaxed);
    }

    for (auto& th : thieves) {
        th.join();
    }

    // 验证历史 buffer 已保留多代
    TEST_ASSERT(deque.history_buffer_count() >= 6);
    TEST_ASSERT(deque.current_capacity() >= 128);

    // 验证每个 Task 恰好执行/认领一次（单一调度引用，D-100）
    for (int i = 0; i < kTotalItems; ++i) {
        int count = item_claim_count[i].load(std::memory_order_relaxed);
        TEST_ASSERT(count == 1);
    }
}

// -----------------------------------------------------------------------------
// 2. R-067 / D-100: 故障注入下 Local 扩容失败返回 false 并回退
// -----------------------------------------------------------------------------
void test_R067_growth_failure_injection_fallback() {
    astra::detail::ChaseLevDeque<int> deque(4);
    TEST_ASSERT(deque.current_capacity() == 4);

    // 填入 3 个元素（容量 4 保留 1 个空 cell，达到扩容阈值）
    TEST_ASSERT(deque.push(101));
    TEST_ASSERT(deque.push(102));
    TEST_ASSERT(deque.push(103));

    // 启用故障注入：模拟内存耗尽导致扩容失败
    deque.set_inject_growth_failure(true);

    // 再次 push 必须返回 false，不得 crash，不得破坏原有数据
    bool pushed = deque.push(104);
    TEST_ASSERT(!pushed);

    // 恢复正常
    deque.set_inject_growth_failure(false);

    // 原有 3 个元素仍然完好无损可被正常消费
    int val = 0;
    TEST_ASSERT(deque.pop(val) == astra::detail::DequeResultStatus::Success);
    TEST_ASSERT(val == 103);
    TEST_ASSERT(deque.pop(val) == astra::detail::DequeResultStatus::Success);
    TEST_ASSERT(val == 102);
    TEST_ASSERT(deque.pop(val) == astra::detail::DequeResultStatus::Success);
    TEST_ASSERT(val == 101);
    TEST_ASSERT(deque.empty());
}

}  // namespace

int main() {
    std::printf("Running astra_chase_lev_growth_test...\n");
    test_R067_growth_preserves_old_buffers_without_uaf();
    test_R067_growth_failure_injection_fallback();
    std::printf("All AST-026 Chase-Lev growth & retention tests passed successfully!\n");
    return 0;
}

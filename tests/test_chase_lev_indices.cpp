#include "../src/scheduling/chase_lev_deque.hpp"
#include <astra/capabilities.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>
#include "expected_local_backend.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

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
// 1. R-068 / D-102: 三态结果区分（Success / Empty / Retry）与边界算术
// -----------------------------------------------------------------------------
void test_R068_boundary_states_three_way_result() {
    astra::detail::ChaseLevDeque<int> deque(8);
    int val = 0;

    // 空队列 pop / steal 返回 Empty
    TEST_ASSERT(deque.pop(val) == astra::detail::DequeResultStatus::Empty);
    TEST_ASSERT(deque.steal(val) == astra::detail::DequeResultStatus::Empty);

    // 存入 1 个元素
    TEST_ASSERT(deque.push(42));
    TEST_ASSERT(deque.size() == 1);
    TEST_ASSERT(!deque.empty());

    // 成功 pop 返回 Success
    TEST_ASSERT(deque.pop(val) == astra::detail::DequeResultStatus::Success);
    TEST_ASSERT(val == 42);

    // 再次 pop 返回 Empty
    TEST_ASSERT(deque.pop(val) == astra::detail::DequeResultStatus::Empty);
}

// -----------------------------------------------------------------------------
// 2. R-068 / D-101: 高水位 Quiescent Rebase（安全归零不依赖无符号溢出环绕）
// -----------------------------------------------------------------------------
void test_R068_quiescent_rebase_high_watermark() {
    astra::detail::ChaseLevDeque<int> deque(8);

    // 模拟队列长期运行后 top/bottom 达到高水位（如 1,000,000）且当前静止为空
    deque.set_test_indices(1000000, 1000000);
    TEST_ASSERT(deque.empty());

    // 触发静止 Rebase
    bool rebased = deque.maybe_quiescent_rebase(1000000);
    TEST_ASSERT(rebased);

    // 验证后续 push / pop 正常工作在 0 起始基线
    TEST_ASSERT(deque.push(888));
    int val = 0;
    TEST_ASSERT(deque.pop(val) == astra::detail::DequeResultStatus::Success);
    TEST_ASSERT(val == 888);
    TEST_ASSERT(deque.empty());
}

// -----------------------------------------------------------------------------
// 2b. R-068 / D-103: 空队列在 canonical zero 上 pop 不得 underflow。
// -----------------------------------------------------------------------------
void test_R068_empty_pop_at_zero_does_not_underflow() {
    astra::detail::ChaseLevDeque<int> deque(8);
    int val = 0;
    TEST_ASSERT(deque.pop(val) == astra::detail::DequeResultStatus::Empty);
    TEST_ASSERT(deque.size() == 0);
    TEST_ASSERT(deque.bottom_for_testing() == 0);
    TEST_ASSERT(deque.top_for_testing() == 0);
    TEST_ASSERT(deque.push(3));
    TEST_ASSERT(deque.pop(val) == astra::detail::DequeResultStatus::Success);
    TEST_ASSERT(val == 3);
}

// -----------------------------------------------------------------------------
// 2c. R-068 / D-101: owner push 在高水位必须进入生产 rebase，而不是继续累加索引。
// -----------------------------------------------------------------------------
void test_R068_owner_push_rebases_high_watermark() {
    astra::detail::ChaseLevDeque<int> deque(8);
    constexpr std::uint64_t kHigh = UINT64_C(1) << 58;
    deque.set_test_indices(kHigh, kHigh);
    TEST_ASSERT(deque.empty());
    TEST_ASSERT(deque.push(7));
    TEST_ASSERT(deque.bottom_for_testing() == 1);
    TEST_ASSERT(deque.top_for_testing() == 0);
    int val = 0;
    TEST_ASSERT(deque.pop(val) == astra::detail::DequeResultStatus::Success);
    TEST_ASSERT(val == 7);
}

// -----------------------------------------------------------------------------
// 2d. R-068 / D-101 / D-102: rebase maintenance 期间新 steal 返回 Retry，不是 Empty。
// -----------------------------------------------------------------------------
void test_R068_steal_retries_during_rebase_maintenance() {
    astra::detail::ChaseLevDeque<int> deque(8);
    TEST_ASSERT(deque.push(1));
    deque.set_maintenance_for_testing(true);
    int val = 0;
    TEST_ASSERT(deque.steal(val) == astra::detail::DequeResultStatus::Retry);
    deque.set_maintenance_for_testing(false);
    TEST_ASSERT(deque.steal(val) == astra::detail::DequeResultStatus::Success);
    TEST_ASSERT(val == 1);
}

// -----------------------------------------------------------------------------
// 3. R-101 / D-162 / D-167: capability 精确反映生产 ReadyQueues backend。
// -----------------------------------------------------------------------------
void test_R101_scheduler_capabilities_reflect_chase_lev_lock_free() {
    astra::Scheduler s;
    const auto caps = s.capabilities();

    TEST_ASSERT(caps.local_deque_backend() == kExpectedLocalDequeBackend);
    TEST_ASSERT(caps.lock_free_local_deque() == kExpectedLocalDequeLockFree);

    // 关停后能力快照依然保留且不可变
    s.shutdown();
    const auto post_shutdown_caps = s.capabilities();
    TEST_ASSERT(post_shutdown_caps == caps);

    // 空/移动后的 Scheduler 抛出 std::logic_error
    astra::Scheduler s_moved = std::move(s);
    try {
        (void)s.capabilities();
        TEST_ASSERT(false && "Empty Scheduler must throw logic_error");
    } catch (const std::logic_error&) {
        // Expected
    }

    TEST_ASSERT(
        s_moved.capabilities().lock_free_local_deque() == kExpectedLocalDequeLockFree);
}

}  // namespace

int main() {
    std::printf("Running astra_chase_lev_indices_test...\n");
    test_R068_boundary_states_three_way_result();
    test_R068_quiescent_rebase_high_watermark();
    test_R068_empty_pop_at_zero_does_not_underflow();
    test_R068_owner_push_rebases_high_watermark();
    test_R068_steal_retries_during_rebase_maintenance();
    test_R101_scheduler_capabilities_reflect_chase_lev_lock_free();
    std::printf("All AST-027 Chase-Lev indices & backend truth tests passed successfully!\n");
    return 0;
}

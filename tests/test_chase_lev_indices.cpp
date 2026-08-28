#include "../src/chase_lev_deque.hpp"
#include <astra/capabilities.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>

#include <atomic>
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
// 3. R-101 / D-162 / D-167: v0.3 正式启用 ChaseLevLockFree 能力报告
// -----------------------------------------------------------------------------
void test_R101_scheduler_capabilities_reflect_chase_lev_lock_free() {
    astra::Scheduler s;
    const auto caps = s.capabilities();

    // v0.3.0 在 64-bit Linux 平台上已真实启用 Chase-Lev 无锁双端队列
    TEST_ASSERT(caps.local_deque_backend() == astra::LocalDequeBackend::ChaseLevLockFree);
    TEST_ASSERT(caps.lock_free_local_deque() == true);

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

    TEST_ASSERT(s_moved.capabilities().lock_free_local_deque() == true);
}

}  // namespace

int main() {
    std::printf("Running astra_chase_lev_indices_test...\n");
    test_R068_boundary_states_three_way_result();
    test_R068_quiescent_rebase_high_watermark();
    test_R101_scheduler_capabilities_reflect_chase_lev_lock_free();
    std::printf("All AST-027 Chase-Lev indices & backend truth tests passed successfully!\n");
    return 0;
}

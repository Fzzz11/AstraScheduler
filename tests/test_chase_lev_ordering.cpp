#include "../src/scheduling/chase_lev_deque.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#define TEST_ASSERT(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond, __FILE__, __LINE__);    \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

namespace {

// -----------------------------------------------------------------------------
// 1. 单线程 LIFO (Owner Pop) 与 FIFO (Thief Steal) 语义验证
// -----------------------------------------------------------------------------
void test_R066_single_thread_lifo_fifo_contract() {
    // (a) Oracle
    {
        astra::detail::ChaseLevSeqCstOracle<int> oracle(16);
        TEST_ASSERT(oracle.empty());
        oracle.push(10);
        oracle.push(20);
        oracle.push(30);
        TEST_ASSERT(oracle.size() == 3);

        int val = 0;
        // Owner Pop 必须是 LIFO (30 -> 20)
        auto res = oracle.pop(val);
        TEST_ASSERT(res == astra::detail::DequeResultStatus::Success);
        TEST_ASSERT(val == 30);

        // Thief Steal 必须是 FIFO (10)
        res = oracle.steal(val);
        TEST_ASSERT(res == astra::detail::DequeResultStatus::Success);
        TEST_ASSERT(val == 10);

        // 剩余最后一个元素 (20)
        res = oracle.pop(val);
        TEST_ASSERT(res == astra::detail::DequeResultStatus::Success);
        TEST_ASSERT(val == 20);

        // 空队列
        res = oracle.pop(val);
        TEST_ASSERT(res == astra::detail::DequeResultStatus::Empty);
        res = oracle.steal(val);
        TEST_ASSERT(res == astra::detail::DequeResultStatus::Empty);
    }

    // (b) Production Portable
    {
        astra::detail::ChaseLevDeque<int> deque(16);
        TEST_ASSERT(deque.empty());
        deque.push(10);
        deque.push(20);
        deque.push(30);
        TEST_ASSERT(deque.size() == 3);

        int val = 0;
        // Owner Pop 必须是 LIFO (30 -> 20)
        auto res = deque.pop(val);
        TEST_ASSERT(res == astra::detail::DequeResultStatus::Success);
        TEST_ASSERT(val == 30);

        // Thief Steal 必须是 FIFO (10)
        res = deque.steal(val);
        TEST_ASSERT(res == astra::detail::DequeResultStatus::Success);
        TEST_ASSERT(val == 10);

        // 剩余最后一个元素 (20)
        res = deque.pop(val);
        TEST_ASSERT(res == astra::detail::DequeResultStatus::Success);
        TEST_ASSERT(val == 20);

        // 空队列
        res = deque.pop(val);
        TEST_ASSERT(res == astra::detail::DequeResultStatus::Empty);
        res = deque.steal(val);
        TEST_ASSERT(res == astra::detail::DequeResultStatus::Empty);
    }
}

// -----------------------------------------------------------------------------
// 2. Last-item Race 决胜仲裁（Owner Pop 与多并发 Thieves 争夺单个元素）
// -----------------------------------------------------------------------------
void test_R066_last_item_race_resolution() {
    constexpr int kRounds = 1000;
    constexpr int kThieves = 4;

    for (int round = 0; round < kRounds; ++round) {
        astra::detail::ChaseLevDeque<int> deque(16);
        deque.push(round + 100);

        std::atomic<bool> start_signal{false};
        std::atomic<int> success_count{0};
        std::atomic<int> captured_value{-1};

        std::vector<std::thread> threads;
        threads.reserve(kThieves + 1);

        // Owner 线程执行 Pop
        threads.emplace_back([&] {
            while (!start_signal.load(std::memory_order_acquire)) {
            }
            int val = 0;
            if (deque.pop(val) == astra::detail::DequeResultStatus::Success) {
                success_count.fetch_add(1, std::memory_order_relaxed);
                captured_value.store(val, std::memory_order_relaxed);
            }
        });

        // Thief 线程执行 Steal
        for (int t = 0; t < kThieves; ++t) {
            threads.emplace_back([&] {
                while (!start_signal.load(std::memory_order_acquire)) {
                }
                int val = 0;
                if (deque.steal(val) == astra::detail::DequeResultStatus::Success) {
                    success_count.fetch_add(1, std::memory_order_relaxed);
                    captured_value.store(val, std::memory_order_relaxed);
                }
            });
        }

        // 释放竞争信号
        start_signal.store(true, std::memory_order_release);

        for (auto& th : threads) {
            th.join();
        }

        // 恰好只有一个线程胜出获得元素（R-066 / D-098 Invariant）
        TEST_ASSERT(success_count.load() == 1);
        TEST_ASSERT(captured_value.load() == round + 100);
        TEST_ASSERT(deque.empty());
    }
}

// -----------------------------------------------------------------------------
// 3. Oracle 扩容期间的 buffer/index 快照必须一致（R-066 / R-067）
// -----------------------------------------------------------------------------
struct OracleStealSnapshotGate {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};

    static void pause(void* context) noexcept {
        auto* gate = static_cast<OracleStealSnapshotGate*>(context);
        gate->entered.store(true, std::memory_order_release);
        while (!gate->release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
};

void test_R066_R067_oracle_growth_snapshot_consistency() {
    astra::detail::ChaseLevSeqCstOracle<int> oracle(2);

    // 在旧 buffer 留下一个已消费值，后续可检测 thief 是否错误读取旧代 cell。
    oracle.push(100);
    int value = -1;
    TEST_ASSERT(oracle.pop(value) == astra::detail::DequeResultStatus::Success);
    TEST_ASSERT(value == 100);

    OracleStealSnapshotGate gate;
    oracle.set_before_steal_index_snapshot_hook(&OracleStealSnapshotGate::pause, &gate);

    astra::detail::DequeResultStatus steal_status = astra::detail::DequeResultStatus::Empty;
    int stolen = -1;
    std::thread thief([&] { steal_status = oracle.steal(stolen); });

    while (!gate.entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Thief 暂停在索引快照前；Owner 扩容、清空队列，再在新 buffer 发布 500。
    oracle.push(200);
    oracle.push(300);
    oracle.push(400);
    TEST_ASSERT(oracle.pop(value) == astra::detail::DequeResultStatus::Success);
    TEST_ASSERT(value == 400);
    TEST_ASSERT(oracle.pop(value) == astra::detail::DequeResultStatus::Success);
    TEST_ASSERT(value == 300);
    TEST_ASSERT(oracle.pop(value) == astra::detail::DequeResultStatus::Success);
    TEST_ASSERT(value == 200);
    oracle.push(500);

    gate.release.store(true, std::memory_order_release);
    thief.join();

    TEST_ASSERT(steal_status == astra::detail::DequeResultStatus::Success);
    TEST_ASSERT(stolen == 500);
    TEST_ASSERT(oracle.empty());
}

// -----------------------------------------------------------------------------
// 4. 高并发多 Thief 差分压测（1 Owner + 4 Thieves, 10,000 Tasks）
// -----------------------------------------------------------------------------
template <typename DequeType> void run_stress_test(const char* name) {
    constexpr int kTotalItems = 10000;
    constexpr int kThieves = 4;

    DequeType deque(32);
    std::atomic<bool> producer_done{false};
    std::vector<std::atomic<int>> item_claim_count(kTotalItems);
    for (int i = 0; i < kTotalItems; ++i) {
        item_claim_count[i].store(0, std::memory_order_relaxed);
    }

    std::vector<std::thread> thieves;
    thieves.reserve(kThieves);

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

    // Owner 线程：交替 push 与 pop
    for (int i = 0; i < kTotalItems; ++i) {
        deque.push(i);
        if (i % 3 == 0) {
            int popped = -1;
            if (deque.pop(popped) == astra::detail::DequeResultStatus::Success) {
                TEST_ASSERT(popped >= 0 && popped < kTotalItems);
                item_claim_count[popped].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    producer_done.store(true, std::memory_order_release);

    for (auto& th : thieves) {
        th.join();
    }

    // 此时所有 Thief 均已退出，Owner 单线程排空剩余所有元素
    int remaining = -1;
    while (deque.pop(remaining) == astra::detail::DequeResultStatus::Success) {
        TEST_ASSERT(remaining >= 0 && remaining < kTotalItems);
        item_claim_count[remaining].fetch_add(1, std::memory_order_relaxed);
    }

    // 验证每个元素均被消费且仅被消费一次（No duplicate, no lost）
    for (int i = 0; i < kTotalItems; ++i) {
        int count = item_claim_count[i].load(std::memory_order_relaxed);
        TEST_ASSERT(count == 1);
    }

    std::printf("  [%s] stress completed successfully: 10000 items uniquely claimed\n", name);
}

void test_R066_oracle_portable_differential_stress() {
    run_stress_test<astra::detail::ChaseLevSeqCstOracle<int>>("ChaseLevSeqCstOracle");
    run_stress_test<astra::detail::ChaseLevDeque<int>>("ChaseLevPortableDeque");
}

} // namespace

int main() {
    std::printf("Running astra_chase_lev_ordering_test...\n");
    test_R066_single_thread_lifo_fifo_contract();
    test_R066_last_item_race_resolution();
    test_R066_R067_oracle_growth_snapshot_consistency();
    test_R066_oracle_portable_differential_stress();
    std::printf("All AST-025 Chase-Lev memory ordering tests passed successfully!\n");
    return 0;
}

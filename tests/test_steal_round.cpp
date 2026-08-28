#include <astra/error.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unordered_set>
#include <vector>

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond,     \
                         __FILE__, __LINE__);                                  \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

namespace astra::detail {
void generate_steal_victims(
    std::size_t self_index,
    std::size_t worker_count,
    std::size_t probe_limit,
    std::uint64_t& rng_state,
    std::vector<std::size_t>& out_victims);
}

namespace {

// -----------------------------------------------------------------------------
// R-064: Steal Round 有界、victim 不重复且排除自身、固定 seed 可复现
// -----------------------------------------------------------------------------
void test_R064_victim_selector_invariants() {
    std::vector<std::size_t> victims;

    // 1. worker_count = 1 时无 victim
    std::uint64_t rng1 = 12345;
    astra::detail::generate_steal_victims(0, 1, 8, rng1, victims);
    TEST_ASSERT(victims.empty());

    // 2. worker_count = 4, probe_limit = 8 时受限于 (worker_count - 1) = 3
    std::uint64_t rng2 = 42;
    astra::detail::generate_steal_victims(0, 4, 8, rng2, victims);
    TEST_ASSERT(victims.size() == 3);
    for (std::size_t v : victims) {
        TEST_ASSERT(v != 0);
        TEST_ASSERT(v < 4);
    }
    std::unordered_set<std::size_t> set2(victims.begin(), victims.end());
    TEST_ASSERT(set2.size() == 3); // 严格不重复

    // 3. worker_count = 16, probe_limit = 4 时受限于 probe_limit
    std::uint64_t rng3 = 999;
    astra::detail::generate_steal_victims(5, 16, 4, rng3, victims);
    TEST_ASSERT(victims.size() == 4);
    for (std::size_t v : victims) {
        TEST_ASSERT(v != 5); // 排除自身
        TEST_ASSERT(v < 16);
    }
    std::unordered_set<std::size_t> set3(victims.begin(), victims.end());
    TEST_ASSERT(set3.size() == 4); // 严格不重复

    // 4. 固定 seed 可完全复现序列
    std::uint64_t rng_a = 0xDEADBEEF;
    std::vector<std::size_t> seq_a;
    astra::detail::generate_steal_victims(2, 10, 5, rng_a, seq_a);

    std::uint64_t rng_b = 0xDEADBEEF;
    std::vector<std::size_t> seq_b;
    astra::detail::generate_steal_victims(2, 10, 5, rng_b, seq_b);

    TEST_ASSERT(seq_a == seq_b);
}

// -----------------------------------------------------------------------------
// R-064: 运行期 Work Stealing 窃取并完成任务
// -----------------------------------------------------------------------------
void test_R064_work_stealing_execution() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 2;
    opt.steal_probe_limit = 8;
    astra::Scheduler s(opt);

    std::atomic<int> completed_count{0};

    // 提交外部根任务
    auto root = s.submit([&s, &completed_count] {
        // 在 worker 0 上批量生成 20 个本地子任务
        for (int i = 0; i < 20; ++i) {
            (void)s.submit([&completed_count] {
                ++completed_count;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            });
        }
    });

    root.wait();

    // 等待所有窃取/执行的子任务完成
    auto start = std::chrono::steady_clock::now();
    while (completed_count.load() < 20) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
            TEST_ASSERT(false && "Timeout waiting for stolen tasks to complete");
        }
    }

    s.shutdown();
    TEST_ASSERT(completed_count.load() == 20);
}

}  // namespace

int main() {
    std::printf("Running astra_steal_round_test...\n");
    test_R064_victim_selector_invariants();
    test_R064_work_stealing_execution();
    std::printf("All AST-023 steal round tests passed successfully!\n");
    return 0;
}

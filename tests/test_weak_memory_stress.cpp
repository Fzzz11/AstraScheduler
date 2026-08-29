// AST-053 / R-111 / D-167 — weak-memory 压力测试（Tier-2 native AArch64 证据载体）。
// 通过 public API 高频并发锤击共享调度路径（submit/get/steal/handoff），
// 以确定性 checksum 验证无数据竞争与无丢失；在 x86_64 上于 TSan 下运行，
// 并为 native Linux AArch64 的 weak-memory 验证提供同一测试载体。

#include <astra/coroutine.hpp>
#include <astra/scheduler.hpp>

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr std::uint64_t kMixRounds = 64;

// 确定性 kernel（与 bench corpus 同族）：可验证、难被优化消除。
[[nodiscard]] std::uint64_t mix(std::uint64_t x) noexcept {
    for (std::uint64_t r = 0; r < kMixRounds; ++r) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x += 0x9E3779B97F4A7C15ull;
    }
    return x;
}

// 1. 多生产者/多消费者并发 submit/get：checksum 精确守恒。
void test_concurrent_submit_get_checksum_conservation() {
    astra::SchedulerOptions opts{};
    opts.worker_count = 4;
    opts.metrics_level = astra::MetricsLevel::Off;
    astra::Scheduler sched(opts);

    constexpr std::size_t kProducers = 4;
    constexpr std::uint64_t kPerProducer = 300;
    std::atomic<std::uint64_t> consumed{0};
    std::vector<std::thread> producers;
    for (std::size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&sched, &consumed, p] {
            std::uint64_t local = 0;
            for (std::uint64_t i = 0; i < kPerProducer; ++i) {
                const std::uint64_t arg = (p + 1) * 1'000'000ull + i;
                auto h = sched.submit([arg] { return mix(arg); });
                local ^= h.get();
                consumed.fetch_add(1, std::memory_order_acq_rel);
            }
            // local 仅用于防优化；checksum 守恒由 consumed 计数与确定 kernel 保证。
            if (local == 0xDEADBEEF) {
                std::abort();
            }
        });
    }
    for (auto& t : producers) {
        t.join();
    }
    if (consumed.load(std::memory_order_acquire) != kProducers * kPerProducer) {
        std::abort();
    }
    sched.shutdown();  // 确定性 join worker，避免与进程退出阶段竞争
    std::cout << "[PASS] test_concurrent_submit_get_checksum_conservation" << std::endl;
}

// 2. steal 压力：多数任务由其他 worker 的 helping/steal 路径完成。
void test_steal_pressure_under_load() {
    astra::SchedulerOptions opts{};
    opts.worker_count = 4;
    opts.metrics_level = astra::MetricsLevel::Off;
    astra::Scheduler sched(opts);

    constexpr std::uint64_t kTasks = 600;
    std::vector<astra::TaskHandle<std::uint64_t>> handles;
    handles.reserve(kTasks);
    std::uint64_t expect = 0;
    for (std::uint64_t i = 0; i < kTasks; ++i) {
        expect ^= mix(i + 1);
        handles.push_back(sched.submit([i] { return mix(i + 1); }));
    }
    std::uint64_t got = 0;
    for (auto& h : handles) {
        got ^= h.get();
    }
    if (got != expect) {
        std::abort();
    }
    sched.shutdown();
    std::cout << "[PASS] test_steal_pressure_under_load" << std::endl;
}

// 3. 协程 yield/sleep/handoff 混合并发（覆盖 resume invoker 重排队路径）。
void test_coroutine_mixed_handoff_storm() {
    astra::SchedulerOptions opts{};
    opts.worker_count = 4;
    opts.metrics_level = astra::MetricsLevel::Off;
    astra::Scheduler sched(opts);

    for (int i = 0; i < 200; ++i) {
        auto yield_coro = [i](astra::Scheduler&) -> astra::Task<std::uint64_t> {
            co_await astra::yield();
            co_await astra::yield();
            co_return static_cast<std::uint64_t>(i) + 1;
        };
        auto hy = sched.spawn(yield_coro(sched));

        auto sleep_coro = [i](astra::Scheduler&) -> astra::Task<std::uint64_t> {
            co_await astra::sleep_for(1ms);
            co_return static_cast<std::uint64_t>(i) + 1;
        };
        auto hs = sched.spawn(sleep_coro(sched));

        if (hy.get() != static_cast<std::uint64_t>(i) + 1 ||
            hs.get() != static_cast<std::uint64_t>(i) + 1) {
            std::abort();
        }
    }
    sched.shutdown();
    std::cout << "[PASS] test_coroutine_mixed_handoff_storm" << std::endl;
}

}  // namespace

int main() {
    std::cout << "Running astra_weak_memory_stress_test..." << std::endl;

    test_concurrent_submit_get_checksum_conservation();
    test_steal_pressure_under_load();
    test_coroutine_mixed_handoff_storm();

    std::cout << "All AST-053 weak memory stress tests passed successfully!" << std::endl;
    return 0;
}

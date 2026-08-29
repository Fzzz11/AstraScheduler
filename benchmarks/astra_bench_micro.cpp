// AstraScheduler micro benchmarks（AST-049 / R-089 / D-141）。
// 固定/pinned Google Benchmark 承载纯机制 micro case：queue primitive、
// admission、TaskHandle outcome、Coroutine suspend/resume、Timer。
// 仅在 ASTRA_BUILD_BENCHMARKS=ON 且 pinned 依赖可用时构建。

#include <astra/coroutine.hpp>
#include <astra/scheduler.hpp>
#include <astra/task_handle.hpp>

#include <benchmark/benchmark.h>

#include <chrono>

namespace {

// BM_submit_throughput：timed region 由 Google Benchmark iteration 承载；
// Scheduler 构造在 BM 体外（计时区不混入构建/销毁，R-089）。
void BM_SubmitDrain(benchmark::State& state) {
    astra::SchedulerOptions opts{};
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Off;
    astra::Scheduler sched(opts);

    for (auto _ : state) {
        auto h = sched.submit([] { return 1; });
        benchmark::DoNotOptimize(h.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SubmitDrain);

void BM_TaskOutcomeGet(benchmark::State& state) {
    astra::SchedulerOptions opts{};
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Off;
    astra::Scheduler sched(opts);

    for (auto _ : state) {
        auto h = sched.submit([] { return 7; });
        benchmark::DoNotOptimize(h.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TaskOutcomeGet);

void BM_CoroutineSuspendResume(benchmark::State& state) {
    astra::SchedulerOptions opts{};
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Off;
    astra::Scheduler sched(opts);

    auto coro_fn = [&](astra::Scheduler& s) -> astra::Task<int> {
        co_await astra::yield();
        co_return 1;
    };
    for (auto _ : state) {
        auto h = sched.spawn(coro_fn(sched));
        benchmark::DoNotOptimize(h.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CoroutineSuspendResume);

void BM_TimerRegisterFire(benchmark::State& state) {
    astra::SchedulerOptions opts{};
    opts.worker_count = 2;
    opts.metrics_level = astra::MetricsLevel::Off;
    astra::Scheduler sched(opts);

    auto coro_fn = [&](astra::Scheduler& s) -> astra::Task<int> {
        co_await astra::sleep_for(std::chrono::microseconds(50));
        co_return 2;
    };
    for (auto _ : state) {
        auto h = sched.spawn(coro_fn(sched));
        benchmark::DoNotOptimize(h.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TimerRegisterFire);

}  // namespace

BENCHMARK_MAIN();

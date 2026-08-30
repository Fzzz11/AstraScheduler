#include <astra/finalization.hpp>
#include <astra/scheduler.hpp>
#include <astra/trace.hpp>
#include <astra/trace_export.hpp>
#include <astra/version.hpp>

#include <chrono>
#include <stop_token>
#include <type_traits>
#include <utility>

static_assert(std::is_copy_constructible_v<astra::Scheduler>);
static_assert(std::is_move_constructible_v<astra::FrozenTaskGraph>);
static_assert(!std::is_copy_constructible_v<astra::FrozenTaskGraph>);
static_assert(std::is_copy_constructible_v<astra::TaskHandle<int>>);
static_assert(std::is_same_v<decltype(astra::header_version()), astra::Version>);
static_assert(std::is_same_v<decltype(astra::library_version()), astra::Version>);

astra::Task<int> public_coroutine_contract() {
    co_await astra::yield();
    co_return 7;
}

void public_source_contract() {
    astra::SchedulerOptions options{};
    options.worker_count = 2;
    astra::Scheduler scheduler(options);

    auto handle = scheduler.submit([](std::stop_token token) {
        astra::throw_if_stop_requested(token);
        return 42;
    });
    (void)handle.valid();
    (void)handle.task_id();
    (void)handle.priority();
    (void)handle.deadline();
    (void)handle.deadline_disposition();
    (void)handle.state();
    (void)handle.wait_for(std::chrono::milliseconds(1));
    handle.request_cancel();

    auto spawned = scheduler.spawn(public_coroutine_contract());
    auto attempted = scheduler.try_submit([] { return 1; });
    (void)spawned;
    (void)attempted;

    astra::TaskGraph builder;
    const astra::NodeId first = builder.emplace([] {});
    const astra::NodeId second = builder.emplace([] {});
    builder.add_edge(first, second, astra::EdgePolicy::RequireSuccess);
    auto frozen = std::move(builder).freeze();
    auto run = scheduler.run(std::move(frozen));
    (void)run.id();
    (void)run.node_count();
    (void)run.state();
    (void)run.is_completed();
    (void)run.wait_for(std::chrono::milliseconds(1));
    run.request_cancel();

    (void)scheduler.runtime_id();
    (void)scheduler.status();
    (void)scheduler.capabilities();
    (void)scheduler.metrics_snapshot();
    scheduler.shutdown();
}

#include <astra/capabilities.hpp>
#include <astra/coroutine.hpp>
#include <astra/error.hpp>
#include <astra/export.hpp>
#include <astra/graph.hpp>
#include <astra/id.hpp>
#include <astra/finalization.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>
#include <astra/status.hpp>
#include <astra/version.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <string_view>
#include <type_traits>

// R-093 / R-097 / R-098 / R-099 / R-100 / R-101 独立 consumer smoke（AST-003 / AST-004 / AST-005）：
// 验证安装的 CMake package 暴露完整的 public headers，版本契约成立，
// 且 SchedulerOptions、SchedulerStatus、强类型逻辑 ID、SchedulerCapabilities、
// 异常类型 scheduler_creation_rejected 均符合规范契约。
// 本模板由 tools/check_cmake_package.py 复制到仓库外构建运行。

// 编译期契约：三个版本查询均 noexcept；Version 可平凡复制；header_version()
// 可用于常量求值（D-164）。
static_assert(noexcept(astra::header_version()), "header_version() must be noexcept");
static_assert(noexcept(astra::library_version()), "library_version() must be noexcept");
static_assert(noexcept(astra::library_version_string()), "library_version_string() must be noexcept");
static_assert(std::is_trivially_copyable_v<astra::Version>, "Version must be trivially copyable");
constexpr astra::Version kHeaderVersion = astra::header_version();
constexpr bool kExpectedLocalDequeLockFree =
    std::atomic<std::uint64_t>::is_always_lock_free &&
    std::atomic<void*>::is_always_lock_free;
constexpr astra::LocalDequeBackend kExpectedLocalDequeBackend =
    kExpectedLocalDequeLockFree
        ? astra::LocalDequeBackend::ChaseLevLockFree
        : astra::LocalDequeBackend::Locked;

// 编译期契约：Version 经 defaulted operator<=> 可比较（含隐式 ==）。
static_assert(kHeaderVersion == astra::Version{ASTRA_VERSION_MAJOR, ASTRA_VERSION_MINOR, ASTRA_VERSION_PATCH});
static_assert(astra::Version{0u, 1u, 0u} < astra::Version{0u, 1u, 1u});
static_assert(astra::Version{0u, 2u, 0u} > astra::Version{0u, 1u, 9u});

// AST-004 编译期契约（R-098 / R-099 / R-100 / R-101）：
static_assert(noexcept(astra::recommended_worker_count()), "recommended_worker_count() must be noexcept");
static_assert(std::is_trivially_copyable_v<astra::SchedulerStatus>, "SchedulerStatus must be trivially copyable");
static_assert(std::is_trivially_copyable_v<astra::RuntimeId>, "RuntimeId must be trivially copyable");
static_assert(std::is_trivially_copyable_v<astra::TaskId>, "TaskId must be trivially copyable");
static_assert(std::is_trivially_copyable_v<astra::GraphRunId>, "GraphRunId must be trivially copyable");
static_assert(std::is_trivially_copyable_v<astra::NodeId>, "NodeId must be trivially copyable");
static_assert(std::is_trivially_copyable_v<astra::SchedulerCapabilities>, "SchedulerCapabilities must be trivially copyable");
static_assert(!std::is_aggregate_v<astra::SchedulerCapabilities>, "SchedulerCapabilities must not be aggregate");

inline astra::Task<int> consumer_coro_val(int val) {
    co_return val * 2;
}

inline astra::Task<void> consumer_coro_void() {
    co_return;
}

// AST-005 编译期契约（R-097）：
static_assert(std::is_base_of_v<std::runtime_error, astra::scheduler_creation_rejected>,
              "scheduler_creation_rejected must inherit from std::runtime_error");
static_assert(noexcept(std::declval<astra::scheduler_creation_rejected>().reason()),
              "scheduler_creation_rejected::reason() must be noexcept");

namespace {

// 读取 /proc/self/status 的 Threads: 行，返回当前进程线程数。
int thread_count() {
    std::FILE* status = std::fopen("/proc/self/status", "r");
    if (status == nullptr) {
        return -1;
    }
    char line[256];
    int threads = -1;
    while (std::fgets(line, sizeof(line), status) != nullptr) {
        if (std::sscanf(line, "Threads: %d", &threads) == 1) {
            break;
        }
    }
    std::fclose(status);
    return threads;
}

// 解析 "major.minor.patch"（纯数字与恰好两个点）并与三元组比较，
// 验证 library_version_string() 是与 library_version() 一致的规范文本。
bool triple_matches(std::string_view text, const astra::Version& version) {
    std::uint32_t parts[3] = {0u, 0u, 0u};
    int index = 0;
    std::uint32_t value = 0;
    bool digits = false;
    for (char c : text) {
        if (c == '.') {
            if (index >= 2 || !digits) {
                return false;
            }
            parts[index++] = value;
            value = 0;
            digits = false;
        } else if (c >= '0' && c <= '9') {
            value = value * 10u + static_cast<std::uint32_t>(c - '0');
            digits = true;
        } else {
            return false;
        }
    }
    if (index != 2 || !digits) {
        return false;
    }
    parts[2] = value;
    return parts[0] == version.major && parts[1] == version.minor && parts[2] == version.patch;
}

}  // namespace

int main() {
    // 1. 同一安装：header_version() == library_version() == ASTRA_VERSION_* 宏。
    const astra::Version library = astra::library_version();
    if (!(kHeaderVersion == library)) {
        std::printf("astra header/library version mismatch: header %u.%u.%u, library %u.%u.%u\n",
                    kHeaderVersion.major, kHeaderVersion.minor, kHeaderVersion.patch,
                    library.major, library.minor, library.patch);
        return 1;
    }
    if (library.major != ASTRA_VERSION_MAJOR || library.minor != ASTRA_VERSION_MINOR ||
        library.patch != ASTRA_VERSION_PATCH) {
        std::printf("astra library version does not match header macros\n");
        return 1;
    }

    // 2. 无副作用：查询前后进程线程数不变（不启动 Reaper/Worker 线程）。
    const int threads_before = thread_count();
    const astra::Version sink_version = astra::library_version();
    const auto sink_string = astra::library_version_string();
    const int threads_after = thread_count();
    if (threads_before < 0 || threads_after < 0) {
        std::printf("cannot read thread count from /proc/self/status\n");
        return 1;
    }
    if (threads_before != threads_after) {
        std::printf("version queries started threads: before=%d after=%d\n",
                    threads_before, threads_after);
        return 1;
    }

    // 3. string_view 指向进程期静态文本：两次调用地址与内容稳定，
    //    且文本三元组与 library_version() 一致（canonical SemVer）。
    const auto text_a = astra::library_version_string();
    const auto text_b = astra::library_version_string();
    if (text_a.data() != text_b.data() || text_a != text_b) {
        std::printf("library_version_string() is not stable across calls\n");
        return 1;
    }
    if (sink_version != library || !triple_matches(sink_string, library) ||
        !triple_matches(text_a, library)) {
        std::printf("library version string does not match the version triple\n");
        return 1;
    }

    // 4. AST-004 consumer contract:
    // R-098: SchedulerOptions
    astra::SchedulerOptions opts{};
    if (opts.worker_count < 1 || opts.external_pending_capacity != 65536 ||
        opts.external_backpressure != astra::ExternalBackpressure::Reject ||
        opts.max_helping_depth != 64 || opts.local_burst_limit != 64 ||
        opts.steal_probe_limit != 8 || opts.metrics_level != astra::MetricsLevel::Basic) {
        std::printf("SchedulerOptions defaults mismatch\n");
        return 1;
    }

    // R-100: Strong IDs
    astra::RuntimeId invalid_rid{};
    if (invalid_rid.valid()) {
        std::printf("Default RuntimeId must be invalid\n");
        return 1;
    }

    // R-099 & R-101: Scheduler instance, status and capabilities
    astra::Scheduler scheduler(opts);
    if (!scheduler.valid()) {
        std::printf("Scheduler should be valid\n");
        return 1;
    }
    if (!scheduler.runtime_id().valid()) {
        std::printf("Scheduler::runtime_id() must be valid\n");
        return 1;
    }
    const astra::SchedulerStatus status = scheduler.status();
    if (status.state != astra::SchedulerState::Running ||
        status.shutdown_mode != astra::ShutdownMode::None) {
        std::printf("Initial SchedulerStatus must be Running + None\n");
        return 1;
    }
    const astra::SchedulerCapabilities caps = scheduler.capabilities();
    if (caps.local_deque_backend() != kExpectedLocalDequeBackend ||
        caps.lock_free_local_deque() != kExpectedLocalDequeLockFree) {
        std::printf("SchedulerCapabilities must report the actual LocalDequeBackend\n");
        return 1;
    }

    // Empty scheduler behavior
    astra::Scheduler moved = std::move(scheduler);
    if (scheduler.valid() || scheduler.runtime_id().valid()) {
        std::printf("Moved-from Scheduler must be invalid\n");
        return 1;
    }
    bool logic_error_caught = false;
    try {
        (void)scheduler.status();
    } catch (const std::logic_error&) {
        logic_error_caught = true;
    }
    if (!logic_error_caught) {
        std::printf("scheduler.status() on empty Scheduler did not throw logic_error\n");
        return 1;
    }

    // 5. AST-005: scheduler_creation_rejected
    astra::scheduler_creation_rejected ex(astra::SchedulerCreationError::FinalizationStarted);
    if (ex.reason() != astra::SchedulerCreationError::FinalizationStarted) {
        std::printf("scheduler_creation_rejected::reason() mismatch\n");
        return 1;
    }
    const std::string_view err_msg = ex.what();
    if (err_msg.empty()) {
        std::printf("scheduler_creation_rejected::what() is empty\n");
        return 1;
    }

    // 6. AST-009 & AST-010: submit and try_submit
    astra::Scheduler s_active;
    auto h = s_active.submit([]() { return 42; });
    if (h.get() != 42) {
        std::printf("submit task execution failed\n");
        return 1;
    }

    auto try_res = s_active.try_submit([]() { return 100; });
    if (!std::holds_alternative<astra::TaskHandle<int>>(try_res)) {
        std::printf("try_submit failed\n");
        return 1;
    }
    if (std::get<astra::TaskHandle<int>>(try_res).get() != 100) {
        std::printf("try_submit result mismatch\n");
        return 1;
    }

    astra::submission_rejected sub_ex(astra::SubmissionError::CapacityExhausted);
    if (sub_ex.reason() != astra::SubmissionError::CapacityExhausted) {
        std::printf("submission_rejected::reason() mismatch\n");
        return 1;
    }

    // 7. AST-011: TaskState, wait, wait_for, and task_cancelled
    if (h.state() != astra::TaskState::Succeeded) {
        std::printf("h.state() must be Succeeded\n");
        return 1;
    }
    h.wait();
    if (h.wait_for(std::chrono::milliseconds(0)) != astra::WaitResult::Completed) {
        std::printf("h.wait_for() must be Completed\n");
        return 1;
    }

    std::atomic<bool> cancel_started{false};
    auto h_cancel = s_active.submit([&cancel_started](std::stop_token token) {
        cancel_started.store(true);
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        astra::throw_if_stop_requested(token);
    });
    while (!cancel_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    h_cancel.request_cancel();
    h_cancel.wait();
    if (h_cancel.state() != astra::TaskState::Cancelled) {
        std::printf("h_cancel.state() must be Cancelled\n");
        return 1;
    }
    bool caught_cancel = false;
    try {
        h_cancel.get();
    } catch (const astra::task_cancelled&) {
        caught_cancel = true;
    }
    if (!caught_cancel) {
        std::printf("h_cancel.get() did not throw task_cancelled\n");
        return 1;
    }

    // 8. AST-012: Helping Wait & helping_depth_exceeded
    astra::SchedulerOptions s_opt{};
    s_opt.worker_count = 1;
    s_opt.max_helping_depth = 2;
    astra::Scheduler s_help(s_opt);

    auto h_nested = s_help.submit([&s_help]() {
        auto c1 = s_help.submit([]() { return 100; });
        return c1.get() + 50;
    });
    if (h_nested.get() != 150) {
        std::printf("Helping wait nested get failed\n");
        return 1;
    }

    astra::helping_depth_exceeded depth_ex;
    std::string depth_msg = depth_ex.what();
    if (depth_msg.empty()) {
        std::printf("helping_depth_exceeded::what() is empty\n");
        return 1;
    }

    // 9. AST-013: Task cancellation pre-start
    std::promise<void> hold_p;
    std::shared_future<void> hold_f = hold_p.get_future().share();
    std::atomic<bool> hold_started{false};

    auto h_hold = s_help.submit([hold_f, &hold_started]() {
        hold_started.store(true);
        hold_f.wait();
    });
    while (!hold_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::atomic<int> pre_exec{0};
    auto h_pre = s_help.submit([&pre_exec]() {
        pre_exec.fetch_add(1);
        return 777;
    });
    h_pre.request_cancel();
    if (h_pre.state() != astra::TaskState::Cancelled) {
        std::printf("h_pre.state() must be Cancelled\n");
        return 1;
    }

    hold_p.set_value();
    h_hold.wait();
    h_pre.wait();
    if (pre_exec.load() != 0) {
        std::printf("pre-start cancelled task must not execute\n");
        return 1;
    }

    // 10. AST-014: Graceful Drain & Stopped Absorbing State
    s_help.shutdown();
    if (s_help.status().state != astra::SchedulerState::Stopped ||
        s_help.status().shutdown_mode != astra::ShutdownMode::Graceful) {
        std::printf("s_help must be Stopped Graceful\n");
        return 1;
    }
    // Repeat call must be safe no-op
    s_help.shutdown();
    s_help.shutdown_now();
    if (s_help.status().state != astra::SchedulerState::Stopped ||
        s_help.status().shutdown_mode != astra::ShutdownMode::Graceful) {
        std::printf("s_help must remain Stopped Graceful\n");
        return 1;
    }

    // 11. AST-015: Shutdown Guards (R-011 / R-013 / R-108)
    astra::Scheduler s_guard;
    std::atomic<bool> caught_self_shutdown{false};
    auto h_guard = s_guard.submit([&s_guard, &caught_self_shutdown]() {
        try {
            s_guard.shutdown();
        } catch (const std::logic_error&) {
            caught_self_shutdown.store(true);
        }
        return 1;
    });
    h_guard.wait();
    if (!caught_self_shutdown.load()) {
        std::printf("self-shutdown from worker must throw std::logic_error\n");
        return 1;
    }
    s_guard.shutdown();

    // 12. AST-016: Immediate Escalation & Unstarted Task Cancellation (R-014 / R-106)
    astra::SchedulerOptions s_esc_opt{};
    s_esc_opt.worker_count = 1;
    astra::Scheduler s_esc(s_esc_opt);

    std::promise<void> hold_esc_p;
    std::shared_future<void> hold_esc_f = hold_esc_p.get_future().share();
    std::atomic<bool> blocker_esc_started{false};

    auto h_esc_block = s_esc.submit([hold_esc_f, &blocker_esc_started]() {
        blocker_esc_started.store(true);
        hold_esc_f.wait();
    });
    while (!blocker_esc_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::atomic<int> esc_unstarted_count{0};
    auto h_esc_unstarted = s_esc.submit([&esc_unstarted_count]() {
        esc_unstarted_count.fetch_add(1);
        return 555;
    });

    std::thread th_esc([&s_esc]() {
        s_esc.shutdown_now();
    });
    while (s_esc.status().shutdown_mode != astra::ShutdownMode::Immediate) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    h_esc_unstarted.wait();
    if (h_esc_unstarted.state() != astra::TaskState::Cancelled) {
        std::printf("h_esc_unstarted must be Cancelled\n");
        return 1;
    }
    if (esc_unstarted_count.load() != 0) {
        std::printf("unstarted task under Immediate must not execute\n");
        return 1;
    }

    hold_esc_p.set_value();
    th_esc.join();

    // 13. AST-017: Last Handle RAII (R-103 / R-105)
    std::atomic<bool> raii_task_done{false};
    {
        astra::Scheduler s_raii1;
        {
            astra::Scheduler s_raii2 = s_raii1;
            try {
                auto h_raii = s_raii2.submit([&raii_task_done]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    raii_task_done.store(true);
                });
            } catch (const std::exception& e) {
                std::printf("s_raii2.submit threw: %s\n", e.what());
                return 1;
            }
        }
        // Non-last handle destruction does not trigger shutdown
        if (s_raii1.status().state != astra::SchedulerState::Running) {
            std::printf("s_raii1 must remain Running after s_raii2 destruction\n");
            return 1;
        }
    } // Last handle destroyed -> RAII graceful drain
    if (!raii_task_done.load()) {
        std::printf("RAII destructor must wait for tasks to complete (done=%d)\n", raii_task_done.load() ? 1 : 0);
        return 1;
    }

    // 14. AST-022: Local Deque & Ready Routing Precedence (R-063 / R-101)
    astra::Scheduler s_locked;
    if (s_locked.capabilities().local_deque_backend() != kExpectedLocalDequeBackend ||
        s_locked.capabilities().lock_free_local_deque() != kExpectedLocalDequeLockFree) {
        std::printf("Local deque capability must match the production backend\n");
        return 1;
    }
    auto h_ext = s_locked.submit([&] {
        auto h_int = s_locked.submit([] {});
        h_int.wait();
    });
    h_ext.wait();
    s_locked.shutdown();

    // 15. AST-023: Bounded non-repeating Steal Round (R-064)
    astra::SchedulerOptions s_steal_opt{};
    s_steal_opt.worker_count = 2;
    s_steal_opt.steal_probe_limit = 4;
    astra::Scheduler s_steal(s_steal_opt);
    std::atomic<bool> steal_done{false};
    auto h_s = s_steal.submit([&s_steal, &steal_done] {
        auto h_child = s_steal.submit([&steal_done] {
            steal_done.store(true);
        });
        h_child.wait();
    });
    h_s.wait();
    if (!steal_done.load()) {
        std::printf("AST-023 steal round task did not complete\n");
        return 1;
    }
    s_steal.shutdown();

    // 16. AST-024: No-lost-wakeup Park Handshake (R-065)
    astra::SchedulerOptions s_park_opt{};
    s_park_opt.worker_count = 2;
    astra::Scheduler s_park(s_park_opt);
    std::atomic<bool> park_task_done{false};
    auto h_park = s_park.submit([&park_task_done] {
        park_task_done.store(true);
    });
    h_park.wait();
    if (!park_task_done.load()) {
        std::printf("AST-024 park handshake task did not complete\n");
        return 1;
    }
    s_park.shutdown();

    // 17. AST-028: TaskGraph consuming freeze & validation (R-069)
    astra::TaskGraph tg;
    auto gn1 = tg.emplace([] {});
    auto gn2 = tg.emplace([] {});
    tg.add_edge(gn1, gn2);
    auto frozen_tg = std::move(tg).freeze();
    if (frozen_tg.node_count() != 2 || frozen_tg.edge_count() != 1 || frozen_tg.empty()) {
        std::printf("AST-028 FrozenTaskGraph node/edge count mismatch\n");
        return 1;
    }
    astra::TaskGraph tg_cycle;
    auto gc1 = tg_cycle.emplace([] {});
    auto gc2 = tg_cycle.emplace([] {});
    tg_cycle.add_edge(gc1, gc2);
    tg_cycle.add_edge(gc2, gc1);
    bool cycle_caught = false;
    try {
        (void)std::move(tg_cycle).freeze();
    } catch (const astra::graph_validation_error& ex) {
        if (ex.reason() == astra::GraphValidationError::Cycle && ex.cycle_witness().size() == 3) {
            cycle_caught = true;
        }
    }
    if (!cycle_caught) {
        std::printf("AST-028 TaskGraph cycle validation failed\n");
        return 1;
    }

    // 18. AST-029: GraphRun admission & dependency release (R-070)
    astra::Scheduler sched_graph;
    astra::TaskGraph tg_exec;
    std::atomic<int> g_n1_done{0};
    std::atomic<int> g_n2_done{0};
    auto eg1 = tg_exec.emplace([&] { g_n1_done.store(1); });
    auto eg2 = tg_exec.emplace([&] {
        if (g_n1_done.load() == 1) {
            g_n2_done.store(1);
        }
    });
    tg_exec.add_edge(eg1, eg2);
    auto gr = sched_graph.run(std::move(tg_exec).freeze());
    gr.wait();
    if (!gr.is_completed() || gr.state() != astra::GraphRunState::Succeeded ||
        g_n1_done.load() != 1 || g_n2_done.load() != 1) {
        std::printf("AST-029 GraphRun execution/dependency verification failed\n");
        return 1;
    }
    // Empty graph run
    auto gr_empty = sched_graph.run(astra::TaskGraph{}.freeze());
    if (!gr_empty.is_completed() || gr_empty.node_count() != 0 ||
        gr_empty.state() != astra::GraphRunState::Succeeded) {
        std::printf("AST-029 empty GraphRun failed\n");
        return 1;
    }
    sched_graph.shutdown();

    // 19. AST-030: void control graph & edge policies (R-071)
    astra::Scheduler sched_policy;
    astra::TaskGraph tg_policy;
    std::atomic<bool> p_cleanup_done{false};
    std::atomic<bool> p_req_done{false};
    auto pn_fail = tg_policy.emplace([] {
        throw std::runtime_error("consumer fail");
    });
    auto pn_req = tg_policy.emplace([&] {
        p_req_done.store(true);
    });
    auto pn_cleanup = tg_policy.emplace([&] {
        p_cleanup_done.store(true);
    });
    tg_policy.add_edge(pn_fail, pn_req, astra::EdgePolicy::RequireSuccess);
    tg_policy.add_edge(pn_fail, pn_cleanup, astra::EdgePolicy::AfterCompletion);
    auto gr_policy = sched_policy.run(std::move(tg_policy).freeze());
    gr_policy.wait();
    if (!gr_policy.is_completed() || gr_policy.state() != astra::GraphRunState::Failed ||
        p_req_done.load() || !p_cleanup_done.load()) {
        std::printf("AST-030 edge policy verification failed\n");
        return 1;
    }
    sched_policy.shutdown();

    // 20. AST-031: GraphRun cancel, report & caller wait (R-072)
    astra::GraphRun empty_run;
    if (empty_run.valid()) {
        std::printf("Default GraphRun must not be valid\n");
        return 1;
    }
    empty_run.request_cancel();

    astra::Scheduler sched_run_ctrl;
    astra::TaskGraph tg_ctrl;
    auto node_c1 = tg_ctrl.emplace([] {
        throw std::runtime_error("consumer error 1");
    });
    auto node_c2 = tg_ctrl.emplace([] {});
    auto gr_ctrl = sched_run_ctrl.run(std::move(tg_ctrl).freeze());
    auto wait_res = gr_ctrl.wait_for(std::chrono::seconds(2));
    if (wait_res != astra::GraphWaitResult::Completed || gr_ctrl.state() != astra::GraphRunState::Failed) {
        std::printf("AST-031 wait_for or state failed\n");
        return 1;
    }
    const auto& rep_ctrl = gr_ctrl.get_report();
    if (rep_ctrl.total_nodes != 2 || rep_ctrl.failed_nodes != 1 || rep_ctrl.succeeded_nodes != 1 ||
        rep_ctrl.failed_node_exceptions.empty() || rep_ctrl.failed_node_exceptions[0].first != node_c1) {
        std::printf("AST-031 get_report verification failed\n");
        return 1;
    }
    sched_run_ctrl.shutdown();

    // 21. AST-032: cold Coroutine Task & spawn (R-073)
    astra::Scheduler sched_coro;
    auto coro_t1 = consumer_coro_val(21);
    if (!coro_t1.valid()) {
        std::printf("Newly returned Task must be valid\n");
        return 1;
    }
    auto coro_h1 = sched_coro.spawn(std::move(coro_t1));
    if (coro_t1.valid() || !coro_h1.valid() || coro_h1.get() != 42) {
        std::printf("AST-032 spawn Task<int> failed\n");
        return 1;
    }

    auto coro_t2 = consumer_coro_void();
    auto coro_h2 = sched_coro.spawn(std::move(coro_t2));
    coro_h2.get();
    if (coro_h2.state() != astra::TaskState::Succeeded) {
        std::printf("AST-032 spawn Task<void> failed\n");
        return 1;
    }
    sched_coro.shutdown();

    // AST-033/034 的 AwaitHandshake 是实现协议，只由 internal tests 验证。
    // Public consumer 从 spawn/await/cancellation 的可观察结果验证同一语义。

    // 22. AST-035: Source-Runtime await & Restricted API Surface (R-076 / R-078 / D-120 / D-121 / D-122 / D-147 / D-125)
    static_assert(requires(astra::TaskHandle<int> h) {
        h.operator co_await();
    }, "TaskHandle must support operator co_await() const &");
    static_assert(requires(astra::GraphRun r) {
        r.operator co_await();
    }, "GraphRun must support operator co_await() const &");
    static_assert(noexcept(astra::cancellation_point()), "cancellation_point must be noexcept");
    static_assert(noexcept(astra::yield()), "yield must be noexcept");

    // 25. AST-018: FinalizationControl API (R-035 / R-036 / R-043 / R-044 / R-045 / R-046)
    static_assert(!std::is_default_constructible_v<astra::FinalizationControl>,
                  "FinalizationControl must not be default-constructible");
    static_assert(std::is_nothrow_copy_constructible_v<astra::FinalizationControl>,
                  "FinalizationControl must be noexcept copyable");
    static_assert(std::is_nothrow_move_constructible_v<astra::FinalizationControl>,
                  "FinalizationControl must be noexcept movable");
    static_assert(std::is_nothrow_destructible_v<astra::FinalizationControl>,
                  "FinalizationControl must be noexcept destructible");
    static_assert(noexcept(astra::begin_finalization()),
                  "begin_finalization must be noexcept");

    auto f_ctrl = astra::begin_finalization();
    auto f_ctrl2 = f_ctrl;
    f_ctrl2.request_immediate();
    auto f_res = f_ctrl2.wait_for(std::chrono::milliseconds(10));
    if (f_res != astra::FinalizationWaitResult::Completed) {
        std::printf("FinalizationWaitResult must be Completed\n");
        return 1;
    }
    f_ctrl.wait();

    // 20. AST-019: Finalization Begin & Startup Race (R-031 / R-037 / R-038 / R-104)
    bool new_sched_rejected = false;
    try {
        astra::Scheduler s_rejected;
    } catch (const astra::scheduler_creation_rejected& e) {
        if (e.reason() == astra::SchedulerCreationError::FinalizationStarted) {
            new_sched_rejected = true;
        }
    }
    if (!new_sched_rejected) {
        std::printf("Scheduler creation after begin_finalization must be rejected\n");
        return 1;
    }

    // 21. AST-020: Finalization Wait & wait_for (R-032 / R-033 / R-039 / R-040 / R-041 / R-042)
    auto f_res_zero = f_ctrl.wait_for(std::chrono::milliseconds(0));
    if (f_res_zero != astra::FinalizationWaitResult::Completed) {
        std::printf("wait_for(0) after completion must return Completed\n");
        return 1;
    }
    auto f_res_neg = f_ctrl.wait_for(std::chrono::milliseconds(-5));
    if (f_res_neg != astra::FinalizationWaitResult::Completed) {
        std::printf("wait_for(negative) after completion must return Completed\n");
        return 1;
    }
    f_ctrl2.wait();

    // 22. AST-021: Finalization Escalation (R-034 / R-047)
    static_assert(noexcept(f_ctrl.request_immediate()), "request_immediate must be noexcept");
    f_ctrl.request_immediate();
    f_ctrl2.request_immediate();
    if (f_ctrl.wait_for(std::chrono::milliseconds(0)) != astra::FinalizationWaitResult::Completed) {
        std::printf("request_immediate idempotence check failed\n");
        return 1;
    }

    return 0;
}

#include <astra/capabilities.hpp>
#include <astra/error.hpp>
#include <astra/export.hpp>
#include <astra/id.hpp>
#include <astra/finalization.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>
#include <astra/status.hpp>
#include <astra/version.hpp>

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
    if (caps.local_deque_backend() != astra::LocalDequeBackend::None ||
        caps.lock_free_local_deque() != false) {
        std::printf("v0.1.0 SchedulerCapabilities must report LocalDequeBackend::None\n");
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

    // 14. AST-018: FinalizationControl API (R-035 / R-036 / R-043 / R-044 / R-045 / R-046)
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

    // 15. AST-019: Finalization Begin & Startup Race (R-031 / R-037 / R-038 / R-104)
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

    return 0;
}

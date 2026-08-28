#include <astra/capabilities.hpp>
#include <astra/error.hpp>
#include <astra/export.hpp>
#include <astra/id.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>
#include <astra/status.hpp>
#include <astra/version.hpp>

#include <cstdint>
#include <cstdio>
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

    return 0;
}

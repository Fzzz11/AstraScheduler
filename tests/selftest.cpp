#include <astra/capabilities.hpp>
#include <astra/export.hpp>
#include <astra/id.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>
#include <astra/status.hpp>
#include <astra/version.hpp>

#include <type_traits>

// in-tree 测试入口：public header 可编译，库 target 可链接，
// 且 AST-003 / AST-004 契约在构建树内成立。

// 编译期契约：Version、ID、Options、Status、Capabilities
static_assert(noexcept(astra::header_version()), "header_version() must be noexcept");
static_assert(noexcept(astra::library_version()), "library_version() must be noexcept");
static_assert(noexcept(astra::library_version_string()), "library_version_string() must be noexcept");
static_assert(noexcept(astra::recommended_worker_count()), "recommended_worker_count() must be noexcept");

static_assert(std::is_trivially_copyable_v<astra::Version>, "Version must be trivially copyable");
static_assert(std::is_trivially_copyable_v<astra::RuntimeId>, "RuntimeId must be trivially copyable");
static_assert(std::is_trivially_copyable_v<astra::TaskId>, "TaskId must be trivially copyable");
static_assert(std::is_trivially_copyable_v<astra::GraphRunId>, "GraphRunId must be trivially copyable");
static_assert(std::is_trivially_copyable_v<astra::NodeId>, "NodeId must be trivially copyable");
static_assert(std::is_trivially_copyable_v<astra::SchedulerStatus>, "SchedulerStatus must be trivially copyable");
static_assert(std::is_trivially_copyable_v<astra::SchedulerCapabilities>, "SchedulerCapabilities must be trivially copyable");
static_assert(!std::is_aggregate_v<astra::SchedulerCapabilities>, "SchedulerCapabilities must not be aggregate");

namespace {
constexpr astra::Version kExpectedHeader{ASTRA_VERSION_MAJOR, ASTRA_VERSION_MINOR, ASTRA_VERSION_PATCH};
}  // namespace

int main() {
    constexpr astra::Version header = astra::header_version();
    if (!(header == kExpectedHeader) || !(header == astra::library_version())) {
        return 1;
    }

    astra::Scheduler s;
    if (!s.valid() || !s.runtime_id().valid()) {
        return 1;
    }
    if (s.status().state != astra::SchedulerState::Running) {
        return 1;
    }
    if (s.capabilities().local_deque_backend() != astra::LocalDequeBackend::None) {
        return 1;
    }
    return 0;
}

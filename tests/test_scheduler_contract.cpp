#include <astra/capabilities.hpp>
#include <astra/export.hpp>
#include <astra/id.hpp>
#include <astra/scheduler.hpp>
#include <astra/scheduler_options.hpp>
#include <astra/status.hpp>
#include <astra/version.hpp>

#include <atomic>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

// =============================================================================
// Compile-time assertions: Invariants for R-098, R-099, R-100, R-101
// =============================================================================

constexpr bool kExpectedLocalDequeLockFree =
    std::atomic<std::int64_t>::is_always_lock_free &&
    std::atomic<void*>::is_always_lock_free;
constexpr astra::LocalDequeBackend kExpectedLocalDequeBackend =
    kExpectedLocalDequeLockFree
        ? astra::LocalDequeBackend::ChaseLevLockFree
        : astra::LocalDequeBackend::Locked;

// R-098: recommended_worker_count() must be noexcept
static_assert(noexcept(astra::recommended_worker_count()),
              "[R-098] recommended_worker_count() must be noexcept");

// R-099: SchedulerStatus is trivially copyable
static_assert(std::is_trivially_copyable_v<astra::SchedulerStatus>,
              "[R-099] SchedulerStatus must be trivially copyable");
static_assert(std::is_standard_layout_v<astra::SchedulerStatus>,
              "[R-099] SchedulerStatus must be standard layout");

// R-100: Logical IDs must be trivially copyable, default-zero-invalid, no implicit conversions
static_assert(std::is_trivially_copyable_v<astra::RuntimeId>,
              "[R-100] RuntimeId must be trivially copyable");
static_assert(std::is_trivially_copyable_v<astra::TaskId>,
              "[R-100] TaskId must be trivially copyable");
static_assert(std::is_trivially_copyable_v<astra::GraphRunId>,
              "[R-100] GraphRunId must be trivially copyable");
static_assert(std::is_trivially_copyable_v<astra::NodeId>,
              "[R-100] NodeId must be trivially copyable");

// Type safety: no implicit conversions to/from integer/pointer
static_assert(!std::is_convertible_v<std::uint64_t, astra::RuntimeId>,
              "[R-100] RuntimeId must not allow implicit conversion from integer");
static_assert(!std::is_convertible_v<astra::RuntimeId, std::uint64_t>,
              "[R-100] RuntimeId must not allow implicit conversion to integer");
static_assert(!std::is_convertible_v<void*, astra::RuntimeId>,
              "[R-100] RuntimeId must not allow implicit conversion from pointer");
static_assert(!std::is_convertible_v<astra::RuntimeId, void*>,
              "[R-100] RuntimeId must not allow implicit conversion to pointer");

static_assert(!std::is_convertible_v<std::uint64_t, astra::TaskId>,
              "[R-100] TaskId must not allow implicit conversion from integer");
static_assert(!std::is_convertible_v<astra::TaskId, std::uint64_t>,
              "[R-100] TaskId must not allow implicit conversion to integer");

static_assert(!std::is_convertible_v<std::uint64_t, astra::GraphRunId>,
              "[R-100] GraphRunId must not allow implicit conversion from integer");
static_assert(!std::is_convertible_v<astra::GraphRunId, std::uint64_t>,
              "[R-100] GraphRunId must not allow implicit conversion to integer");

static_assert(!std::is_convertible_v<std::uint64_t, astra::NodeId>,
              "[R-100] NodeId must not allow implicit conversion from integer");
static_assert(!std::is_convertible_v<astra::NodeId, std::uint64_t>,
              "[R-100] NodeId must not allow implicit conversion to integer");

// R-101: SchedulerCapabilities is trivially copyable and NOT aggregate initializable
static_assert(std::is_trivially_copyable_v<astra::SchedulerCapabilities>,
              "[R-101] SchedulerCapabilities must be trivially copyable");
static_assert(!std::is_aggregate_v<astra::SchedulerCapabilities>,
              "[R-101] SchedulerCapabilities must not be aggregate-initializable by users");

// Check that Scheduler does not provide independent getters
template <typename T>
concept HasIsRunning = requires(const T& t) { t.is_running(); };
template <typename T>
concept HasIsStopped = requires(const T& t) { t.is_stopped(); };
template <typename T>
concept HasMode = requires(const T& t) { t.mode(); };

static_assert(!HasIsRunning<astra::Scheduler>,
              "[R-099] Scheduler must not provide independent is_running() getter");
static_assert(!HasIsStopped<astra::Scheduler>,
              "[R-099] Scheduler must not provide independent is_stopped() getter");
static_assert(!HasMode<astra::Scheduler>,
              "[R-099] Scheduler must not provide independent mode() getter");

// =============================================================================
// Helper macros
// =============================================================================

#define TEST_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond,     \
                         __FILE__, __LINE__);                                  \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

#define TEST_THROWS(expr, ExceptionType)                                       \
    do {                                                                       \
        bool caught = false;                                                   \
        try {                                                                  \
            (void)(expr);                                                      \
        } catch (const ExceptionType&) {                                       \
            caught = true;                                                     \
        } catch (...) {                                                        \
        }                                                                      \
        if (!caught) {                                                         \
            std::fprintf(stderr,                                               \
                         "Expected exception %s not thrown: %s at %s:%d\n",    \
                         #ExceptionType, #expr, __FILE__, __LINE__);           \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

// =============================================================================
// R-098: SchedulerOptions tests
// =============================================================================

void test_R098_options_defaults() {
    astra::SchedulerOptions options{};
    TEST_ASSERT(options.worker_count == astra::recommended_worker_count());
    TEST_ASSERT(options.external_pending_capacity == 65536);
    TEST_ASSERT(options.external_backpressure ==
                astra::ExternalBackpressure::Reject);
    TEST_ASSERT(options.max_helping_depth == 64);
    TEST_ASSERT(options.local_burst_limit == 64);
    TEST_ASSERT(options.steal_probe_limit == 8);
    TEST_ASSERT(options.metrics_level == astra::MetricsLevel::Basic);
    TEST_ASSERT(options.trace_collector == nullptr);

    // recommended_worker_count must always return at least 1
    std::size_t recommended = astra::recommended_worker_count();
    TEST_ASSERT(recommended >= 1);
}

void test_R098_options_validation_zero_sizes() {
    // worker_count == 0 must throw invalid_argument
    {
        astra::SchedulerOptions opt{};
        opt.worker_count = 0;
        TEST_THROWS(astra::Scheduler(opt), std::invalid_argument);
    }
    // external_pending_capacity == 0 must throw invalid_argument
    {
        astra::SchedulerOptions opt{};
        opt.external_pending_capacity = 0;
        TEST_THROWS(astra::Scheduler(opt), std::invalid_argument);
    }
    // max_helping_depth == 0 must throw invalid_argument
    {
        astra::SchedulerOptions opt{};
        opt.max_helping_depth = 0;
        TEST_THROWS(astra::Scheduler(opt), std::invalid_argument);
    }
    // local_burst_limit == 0 must throw invalid_argument
    {
        astra::SchedulerOptions opt{};
        opt.local_burst_limit = 0;
        TEST_THROWS(astra::Scheduler(opt), std::invalid_argument);
    }
    // steal_probe_limit == 0 must throw invalid_argument
    {
        astra::SchedulerOptions opt{};
        opt.steal_probe_limit = 0;
        TEST_THROWS(astra::Scheduler(opt), std::invalid_argument);
    }
}

void test_R098_options_validation_invalid_enums() {
    {
        astra::SchedulerOptions opt{};
        opt.external_backpressure =
            static_cast<astra::ExternalBackpressure>(999);
        TEST_THROWS(astra::Scheduler(opt), std::invalid_argument);
    }
    {
        astra::SchedulerOptions opt{};
        opt.metrics_level = static_cast<astra::MetricsLevel>(999);
        TEST_THROWS(astra::Scheduler(opt), std::invalid_argument);
    }
}

void test_R098_options_frozen_after_construction() {
    astra::SchedulerOptions opt{};
    opt.worker_count = 4;
    opt.external_pending_capacity = 1024;
    astra::Scheduler s(opt);

    // Caller modifies original options after construction
    opt.worker_count = 0;
    opt.external_pending_capacity = 0;

    // Scheduler instance remains valid and operational
    TEST_ASSERT(s.valid());
    TEST_ASSERT(s.runtime_id().valid());
    TEST_ASSERT(s.status().state == astra::SchedulerState::Running);
}

// =============================================================================
// R-099: Scheduler status tests
// =============================================================================

void test_R099_status_paired_snapshot() {
    astra::Scheduler s;
    TEST_ASSERT(s.valid());

    astra::SchedulerStatus status = s.status();
    // Valid combination upon construction: Running + None
    TEST_ASSERT(status.state == astra::SchedulerState::Running);
    TEST_ASSERT(status.shutdown_mode == astra::ShutdownMode::None);

    // Verify comparison works
    astra::SchedulerStatus expected{astra::SchedulerState::Running,
                                    astra::ShutdownMode::None};
    TEST_ASSERT(status == expected);
}

void test_R099_empty_scheduler_throws_logic_error() {
    astra::Scheduler s1;
    TEST_ASSERT(s1.valid());

    // Move construct: s1 becomes empty
    astra::Scheduler s2 = std::move(s1);
    TEST_ASSERT(!s1.valid());
    TEST_ASSERT(s2.valid());

    // s1.status() must throw logic_error
    TEST_THROWS(s1.status(), std::logic_error);

    // s2.status() must succeed
    TEST_ASSERT(s2.status().state == astra::SchedulerState::Running);
}

void test_R099_legal_pairs_enumeration() {
    // Only legal pairs:
    // Running + None
    // Stopping + Graceful
    // Stopping + Immediate
    // Stopped + Graceful
    // Stopped + Immediate
    auto is_legal_pair = [](astra::SchedulerStatus st) -> bool {
        if (st.state == astra::SchedulerState::Running) {
            return st.shutdown_mode == astra::ShutdownMode::None;
        }
        if (st.state == astra::SchedulerState::Stopping ||
            st.state == astra::SchedulerState::Stopped) {
            return st.shutdown_mode == astra::ShutdownMode::Graceful ||
                   st.shutdown_mode == astra::ShutdownMode::Immediate;
        }
        return false;
    };

    TEST_ASSERT(is_legal_pair({astra::SchedulerState::Running, astra::ShutdownMode::None}));
    TEST_ASSERT(is_legal_pair({astra::SchedulerState::Stopping, astra::ShutdownMode::Graceful}));
    TEST_ASSERT(is_legal_pair({astra::SchedulerState::Stopping, astra::ShutdownMode::Immediate}));
    TEST_ASSERT(is_legal_pair({astra::SchedulerState::Stopped, astra::ShutdownMode::Graceful}));
    TEST_ASSERT(is_legal_pair({astra::SchedulerState::Stopped, astra::ShutdownMode::Immediate}));

    // Illegal pairs
    TEST_ASSERT(!is_legal_pair({astra::SchedulerState::Running, astra::ShutdownMode::Graceful}));
    TEST_ASSERT(!is_legal_pair({astra::SchedulerState::Running, astra::ShutdownMode::Immediate}));
    TEST_ASSERT(!is_legal_pair({astra::SchedulerState::Stopping, astra::ShutdownMode::None}));
    TEST_ASSERT(!is_legal_pair({astra::SchedulerState::Stopped, astra::ShutdownMode::None}));
}

void test_R099_concurrent_status_reads() {
    astra::Scheduler s;
    std::vector<std::thread> readers;
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> read_count{0};

    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                astra::SchedulerStatus st = s.status();
                TEST_ASSERT(st.state == astra::SchedulerState::Running);
                TEST_ASSERT(st.shutdown_mode == astra::ShutdownMode::None);
                read_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : readers) {
        t.join();
    }
    TEST_ASSERT(read_count.load() > 0);
}

// =============================================================================
// R-100: Strong logical ID tests
// =============================================================================

void test_R100_strong_logical_ids_properties() {
    // Default-zero-invalid
    astra::RuntimeId default_runtime{};
    TEST_ASSERT(!default_runtime.valid());
    TEST_ASSERT(!static_cast<bool>(default_runtime));
    TEST_ASSERT(default_runtime.value() == 0);

    astra::TaskId default_task{};
    TEST_ASSERT(!default_task.valid());
    TEST_ASSERT(!static_cast<bool>(default_task));
    TEST_ASSERT(default_task.sequence() == 0);
    TEST_ASSERT(!default_task.runtime_id().valid());

    astra::GraphRunId default_graph{};
    TEST_ASSERT(!default_graph.valid());
    TEST_ASSERT(!static_cast<bool>(default_graph));
    TEST_ASSERT(default_graph.sequence() == 0);
    TEST_ASSERT(!default_graph.runtime_id().valid());

    astra::NodeId default_node{};
    TEST_ASSERT(!default_node.valid());
    TEST_ASSERT(!static_cast<bool>(default_node));
    TEST_ASSERT(default_node.value() == 0);

    // Non-zero values are valid
    astra::RuntimeId valid_runtime{100};
    TEST_ASSERT(valid_runtime.valid());
    TEST_ASSERT(static_cast<bool>(valid_runtime));
    TEST_ASSERT(valid_runtime.value() == 100);

    astra::TaskId valid_task{valid_runtime, 1};
    TEST_ASSERT(valid_task.valid());
    TEST_ASSERT(valid_task.runtime_id() == valid_runtime);
    TEST_ASSERT(valid_task.sequence() == 1);

    // TaskId with invalid RuntimeId is invalid even if sequence != 0
    astra::TaskId task_invalid_runtime{default_runtime, 1};
    TEST_ASSERT(!task_invalid_runtime.valid());

    // TaskId with valid RuntimeId but sequence 0 is invalid
    astra::TaskId task_zero_seq{valid_runtime, 0};
    TEST_ASSERT(!task_zero_seq.valid());

    astra::GraphRunId valid_graph{valid_runtime, 2};
    TEST_ASSERT(valid_graph.valid());
    TEST_ASSERT(valid_graph.runtime_id() == valid_runtime);
    TEST_ASSERT(valid_graph.sequence() == 2);

    astra::NodeId valid_node{42};
    TEST_ASSERT(valid_node.valid());
    TEST_ASSERT(valid_node.value() == 42);
}

void test_R100_comparisons_and_hash() {
    astra::RuntimeId r1{1}, r2{2}, r1_dup{1};
    TEST_ASSERT(r1 == r1_dup);
    TEST_ASSERT(r1 != r2);
    TEST_ASSERT(r1 < r2);
    TEST_ASSERT(r2 > r1);

    astra::TaskId t1{r1, 10}, t2{r1, 20}, t3{r2, 10};
    TEST_ASSERT(t1 < t2);
    TEST_ASSERT(t1 < t3);
    TEST_ASSERT(t1 == (astra::TaskId{r1, 10}));

    astra::NodeId n1{5}, n2{10};
    TEST_ASSERT(n1 < n2);
    TEST_ASSERT(n1 == (astra::NodeId{5}));

    // std::hash support
    std::unordered_set<astra::RuntimeId> r_set;
    r_set.insert(r1);
    r_set.insert(r2);
    TEST_ASSERT(r_set.size() == 2);
    TEST_ASSERT(r_set.contains(r1_dup));

    std::unordered_set<astra::TaskId> t_set;
    t_set.insert(t1);
    t_set.insert(t2);
    TEST_ASSERT(t_set.size() == 2);
    TEST_ASSERT(t_set.contains(astra::TaskId{r1, 10}));

    std::unordered_set<astra::NodeId> n_set;
    n_set.insert(n1);
    n_set.insert(n2);
    TEST_ASSERT(n_set.size() == 2);
    TEST_ASSERT(n_set.contains(astra::NodeId{5}));
}

void test_R100_scheduler_runtime_id() {
    astra::Scheduler s1;
    astra::Scheduler s2;
    TEST_ASSERT(s1.runtime_id().valid());
    TEST_ASSERT(s2.runtime_id().valid());
    TEST_ASSERT(s1.runtime_id() != s2.runtime_id());
    TEST_ASSERT(s1.runtime_id() < s2.runtime_id());

    // Copy shares the same RuntimeId
    astra::Scheduler s1_copy = s1;
    TEST_ASSERT(s1_copy.runtime_id() == s1.runtime_id());

    // Move empties the source
    astra::Scheduler s1_moved = std::move(s1);
    TEST_ASSERT(!s1.valid());
    TEST_ASSERT(!s1.runtime_id().valid());
    TEST_ASSERT(s1.runtime_id() == astra::RuntimeId{});
    TEST_ASSERT(s1_moved.runtime_id() == s1_copy.runtime_id());
}

// =============================================================================
// R-101: SchedulerCapabilities tests
// =============================================================================

void test_R101_capabilities() {
    astra::Scheduler s;
    TEST_ASSERT(s.valid());

    astra::SchedulerCapabilities caps = s.capabilities();
    TEST_ASSERT(caps.local_deque_backend() == kExpectedLocalDequeBackend);
    TEST_ASSERT(caps.lock_free_local_deque() == kExpectedLocalDequeLockFree);

    // Capabilities mappings
    astra::SchedulerCapabilities caps_none(astra::LocalDequeBackend::None);
    TEST_ASSERT(caps_none.local_deque_backend() == astra::LocalDequeBackend::None);
    TEST_ASSERT(caps_none.lock_free_local_deque() == false);

    astra::SchedulerCapabilities caps_locked(astra::LocalDequeBackend::Locked);
    TEST_ASSERT(caps_locked.local_deque_backend() == astra::LocalDequeBackend::Locked);
    TEST_ASSERT(caps_locked.lock_free_local_deque() == false);

    astra::SchedulerCapabilities caps_lockfree(astra::LocalDequeBackend::ChaseLevLockFree);
    TEST_ASSERT(caps_lockfree.local_deque_backend() == astra::LocalDequeBackend::ChaseLevLockFree);
    TEST_ASSERT(caps_lockfree.lock_free_local_deque() == true);

    // Empty scheduler capabilities throws logic_error
    astra::Scheduler moved = std::move(s);
    TEST_THROWS(s.capabilities(), std::logic_error);
    TEST_ASSERT(moved.capabilities().local_deque_backend() == kExpectedLocalDequeBackend);
}

// =============================================================================
// Main runner
// =============================================================================

int main() {
    std::printf("[AST-004 Test Suite Starting]\n");

    // R-098
    test_R098_options_defaults();
    test_R098_options_validation_zero_sizes();
    test_R098_options_validation_invalid_enums();
    test_R098_options_frozen_after_construction();
    std::printf("  [PASS] R-098 SchedulerOptions policy & validation\n");

    // R-099
    test_R099_status_paired_snapshot();
    test_R099_empty_scheduler_throws_logic_error();
    test_R099_legal_pairs_enumeration();
    test_R099_concurrent_status_reads();
    std::printf("  [PASS] R-099 Scheduler status paired snapshot\n");

    // R-100
    test_R100_strong_logical_ids_properties();
    test_R100_comparisons_and_hash();
    test_R100_scheduler_runtime_id();
    std::printf("  [PASS] R-100 Strong logical IDs & monotonic sequence\n");

    // R-101
    test_R101_capabilities();
    std::printf("  [PASS] R-101 SchedulerCapabilities & backend reporting\n");

    std::printf("[AST-004 Test Suite All Passed]\n");
    return 0;
}

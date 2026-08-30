#include "graph/graph_runtime_port.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

#define TEST_ASSERT(condition)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::fprintf(stderr, "Assertion failed: %s at %s:%d\n",          \
                         #condition, __FILE__, __LINE__);                     \
            std::abort();                                                     \
        }                                                                     \
    } while (false)

namespace {

class FakeGraphRuntime final : public astra::detail::GraphRuntimePort {
public:
    astra::RuntimeId runtime_identity() const noexcept override {
        return astra::RuntimeId{1};
    }
    astra::TaskId allocate_graph_task_id() override { return {}; }
    astra::GraphRunId allocate_graph_run_id() override { return {}; }
    astra::detail::AdmissionDecision acquire_graph_slots(
        std::size_t, bool, bool) override {
        return astra::detail::AdmissionDecision::Success;
    }
    void release_graph_slots(std::size_t count) noexcept override {
        released_slots += count;
    }
    void post_graph_task(
        std::unique_ptr<astra::detail::TaskInvokerBase>, bool) override {}
    std::uint64_t register_graph_timer(
        std::chrono::steady_clock::time_point,
        std::shared_ptr<astra::detail::AwaitHandshake>,
        std::function<void()>) override {
        return 0;
    }
    void cancel_graph_timer(std::uint64_t) noexcept override {}
    void record_graph_admission_attempt() noexcept override {}
    void record_graph_rejected() noexcept override {}
    void record_graph_started() noexcept override {}
    void rollback_graph_started(std::size_t task_count) noexcept override {
        ++rollbacks;
        rolled_back_tasks += task_count;
    }

    std::size_t released_slots{0};
    std::size_t rollbacks{0};
    std::size_t rolled_back_tasks{0};
};

void test_external_lease_rolls_back_both_resources() {
    FakeGraphRuntime runtime;
    {
        astra::detail::GraphAdmissionLease lease(runtime, 4, false);
    }
    TEST_ASSERT(runtime.rollbacks == 1);
    TEST_ASSERT(runtime.rolled_back_tasks == 4);
    TEST_ASSERT(runtime.released_slots == 4);
}

void test_internal_lease_does_not_release_external_slots() {
    FakeGraphRuntime runtime;
    {
        astra::detail::GraphAdmissionLease lease(runtime, 4, true);
    }
    TEST_ASSERT(runtime.rollbacks == 1);
    TEST_ASSERT(runtime.rolled_back_tasks == 4);
    TEST_ASSERT(runtime.released_slots == 0);
}

void test_committed_lease_transfers_ownership() {
    FakeGraphRuntime runtime;
    {
        astra::detail::GraphAdmissionLease lease(runtime, 4, false);
        lease.commit();
    }
    TEST_ASSERT(runtime.rollbacks == 0);
    TEST_ASSERT(runtime.rolled_back_tasks == 0);
    TEST_ASSERT(runtime.released_slots == 0);
}

}  // namespace

int main() {
    test_external_lease_rolls_back_both_resources();
    test_internal_lease_does_not_release_external_slots();
    test_committed_lease_transfers_ownership();
    return 0;
}

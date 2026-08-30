#ifndef ASTRA_SRC_RUNTIME_IDENTITY_HPP
#define ASTRA_SRC_RUNTIME_IDENTITY_HPP

#include <astra/id.hpp>

#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace astra::detail {

// 单个 Runtime 的逻辑身份分配器。序列只增不减，耗尽时拒绝分配，永不 wrap/reuse。
class RuntimeIdentityAllocator final {
public:
    RuntimeIdentityAllocator() = default;
    RuntimeIdentityAllocator(const RuntimeIdentityAllocator&) = delete;
    RuntimeIdentityAllocator& operator=(const RuntimeIdentityAllocator&) = delete;

    [[nodiscard]] TaskId allocate_task(RuntimeId runtime_id) {
        return TaskId{runtime_id, allocate_sequence(task_sequence_, "TaskId")};
    }

    [[nodiscard]] GraphRunId allocate_graph_run(RuntimeId runtime_id) {
        return GraphRunId{runtime_id, allocate_sequence(graph_run_sequence_, "GraphRunId")};
    }

private:
    static std::uint64_t allocate_sequence(
        std::atomic<std::uint64_t>& sequence,
        const char* identity_name) {
        std::uint64_t current = sequence.load(std::memory_order_relaxed);
        while (current != std::numeric_limits<std::uint64_t>::max()) {
            if (sequence.compare_exchange_weak(
                    current, current + 1, std::memory_order_relaxed)) {
                return current + 1;
            }
        }
        throw std::overflow_error(std::string(identity_name) + " sequence exhausted");
    }

    std::atomic<std::uint64_t> task_sequence_{0};
    std::atomic<std::uint64_t> graph_run_sequence_{0};
};

}  // namespace astra::detail

#endif  // ASTRA_SRC_RUNTIME_IDENTITY_HPP

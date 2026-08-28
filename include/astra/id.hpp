#ifndef ASTRA_ID_HPP
#define ASTRA_ID_HPP

// AstraScheduler 强类型逻辑 ID（AST-004 / R-100 / D-153 / D-161）。
// RuntimeId、TaskId、GraphRunId 与 NodeId 均为 default-zero-invalid、
// trivially-copyable 强值类型，支持 valid/equality/order/hash，
// 且无隐式整数或指针转换。
// Supported Configuration 仅 64-bit Linux（R-111，经 export.hpp 检查）。

#include <astra/export.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace astra {

// 进程内单次 Runtime 实例的唯一强标识（D-153）。
class RuntimeId {
public:
    constexpr RuntimeId() noexcept = default;
    constexpr explicit RuntimeId(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

    friend constexpr auto operator<=>(RuntimeId, RuntimeId) = default;
    friend constexpr bool operator==(RuntimeId, RuntimeId) = default;

private:
    std::uint64_t value_{0};
};

// 单个 Task 的强类型全局逻辑标识（D-153：RuntimeId + 64-bit sequence）。
class TaskId {
public:
    constexpr TaskId() noexcept = default;
    constexpr explicit TaskId(RuntimeId runtime_id, std::uint64_t sequence) noexcept
        : runtime_id_(runtime_id), sequence_(sequence) {}

    [[nodiscard]] constexpr bool valid() const noexcept {
        return runtime_id_.valid() && sequence_ != 0;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    [[nodiscard]] constexpr RuntimeId runtime_id() const noexcept { return runtime_id_; }
    [[nodiscard]] constexpr std::uint64_t sequence() const noexcept { return sequence_; }

    friend constexpr auto operator<=>(const TaskId&, const TaskId&) = default;
    friend constexpr bool operator==(const TaskId&, const TaskId&) = default;

private:
    RuntimeId runtime_id_{};
    std::uint64_t sequence_{0};
};

// 单次 Graph（任务图：一组有依赖关系的 Task） 运行的强类型全局逻辑标识（D-153：RuntimeId + 64-bit sequence）。
class GraphRunId {
public:
    constexpr GraphRunId() noexcept = default;
    constexpr explicit GraphRunId(RuntimeId runtime_id, std::uint64_t sequence) noexcept
        : runtime_id_(runtime_id), sequence_(sequence) {}

    [[nodiscard]] constexpr bool valid() const noexcept {
        return runtime_id_.valid() && sequence_ != 0;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    [[nodiscard]] constexpr RuntimeId runtime_id() const noexcept { return runtime_id_; }
    [[nodiscard]] constexpr std::uint64_t sequence() const noexcept { return sequence_; }

    friend constexpr auto operator<=>(const GraphRunId&, const GraphRunId&) = default;
    friend constexpr bool operator==(const GraphRunId&, const GraphRunId&) = default;

private:
    RuntimeId runtime_id_{};
    std::uint64_t sequence_{0};
};

// Graph 定义内按插入顺序分配的 graph-local 强类型标识（D-161）。
class NodeId {
public:
    constexpr NodeId() noexcept = default;
    constexpr explicit NodeId(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

    friend constexpr auto operator<=>(NodeId, NodeId) = default;
    friend constexpr bool operator==(NodeId, NodeId) = default;

private:
    std::uint64_t value_{0};
};

}  // namespace astra

// std::hash 模板特化
template <>
struct std::hash<astra::RuntimeId> {
    std::size_t operator()(const astra::RuntimeId& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value());
    }
};

template <>
struct std::hash<astra::TaskId> {
    std::size_t operator()(const astra::TaskId& id) const noexcept {
        const std::size_t h1 = std::hash<astra::RuntimeId>{}(id.runtime_id());
        const std::size_t h2 = std::hash<std::uint64_t>{}(id.sequence());
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

template <>
struct std::hash<astra::GraphRunId> {
    std::size_t operator()(const astra::GraphRunId& id) const noexcept {
        const std::size_t h1 = std::hash<astra::RuntimeId>{}(id.runtime_id());
        const std::size_t h2 = std::hash<std::uint64_t>{}(id.sequence());
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

template <>
struct std::hash<astra::NodeId> {
    std::size_t operator()(const astra::NodeId& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value());
    }
};

#endif  // ASTRA_ID_HPP

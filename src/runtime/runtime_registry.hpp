#ifndef ASTRA_SRC_RUNTIME_REGISTRY_HPP
#define ASTRA_SRC_RUNTIME_REGISTRY_HPP

#include <astra/id.hpp>

namespace astra::detail {

class RuntimeDiagnostics;

// 仅为跨模块指标/等待路由保存非拥有指针；Runtime 在析构前主动注销。
void register_runtime_instance(RuntimeId runtime_id, RuntimeDiagnostics* diagnostics);
void unregister_runtime_instance(RuntimeId runtime_id) noexcept;
[[nodiscard]] RuntimeDiagnostics* find_runtime_instance(RuntimeId runtime_id) noexcept;

}  // namespace astra::detail

#endif  // ASTRA_SRC_RUNTIME_REGISTRY_HPP

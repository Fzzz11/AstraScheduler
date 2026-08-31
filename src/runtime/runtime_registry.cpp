#include "runtime_registry.hpp"
#include "runtime_diagnostics.hpp"

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace astra::detail {
namespace {

std::mutex runtime_registry_mutex;
std::unordered_map<std::uint64_t, RuntimeDiagnostics*> runtime_registry;

}  // namespace

void register_runtime_instance(RuntimeId runtime_id, RuntimeDiagnostics* diagnostics) {
    std::lock_guard<std::mutex> lock(runtime_registry_mutex);
    runtime_registry[runtime_id.value()] = diagnostics;
}

void unregister_runtime_instance(RuntimeId runtime_id) noexcept {
    std::lock_guard<std::mutex> lock(runtime_registry_mutex);
    runtime_registry.erase(runtime_id.value());
}

RuntimeDiagnostics* find_runtime_instance(RuntimeId runtime_id) noexcept {
    std::lock_guard<std::mutex> lock(runtime_registry_mutex);
    const auto iterator = runtime_registry.find(runtime_id.value());
    return iterator == runtime_registry.end() ? nullptr : iterator->second;
}

}  // namespace astra::detail

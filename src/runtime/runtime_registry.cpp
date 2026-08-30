#include "runtime_registry.hpp"

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace astra::detail {
namespace {

std::mutex runtime_registry_mutex;
std::unordered_map<std::uint64_t, void*> runtime_registry;

}  // namespace

void register_runtime_instance(RuntimeId runtime_id, void* instance) {
    std::lock_guard<std::mutex> lock(runtime_registry_mutex);
    runtime_registry[runtime_id.value()] = instance;
}

void unregister_runtime_instance(RuntimeId runtime_id) noexcept {
    std::lock_guard<std::mutex> lock(runtime_registry_mutex);
    runtime_registry.erase(runtime_id.value());
}

void* find_runtime_instance(RuntimeId runtime_id) noexcept {
    std::lock_guard<std::mutex> lock(runtime_registry_mutex);
    const auto iterator = runtime_registry.find(runtime_id.value());
    return iterator == runtime_registry.end() ? nullptr : iterator->second;
}

}  // namespace astra::detail

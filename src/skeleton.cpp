#include <astra/export.hpp>

// Phase 0 编译库骨架（AST-002）：不实现任何 Runtime 调度行为。
// 以下两个符号是 package 符号可见性机制的私有测试 seam（R-110：
// public symbol 经 export macro 控制、internal symbol hidden），
// 不是 public API，不由任何 public header 声明，后续 Ticket 可移除。

namespace astra {
namespace detail {

ASTRA_NO_EXPORT int package_probe() noexcept { return 0; }

ASTRA_EXPORT int package_probe_exported() noexcept { return 1; }

}  // namespace detail
}  // namespace astra

#include <astra/version.hpp>

// AST-003 in-tree 测试入口：public header 可编译，库 target 可链接，
// 且 R-093 版本契约在构建树内成立（installed consumer 侧证据由
// tools/check_cmake_package.py 的独立 consumer 提供）。

// 编译期契约：noexcept、可平凡复制、header_version() 可常量求值、
// defaulted operator<=> 提供全序比较。
static_assert(noexcept(astra::header_version()), "header_version() must be noexcept");
static_assert(noexcept(astra::library_version()), "library_version() must be noexcept");
static_assert(noexcept(astra::library_version_string()), "library_version_string() must be noexcept");
static_assert(astra::Version{0u, 1u, 0u} <= astra::Version{0u, 1u, 0u}, "Version must be ordered");
static_assert(astra::Version{1u, 0u, 0u} != astra::Version{0u, 9u, 9u}, "Version must be comparable");

namespace {
constexpr astra::Version kExpectedHeader{ASTRA_VERSION_MAJOR, ASTRA_VERSION_MINOR, ASTRA_VERSION_PATCH};
}  // namespace

int main() {
    constexpr astra::Version header = astra::header_version();
    if (!(header == kExpectedHeader) || !(header == astra::library_version())) {
        return 1;
    }
    return 0;
}

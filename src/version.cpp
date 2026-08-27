#include <astra/version.hpp>

// AST-003 版本查询的 compiled library 实现（R-093 / D-164）。
// 三元组与规范 SemVer 文本都源自 cmake/version.hpp.in 生成的同一组
// 版本宏（单一版本源：CMake project VERSION）；三个查询不分配、不
// 加锁、不启动任何线程或服务。

// 宏字符串化：十进制字面量经一次展开后转为文本（0.1.0 形如 "0.1.0"）。
#define ASTRA_DETAIL_STR_IMPL(x) #x
#define ASTRA_DETAIL_STR(x) ASTRA_DETAIL_STR_IMPL(x)

namespace astra {
namespace {

// 进程期静态只读存储（internal linkage，不进入动态符号表）：内容与
// 地址在进程生命周期内稳定，string_view 指向它且调用方不得释放。
constexpr char kLibraryVersionString[] =
    ASTRA_DETAIL_STR(ASTRA_VERSION_MAJOR) "."
    ASTRA_DETAIL_STR(ASTRA_VERSION_MINOR) "."
    ASTRA_DETAIL_STR(ASTRA_VERSION_PATCH);

}  // namespace

Version library_version() noexcept {
    return Version{ASTRA_VERSION_MAJOR, ASTRA_VERSION_MINOR, ASTRA_VERSION_PATCH};
}

std::string_view library_version_string() noexcept {
    // 显式长度减去字符串字面量的 NUL；string_view 不包含终止符。
    return std::string_view{kLibraryVersionString, sizeof(kLibraryVersionString) - 1};
}

}  // namespace astra

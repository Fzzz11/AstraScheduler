#include <astra/version.hpp>

#include <cstdio>

// R-093 手工链接（绕过 CMake exact-version 边界）时的 mismatch 诊断模板：
// 比较编译期 header 版本与实际链接的 library 版本，不一致即打印诊断
// 并以非零退出。该模板由 tools/check_cmake_package.py 与一份篡改了
// ASTRA_VERSION_* 宏的 header 副本一起编译，模拟错误 header/binary
// 组合并验证其可被发现（R-093 例外边界：可诊断不等于受支持）。
// 本文件不参与 tests/ 的 in-tree CMake 构建，仅供该 gate 手工编译。

int main() {
    const astra::Version header = astra::header_version();
    const astra::Version library = astra::library_version();
    if (!(header == library)) {
        std::printf("astra header/library version mismatch: header %u.%u.%u, library %u.%u.%u\n",
                    header.major, header.minor, header.patch,
                    library.major, library.minor, library.patch);
        return 1;
    }
    return 0;
}

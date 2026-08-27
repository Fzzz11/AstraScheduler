#include <astra/version.hpp>

#include <cstdint>
#include <cstdio>
#include <type_traits>

// R-093 独立 consumer smoke（AST-003）：验证同一安装的 header/library
// 版本一致，且版本查询无副作用——不启动 Runtime/Reaper/Worker 线程、
// 不分配（返回类型均为非拥有值）、string_view 指向进程期静态规范
// SemVer 文本。任何失败都打印诊断并以非零退出。
// 本模板由 tools/check_cmake_package.py 复制到仓库外构建运行。

// 编译期契约：三个查询均 noexcept；Version 可平凡复制；header_version()
// 可用于常量求值（D-164）。
static_assert(noexcept(astra::header_version()), "header_version() must be noexcept");
static_assert(noexcept(astra::library_version()), "library_version() must be noexcept");
static_assert(noexcept(astra::library_version_string()), "library_version_string() must be noexcept");
static_assert(std::is_trivially_copyable_v<astra::Version>, "Version must be trivially copyable");
constexpr astra::Version kHeaderVersion = astra::header_version();

// 编译期契约：Version 经 defaulted operator<=> 可比较（含隐式 ==）。
static_assert(kHeaderVersion == astra::Version{ASTRA_VERSION_MAJOR, ASTRA_VERSION_MINOR, ASTRA_VERSION_PATCH});
static_assert(astra::Version{0u, 1u, 0u} < astra::Version{0u, 1u, 1u});
static_assert(astra::Version{0u, 2u, 0u} > astra::Version{0u, 1u, 9u});

namespace {

// 读取 /proc/self/status 的 Threads: 行，返回当前进程线程数。
int thread_count() {
    std::FILE* status = std::fopen("/proc/self/status", "r");
    if (status == nullptr) {
        return -1;
    }
    char line[256];
    int threads = -1;
    while (std::fgets(line, sizeof(line), status) != nullptr) {
        if (std::sscanf(line, "Threads: %d", &threads) == 1) {
            break;
        }
    }
    std::fclose(status);
    return threads;
}

// 解析 "major.minor.patch"（纯数字与恰好两个点）并与三元组比较，
// 验证 library_version_string() 是与 library_version() 一致的规范文本。
bool triple_matches(std::string_view text, const astra::Version& version) {
    std::uint32_t parts[3] = {0u, 0u, 0u};
    int index = 0;
    std::uint32_t value = 0;
    bool digits = false;
    for (char c : text) {
        if (c == '.') {
            if (index >= 2 || !digits) {
                return false;
            }
            parts[index++] = value;
            value = 0;
            digits = false;
        } else if (c >= '0' && c <= '9') {
            value = value * 10u + static_cast<std::uint32_t>(c - '0');
            digits = true;
        } else {
            return false;
        }
    }
    if (index != 2 || !digits) {
        return false;
    }
    parts[2] = value;
    return parts[0] == version.major && parts[1] == version.minor && parts[2] == version.patch;
}

}  // namespace

int main() {
    // 1. 同一安装：header_version() == library_version() == ASTRA_VERSION_* 宏。
    const astra::Version library = astra::library_version();
    if (!(kHeaderVersion == library)) {
        std::printf("astra header/library version mismatch: header %u.%u.%u, library %u.%u.%u\n",
                    kHeaderVersion.major, kHeaderVersion.minor, kHeaderVersion.patch,
                    library.major, library.minor, library.patch);
        return 1;
    }
    if (library.major != ASTRA_VERSION_MAJOR || library.minor != ASTRA_VERSION_MINOR ||
        library.patch != ASTRA_VERSION_PATCH) {
        std::printf("astra library version does not match header macros\n");
        return 1;
    }

    // 2. 无副作用：查询前后进程线程数不变（不启动 Reaper/Worker 线程）。
    const int threads_before = thread_count();
    const astra::Version sink_version = astra::library_version();
    const auto sink_string = astra::library_version_string();
    const int threads_after = thread_count();
    if (threads_before < 0 || threads_after < 0) {
        std::printf("cannot read thread count from /proc/self/status\n");
        return 1;
    }
    if (threads_before != threads_after) {
        std::printf("version queries started threads: before=%d after=%d\n",
                    threads_before, threads_after);
        return 1;
    }

    // 3. string_view 指向进程期静态文本：两次调用地址与内容稳定，
    //    且文本三元组与 library_version() 一致（canonical SemVer）。
    const auto text_a = astra::library_version_string();
    const auto text_b = astra::library_version_string();
    if (text_a.data() != text_b.data() || text_a != text_b) {
        std::printf("library_version_string() is not stable across calls\n");
        return 1;
    }
    if (sink_version != library || !triple_matches(sink_string, library) ||
        !triple_matches(text_a, library)) {
        std::printf("library version string does not match the version triple\n");
        return 1;
    }

    return 0;
}

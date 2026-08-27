---
status: accepted
date: 2026-08-26
decisions: [D-145, D-146, D-159, D-162, D-164, D-167]
---

# Releases guarantee source API, not cross-toolchain ABI

AstraScheduler是C++20 compiled CMake library，static默认、shared可选。Supported Configuration仅限64-bit Linux：Tier-1支持Linux x86_64 GCC/Clang，Tier-2用native Linux AArch64验证weak-memory；Windows/MSVC和其他非Linux平台不受支持。atomic capability不足时使用locked semantic fallback并公开报告。

每个Runtime以不可变`SchedulerCapabilities`精确报告Local Deque实际backend：Global-only=`None`、locked/fallback=`Locked`、满足条件且实际启用的Chase-Lev=`ChaseLevLockFree`。`lock_free_local_deque()`仅是该枚举的派生判断，不能把项目版本或算法名称当作lock-free证据。

Supported Configuration每进程只加载一个implementation instance：主程序可用单份static；多个DSO/plugin必须共同链接同一个exact-version shared library。多个DSO各自嵌入static copy时，process-wide Reaper/IDs/metrics和跨模块Handle互操作均不受支持。

0.x minor允许有migration的breaking source change，v1起documented source与observable semantics遵循SemVer。项目不承诺跨Astra版本、compiler、stdlib、CRT或build配置的C++ ABI；shared package必须exact-version/toolchain匹配。Metrics、Trace和Benchmark schemas独立版本化。

public header以宏和constexpr `header_version()`报告编译期版本，compiled binary以无副作用`library_version()`/`library_version_string()`报告已链接版本；查询不创建Runtime或Reaper。CMake exact-version检查仍是主要安全边界，运行时值用于artifact与诊断。

路线从v0.1就交付TaskHandle/Outcome/Cancellation及完整Runtime lifecycle，然后纵向加入locked Work-Stealing、Chase-Lev、DAG、Coroutine/Timer、Priority/Deadline、Observability、Benchmark，v0.9专门hardening后才冻结v1。

决策细节见 [D-145、D-146、D-159、D-162、D-164 与 D-167](../../.scratch/astra-scheduler-runtime/decision-log.md)。Linux-only支持范围与WSL开发入口另见 [ADR-0047](0047-linux-only-support-and-wsl-development.md)。

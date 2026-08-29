# AstraScheduler Supported Platforms（AST-052 / R-111 / R-107 / D-167）

机器可读版本：[`tools/platform_matrix.json`](../tools/platform_matrix.json)
（审计入口：`python3 tools/check_platform_matrix.py`，已注册为 ctest `check_platform_matrix`）。

## Tier-1（release-gated）

| 平台 | 工具链 | 证据 |
|---|---|---|
| Linux x86_64 | GCC 13+ / Clang 17+ | 构建、unit/integration 测试、package consumer、sanitizer |

## Tier-2（periodic native validation）

| 平台 | 工具链 | 证据 |
|---|---|---|
| native Linux AArch64 | GCC / Clang | 定期 native weak-memory stress（cross-compile/QEMU 编译不替代） |

## Unsupported

Windows/MSVC、macOS 及其他非 Linux OS、32-bit 目标一律不支持；
偶然编译成功不产生兼容性/正确性/性能/生命周期保证，也不得进入 release gate
或 package 支持声明。Linux atomic 能力不足时按 R-101 报告 Locked fallback，
不改变平台支持状态。

## 部署拓扑（R-107 / D-159）

每个受支持 OS 进程恰好加载一个 Astra implementation instance：单个可执行程序
直接链接一份 static library；多个 DSO/plugin 必须共同依赖同一 exact-version
shared library。把 static archive 复制进两个动态模块、加载多个私有
loader namespace 副本或跨不匹配 CRT 边界传递句柄均属 unsupported topology，
不享有进程唯一 Reaper coordinator/ID/metrics 域保证。

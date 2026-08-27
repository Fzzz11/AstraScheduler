# AstraScheduler Linux / WSL Development Guide

## Supported target

AstraScheduler 的开发目标和最终支持平台均仅限 64-bit Linux：

- Tier-1：Linux x86_64，GCC 13+ 或 Clang 17+。
- Tier-2：native Linux AArch64，使用 GCC/Clang 定期验证 weak-memory 路径。
- Unsupported：Windows/MSVC、macOS、其他非 Linux OS 与 32-bit 目标。

非 Linux 平台偶然编译成功不产生兼容性、正确性、生命周期或性能保证。

## Canonical local environment

本机开发统一在 WSL Linux 用户空间执行。宿主工作区：

```text
D:\code\cppStudy\AstraScheduler
```

对应的 canonical WSL 工作区：

```text
/mnt/d/code/cppStudy/AstraScheduler
```

从 Windows 宿主进入项目时使用：

```powershell
wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && <command>"
```

已经位于 WSL shell 时，直接进入 canonical 工作区执行同一 Linux 命令即可。

## Commands that must run in WSL

以下命令及其生成的验证证据必须来自 WSL/Linux：

- CMake configure、build、install 与独立 package consumer；
- unit、integration、stress 与 concurrency tests；
- format、lint 与静态分析；
- ASan、UBSan、TSan 或其他 Linux sanitizer；
- Benchmark build、scenario runner 与 artifact 生成；
- release gate、版本与 schema 验证脚本。

Windows PowerShell/cmd 可以启动 WSL或查看宿主文件，但不得直接运行 MSVC、Windows-native CMake/Ninja、项目测试程序或 benchmark 作为项目证据。

## Build-directory isolation

WSL/Linux 使用独立且可识别的 build 目录，例如：

```text
build/wsl-gcc-debug
build/wsl-clang-release
build/wsl-clang-asan
```

不得在 Windows native 与 WSL 之间复用 CMake cache、object、library、executable、sanitizer output 或 benchmark artifact。发现来源不明的 build cache 时，应创建新的 WSL 专用目录，而不是在原目录切换 generator/toolchain。

## Current documentation gate

在实现 CMake Runtime 之前，当前仓库级文档门禁从 WSL 运行：

```powershell
wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"
```

后续每个 Ticket 必须在 `Verification` 中记录真实 WSL/Linux 命令与结果。只有来自 Tier-1/Tier-2 Linux 环境的证据才能支持 release claim。

## Benchmark note

WSL benchmark 必须在 artifact 中记录 WSL、kernel、CPU、compiler、build mode 与环境限制。WSL 数据适合本机回归与开发比较；正式性能声明应可由独立 native Linux runner 复核。

设计理由见 [ADR-0047](adr/0047-linux-only-support-and-wsl-development.md)，规范来源见 [Runtime Spec](../.scratch/astra-scheduler-runtime/spec.md)。

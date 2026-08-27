---
status: accepted
date: 2026-08-27
decisions: [D-167, D-168]
---

# Linux-only support and WSL development

AstraScheduler 的最终 Supported Configuration 仅包含 64-bit Linux。Tier-1 是 Linux x86_64 上的 GCC 13+ 与 Clang 17+；Tier-2 是 native Linux AArch64 上的 GCC/Clang weak-memory 验证。Windows/MSVC、macOS、其他非 Linux OS 与 32-bit 目标均不受支持，不进入 release gate，也不能因偶然编译成功获得兼容性、正确性或性能承诺。

本机开发统一在 WSL Linux 用户空间执行。项目宿主路径 `D:\code\cppStudy\AstraScheduler` 的 canonical WSL 路径为 `/mnt/d/code/cppStudy/AstraScheduler`；configure、build、test、format、lint、package consumer、sanitizer、stress、benchmark 与 release verification 均从 WSL 运行。Windows PowerShell/cmd 只可启动 WSL 或执行非开发性的宿主编排，不可直接调用 Windows-native toolchain 形成项目证据。

WSL/Linux build 目录与任何宿主原生产物严格隔离，不能跨环境复用 CMake cache、object、library、executable 或 sanitizer artifact。WSL 是本机开发入口，不是最终用户运行时依赖；CI 和 release 仍以原生 Linux runner 为权威证据。本机 WSL benchmark 必须记录环境，正式性能声明应可在 native Linux 复核。

该选择把有限验证预算集中到 GCC/Clang、Linux sanitizer 与 native AArch64 memory-order 证据，并避免在没有持续 MSVC/Windows 验证时制造虚假 Tier-1 承诺。未来若增加其他 OS，必须新增决策、Spec 规则、Tickets 与持续验证资源。

决策细节见 [D-167 与 D-168](../../.scratch/astra-scheduler-runtime/decision-log.md)。

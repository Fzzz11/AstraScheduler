# AstraScheduler Project Instructions

## Platform scope

- AstraScheduler 的开发目标和最终 Supported Configuration 均仅限 64-bit Linux。
- Tier-1 是 Linux x86_64 GCC 13+ / Clang 17+；Tier-2 是 native Linux AArch64 GCC/Clang weak-memory 验证。
- Windows/MSVC、macOS、其他非 Linux OS 与 32-bit 目标不受支持，不得新增相应 release 承诺或把偶然编译成功作为验证证据。

## Local development environment

- 所有项目开发命令必须在本机 WSL Linux 内执行，包括 configure、build、test、format、lint、package consumer、sanitizer、stress、benchmark 与 release verification。
- Canonical WSL workspace 是 `/mnt/d/code/cppStudy/AstraScheduler`。
- 从 Windows 宿主发起命令时使用：`wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && <command>"`。
- Windows PowerShell/cmd 仅可用于启动 WSL、查看宿主状态或非开发编排；不得直接调用 MSVC、Windows-native CMake/Ninja、项目测试二进制或 benchmark 形成项目证据。
- 文件编辑工具可以操作共享工作区，但所有声称通过的项目验证命令必须来自 WSL/Linux。

## Build isolation

- 使用明确的 WSL/Linux build 目录，例如 `build/wsl-gcc-debug`、`build/wsl-clang-asan`。
- 不得在 Windows native 与 WSL 之间复用 CMake cache、object、library、executable、sanitizer 或 benchmark artifact。
- 本机 WSL benchmark 必须记录完整环境；正式 release 性能声明应可在 native Linux 复核。

## Authoritative design sources

- Approved Spec：`.scratch/astra-scheduler-runtime/spec.md`
- Decision ledger：`.scratch/astra-scheduler-runtime/decision-log.md`
- Ticket tracker：`.scratch/astra-scheduler-runtime/issues/`
- Linux/WSL ADR：`docs/adr/0047-linux-only-support-and-wsl-development.md`

若实现请求与上述来源冲突，先停止实现并修订 decision ledger、Spec 与 Tickets，不得以代码或聊天上下文静默覆盖。

## Agent skills

### Issue tracker

AstraScheduler 使用本地 Markdown Ticket tracker，而不是 GitHub Issues；发布、读取和更新 Ticket 时遵循 `docs/agents/issue-tracker.md`。

### Domain docs

AstraScheduler 使用单领域上下文；开始需求讨论、设计、拆票或实现前，按 `docs/agents/domain.md` 定位术语表、ADR、决策台账与 approved Spec。

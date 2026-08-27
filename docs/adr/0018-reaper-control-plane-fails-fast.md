---
status: accepted
date: 2026-08-25
decisions: [D-040]
---

# Reaper control-plane failures fail fast

Reaper 是 orphan Runtime State、唯一 thread join 与 Finalized 发布的最后安全网；无法证明可恢复的 coordinator 异常、所有权不变量破坏或 join 故障必须先进行 `noexcept` 的尽力诊断，再调用 `std::terminate()`。Runtime 不得把控制面损坏伪装成 `TimedOut`、Task 异常、`Stopped`/`Finalized`，也不得 detach 或重启 Reaper；继续运行却无法兑现内存和线程生命周期承诺，比确定性 fail-fast 更危险。

用户 Callable 异常、永久 Pending Runtime 与合法 wait timeout 都是预期域行为，不进入 fatal path。故障注入必须在独立子进程验证。

决策细节见 [D-040](../../.scratch/astra-scheduler-runtime/decision-log.md)。

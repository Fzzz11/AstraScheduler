---
status: accepted
date: 2026-08-25
decisions: [D-026, D-027, D-028, D-029]
---

# Finalization is split-phase

Reaper Finalization 被拆为立即返回的 `begin_finalization()` 与独立的 `wait()`/`wait_for(timeout)`：begin 永久关闭注册并启动 Graceful 终结，`wait()` 只在真实 Finalization Completion 后返回且可以无限阻塞，`wait_for()` 超时只返回 `TimedOut`。超时不伪造完成、不恢复注册，也不停止 Reaper/coordinator；调用方继续掌握等待、显式升级或终止进程的策略选择。

显式 Finalization Escalation 覆盖同一核算集合内全部尚未完成的 Runtime，包括已经失去 Scheduler Handle、仅由 Reaper 托管的 Runtime State。它把 Graceful 单向提升为 Immediate 并在升级请求可靠发布后返回，复用原 Finalization Completion；Running Task 仍只接受协作停止请求，因此升级不会把无界终结伪装为有界终结。

决策细节见 [D-026](../../.scratch/astra-scheduler-runtime/decision-log.md)、[D-027](../../.scratch/astra-scheduler-runtime/decision-log.md)、[D-028](../../.scratch/astra-scheduler-runtime/decision-log.md) 与 [D-029](../../.scratch/astra-scheduler-runtime/decision-log.md)。

最终公共 Interface 与并发等待协议见 [ADR-0016](./0016-finalization-uses-a-control-object.md)。

---
status: accepted
date: 2026-08-25
decisions: [D-012]
---

# Immediate shutdown monotonically escalates graceful shutdown

Graceful Stopping 期间收到的非 Worker `shutdown_now()` 会在单一线性化点把 Shutdown Mode 升级为 Immediate：升级前已接受但尚未运行的任务被取消，Running Task 收到协作式 stop request，升级后所有 submission admission 关闭。Immediate 是不可逆的更强停止策略，后续 `shutdown()` 不能将其降级；这使紧急停止请求仍能兑现语义，同时避免恢复已取消任务、撤销 stop request 或重新开放 admission 等不可能安全完成的回滚。

决策细节见 [D-012](../../.scratch/astra-scheduler-runtime/decision-log.md)。

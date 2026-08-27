---
status: accepted
date: 2026-08-25
decisions: [D-024]
---

# Reaper Finalization requests graceful shutdown

Reaper Finalization 对关闭注册前纳入核算的全部 Runtime 请求 Graceful Shutdown，保持已有 Immediate 模式并允许显式 `shutdown_now()` 单向升级。Finalization 是进程级资源终结而非隐式取消意图；已注册但仍在启动中的 Runtime 必须在开放任何用户工作前观察 sticky 终结请求。

决策细节见 [D-024](../../.scratch/astra-scheduler-runtime/decision-log.md)。

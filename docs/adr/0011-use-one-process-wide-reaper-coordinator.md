---
status: accepted
date: 2026-08-25
decisions: [D-021]
---

# Use one process-wide Reaper coordinator

AstraScheduler 在一个进程内共享一个 Reaper Service，并由恰好一条不属于任何 Scheduler 的专用线程协调全部 Pending Runtime State 与 Join Ready 回收。D-020 已隔离永久 Pending Runtime 的阻塞风险，因此单线程足以承载低频控制面工作，同时避免每个 Scheduler 或 handoff 创建额外回收线程。

决策细节见 [D-021](../../.scratch/astra-scheduler-runtime/decision-log.md)。

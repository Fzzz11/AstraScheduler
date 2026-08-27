---
status: accepted
date: 2026-08-25
decisions: [D-020]
---

# Reaper joins only Join Ready runtimes

Reaper 在 handoff 后可以持有 Pending Runtime State，但不得阻塞等待其活动任务或 Drain Work Closure；只有全部 Worker 不可逆地进入终止收尾且 Runtime 达到 Join Ready 后，Reaper 才能认领 join、发布 `Stopped` 并最终回收。这个两阶段边界允许永久不终结的 Runtime 保持存活，同时不阻塞其他 Scheduler 的回收。

决策细节见 [D-020](../../.scratch/astra-scheduler-runtime/decision-log.md)。

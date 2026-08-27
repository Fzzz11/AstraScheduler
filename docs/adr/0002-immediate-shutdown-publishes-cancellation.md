---
status: accepted
date: 2026-08-25
decisions: [D-006, D-007, D-008, D-009]
---

# Immediate shutdown cancels pending work, requests cooperative stop, joins workers, and rejects self-shutdown

Immediate Shutdown 对尚未进入 `Running` 的已接受任务发布 `Cancelled` 终态并唤醒等待者，对已经 `Running` 的任务只发布协作式 stop request，绝不强杀执行线程；从非 Worker 线程调用时，它会等待所有 Worker 退出并 join 后才返回。当前 Scheduler 的 Worker 调用该方法会在任何状态转换、取消或 stop request 之前被同步拒绝，从而避免 self-join，也不让同一 API 静默退化为异步语义。该契约为资源回收提供明确边界并保留 RAII，但接受不合作 Callable 可导致关停无限期阻塞。

决策细节见 [D-006 至 D-009](../../.scratch/astra-scheduler-runtime/decision-log.md)。

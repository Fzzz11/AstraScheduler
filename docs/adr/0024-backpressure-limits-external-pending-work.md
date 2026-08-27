---
status: accepted
date: 2026-08-26
decisions: [D-083, D-084, D-085, D-086]
---

# Backpressure limits external pending work

AstraScheduler 不把 Global Injection Queue 的物理容量误作整个 Runtime 的硬任务上限。`external_pending_capacity` 限制每个 Runtime 已接受但尚未首次 Running 的 External Submission，覆盖 Waiting 与 Ready；同 Runtime Internal Submission 为保持 Graceful Drain Work Closure 和 Worker liveness 不占该配额。该选择允许内部任务洪泛超过配额，因此它是外部准入保护，不是总内存保证。

External Backpressure 只提供 `Reject` 与 `Block`，默认 Reject；CallerRuns 被排除，所有用户 Callable 都必须经正常 Task admission 和 Scheduler 路径执行。Block 只会阻塞普通非 Worker，其他 Runtime 的 Worker 在容量满时立即拒绝。Blocked submit 同时观察 slot 与 lifecycle gate，可靠处理 release/shutdown 唤醒但不承诺 FIFO 或返回延迟。

决策细节见 [D-083 至 D-086](../../.scratch/astra-scheduler-runtime/decision-log.md)。

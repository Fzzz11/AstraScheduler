---
status: accepted
date: 2026-08-25
decisions: [D-002, D-003, D-010, D-011]
---

# Graceful shutdown closes external admission, drains authorized internal work, joins workers, and rejects self-shutdown

Graceful shutdown 以可线性化的 `Running → Stopping` 转换关闭 External Submission，同时仍允许已接受任务从同一 Scheduler 的执行上下文派生 Internal Submission。从非 Worker 线程调用时，`shutdown()` 同步等待整个 Drain Work Closure 终结、全部 Worker 退出并完成 join，发布 `Stopped` 后才返回；不能终结的任务或无限派生链可使调用无限期阻塞。当前 Scheduler 的 Worker 调用该方法会在任何状态转换、admission 关闭或 outstanding-work 变更之前被同步拒绝，从而避免当前任务对自身形成 self-wait 和后续 self-join，也不让同一 API 静默退化为异步语义。相比 best-effort admission、在 `Stopping` 后拒绝所有提交或关停请求后立即返回，这增加了执行上下文识别与终止检测复杂度，但避免孤儿任务、已接受工作链被截断以及返回后 Worker 仍访问 Runtime 的生命周期竞态。

决策细节见 [D-002、D-003、D-010 与 D-011](../../.scratch/astra-scheduler-runtime/decision-log.md)。

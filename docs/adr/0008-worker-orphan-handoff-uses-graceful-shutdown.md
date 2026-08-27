---
status: accepted
date: 2026-08-25
decisions: [D-018]
---

# Worker orphan handoff uses graceful shutdown

同 Scheduler Worker 触发最后 Scheduler Handle 析构时，`Running` Runtime 默认请求 Graceful Shutdown，已在 `Stopping` 时保持当前 Shutdown Mode，并在 Runtime State 安全移交给 Reaper 后立即返回。Reaper 改变的是等待与回收的执行上下文，不把析构隐式变成任务取消；代价是无法终结的 Drain Work Closure 会让 Runtime State 的最终回收无限期延后。

决策细节见 [D-018](../../.scratch/astra-scheduler-runtime/decision-log.md)。

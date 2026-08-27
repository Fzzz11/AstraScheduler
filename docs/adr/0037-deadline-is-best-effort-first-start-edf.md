---
status: accepted
date: 2026-08-26
decisions: [D-132, D-133, D-134]
---

# Deadline is best-effort first-start EDF

`TaskDeadline`是steady-clock绝对首次开始目标，不是Timer Wake Time、完成期限或取消时刻。`TaskDeadline::after`在构造时固定时间；Deadline只由显式`TaskOptions`携带，不沿Internal submission、Graph edge或Coroutine组合继承。

带Deadline且从未开始的Ready Task进入Runtime-wide、按Priority分区的indexed EDF heaps。Priority weighted calendar先选band，同band内deadline-preferred EDF最多连续服务8个任务，随后给普通Global FIFO一次机会。Deadline Task首次Running后退出EDF，Coroutine后续resume使用普通Priority band。

Deadline miss只进入Metrics/Trace；Task继续执行且Outcome不变。Runtime不提供自动boost/cancel、抢占、最大lateness或硬实时保证。

决策细节见 [D-132 至 D-134](../../.scratch/astra-scheduler-runtime/decision-log.md)。

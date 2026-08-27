---
status: accepted
date: 2026-08-26
decisions: [D-114, D-115, D-116, D-117]
---

# Coroutine Tasks are cold and Runtime-owned after spawn

`astra::Task<T>` 是initial-suspend cold、move-only、single-shot frame owner；未spawn析构安全destroy，spawn成功才把唯一ownership转给Runtime并返回统一`TaskHandle<T>`，rejection保持source Task可重试。Body只由正常Scheduler Worker执行，任何completion callback都不能inline resume。

每个resume segment由唯一Ready/Resume Ticket与Worker claim驱动，跨Worker迁移允许但并发resume禁止。Promise final suspend always-suspend且noexcept；结果先移到独立TCB并发布Terminal Outcome，再由Runtime在final-suspended前提下恰好一次destroy frame，因此TaskHandle结果不保留整个coroutine frame。

决策细节见 [D-114 至 D-117](../../.scratch/astra-scheduler-runtime/decision-log.md)。

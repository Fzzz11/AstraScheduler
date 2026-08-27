---
status: accepted
date: 2026-08-25
decisions: [D-013, D-016]
---

# Concurrent shutdown callers share one completion

一次进行中的关停只有一个 Shutdown Completion：所有非 Worker `shutdown()`/`shutdown_now()` 调用幂等参与同一过程，必要时仅按 D-012 升级 Shutdown Mode，并等待全部 Worker 退出、各被 join 恰好一次且 `Stopped` 发布后才返回。具体由哪个线程执行 join 是实现选择；共享完成状态避免后来的调用提前返回、迫使外部串行化或并发 join 同一 Worker。`Stopped` 是吸收状态，完成后的任一关停调用都成功、无副作用地立即返回，不创建第二个完成世代，也不追溯改写历史任务终态。

决策细节见 [D-013 与 D-016](../../.scratch/astra-scheduler-runtime/decision-log.md)。

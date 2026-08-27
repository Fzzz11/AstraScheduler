---
status: accepted
date: 2026-08-25
decisions: [D-014]
---

# Destructor uses graceful shutdown as an RAII fallback

从非 Worker 线程销毁活动 Scheduler 时，析构函数是同步、`noexcept` 的 RAII 回收边界：`Running` 状态发起 Graceful Shutdown，已在 `Stopping` 时加入现有 Shutdown Completion 且不改变当前 Shutdown Mode。析构只有在全部 Worker 退出、各自被 join 且 `Stopped` 发布后才完成；Drain Work Closure 无法终结时可以无限期阻塞，但不得 detach Worker 或伪造完成。同 Scheduler Worker 上的自身析构不在本 ADR 中定义。

决策细节见 [D-014](../../.scratch/astra-scheduler-runtime/decision-log.md)。

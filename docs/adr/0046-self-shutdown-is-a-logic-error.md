---
status: accepted
date: 2026-08-26
decisions: [D-166]
---

# Self shutdown is a logic error

同一 Runtime Worker 调用该 Scheduler 的同步 `shutdown()` 或 `shutdown_now()` 时，在任何 lifecycle、admission、cancellation、join 或 wait 副作用前抛 `std::logic_error`。其他 Runtime Worker仍是目标Runtime的合法非Worker同步调用方；empty Scheduler同样抛logic_error但不增加reason enum。

两个方法保持`void`且非`noexcept`，异常文本不属于稳定契约。该选择与Direct Self-Wait和Finalization Worker Wait保持一致，避免静默异步降级或新增低价值专用错误类型。

决策细节见 [D-166](../../.scratch/astra-scheduler-runtime/decision-log.md)。

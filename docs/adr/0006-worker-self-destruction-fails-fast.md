---
status: superseded by ADR-0007
date: 2026-08-25
decisions: [D-015]
---

# Worker self-destruction fails fast

若 Scheduler 的析构函数在其自身 Worker 上执行且 Runtime 尚未完成 `Stopped`，这是生命周期契约违例，必须在任何部分销毁或关停副作用前调用 `std::terminate()`。同步析构无法等待或 join 当前 Worker，继续析构又会使任务返回路径访问失效状态；在没有真实用例驱动前，Runtime 不引入独立共享状态与外部 reaper，也不以 detach 或静默异步清理伪装支持。

决策细节见 [D-015](../../.scratch/astra-scheduler-runtime/decision-log.md)。

本决策已被 [ADR-0007](./0007-decouple-scheduler-handle-from-runtime-state.md) 取代。

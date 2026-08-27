---
status: accepted
date: 2026-08-25
decisions: [D-017]
supersedes: ADR-0006
---

# Decouple Scheduler Handle from shared Runtime State

Scheduler Handle 不再是 Runtime 的唯一生命周期载体：Worker 触发最后 Handle 析构时，必须把共享 Runtime State 原子移交给非目标 Worker 的 Reaper，由后者协调 join 与最终回收，而不是 `std::terminate()`、self-join 或 detach。这个对象模型承担额外的共享所有权与回收协调复杂度，以合法支持 Worker 上的最后 Handle 释放并保证 Runtime State 存活到全部 Worker 停止访问。

决策细节见 [D-017](../../.scratch/astra-scheduler-runtime/decision-log.md)；本 ADR 取代 [ADR-0006](./0006-worker-self-destruction-fails-fast.md)。

---
status: accepted
date: 2026-08-26
decisions: [D-157]
---

# Scheduler options expose policy, not implementation tuning

Stable `SchedulerOptions`只暴露worker count、External capacity/backpressure、Helping/local burst/steal bounds、Metrics level与shared TraceCollector。默认worker count由`recommended_worker_count()`显式计算并fallback到1；所有必需size字段的0是startup前`invalid_argument`，不承载隐藏auto语义。

Priority weights、deadline burst与histogram buckets由稳定policy固定；active spin、deque capacity、timer和notification internals保持private benchmark-tuned。Options在Scheduler构造时形成immutable snapshot，Runtime和benchmark报告resolved values。

决策细节见 [D-157](../../.scratch/astra-scheduler-runtime/decision-log.md)。

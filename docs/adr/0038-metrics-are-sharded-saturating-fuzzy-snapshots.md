---
status: accepted
date: 2026-08-26
decisions: [D-135, D-136, D-137]
---

# Metrics are sharded saturating fuzzy snapshots

Runtime Metrics分为Off、默认Basic和Detailed。Basic记录固定低基数的准入、Task Identity、调度、Coroutine、Timer、Graph和Deadline counter/gauge；Detailed加入per-Worker/per-Priority分解与固定log2纳秒直方图。Metrics不参与调度正确性。

高频数据分片更新，所有累计字段饱和而不wrap。`metrics_snapshot()`返回不可变fuzzy snapshot：每字段安全有效，但并发capture不暂停Worker、不提供全局事务瞬间；守恒关系只在quiescent point强制成立。

Detailed不保存原始样本或在Runtime计算percentile，明确区分ready wait、execution segment、task wall time、timer wake lateness和deadline start lateness。

决策细节见 [D-135 至 D-137](../../.scratch/astra-scheduler-runtime/decision-log.md)。

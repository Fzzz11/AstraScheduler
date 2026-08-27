---
status: accepted
date: 2026-08-26
decisions: [D-148, D-149]
---

# Observability covers process lifecycle and wait edges

`process_metrics_snapshot()`始终低成本启用并只观察Reaper/Finalization控制面；查询从不初始化或重启服务，Finalized后保留最终累计事实，也不把所有Runtime task counters集中到global hot path。

Runtime Metrics补充同步wait、Helping、cross-Runtime和Coroutine await的固定低基数counter/histogram。Trace用稳定source/target logical identity记录WaitBegin/End及Await arm/trigger/resume，使Helping嵌套、远端依赖和Ready queue delay可离线分析。

Observability不建立在线wait-for graph、不自动检测/打破Indirect Wait Cycle，也不改变wait、timeout、Outcome或shutdown语义。

决策细节见 [D-148 与 D-149](../../.scratch/astra-scheduler-runtime/decision-log.md)。

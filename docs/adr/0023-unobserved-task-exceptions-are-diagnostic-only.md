---
status: accepted
date: 2026-08-26
decisions: [D-081, D-082, D-151]
---

# Unobserved Task exceptions are diagnostic only

用户 Callable 的异常始终属于 Task 结果域：即使没有 Handle 调用 `get()`，Failed Outcome 也不触发 terminate、默认输出、用户全局回调、级联取消或终态改写。Runtime 内部不变量异常仍走独立 fail-fast 边界，不能与用户 Task failure 混淆。

Exception Outcome 只在 `get()`/TaskHandle await即将传播时幂等标记为observed；状态和纯等待不算观察原始异常。在completion shared state最终释放时，未observed failure只在Metrics Basic/Detailed增加稳定字段`unobserved_failures`，并仅在活动Trace capture可用时尽力发event；Metrics Off/Trace disabled不保留隐藏诊断，所有组合都不影响资源释放。

决策细节见 [D-081、D-082 与 D-151](../../.scratch/astra-scheduler-runtime/decision-log.md)。

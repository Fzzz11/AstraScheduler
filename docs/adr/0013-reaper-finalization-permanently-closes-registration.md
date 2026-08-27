---
status: accepted
date: 2026-08-25
decisions: [D-023]
---

# Reaper Finalization permanently closes registration

Reaper Finalization 是进程级不可逆边界：它先线性化地永久关闭 Scheduler Runtime 注册，关闭前已注册的 Runtime 被纳入终结核算，关闭后的启动则在创建 Worker 前失败。服务进入 `Finalizing` 或 `Finalized` 后不得重建或重新开放注册，从而让 coordinator 针对一个有限且不再增长的集合安全退出。

决策细节见 [D-023](../../.scratch/astra-scheduler-runtime/decision-log.md)。

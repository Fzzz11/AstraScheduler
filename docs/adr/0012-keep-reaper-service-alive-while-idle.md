---
status: accepted
date: 2026-08-25
decisions: [D-022]
---

# Keep Reaper Service alive while idle

Reaper Service 首次建立后，即使注册表和回收队列为空，也保留同一 coordinator thread 并阻塞等待后续工作；单个或最后一个 Scheduler 的关停不得隐式停止它。用一条休眠线程换取无 self-join、无 stop/register 竞态的稳定进程级 handoff 能力，服务只在独立的进程级终结阶段退出。

决策细节见 [D-022](../../.scratch/astra-scheduler-runtime/decision-log.md)。

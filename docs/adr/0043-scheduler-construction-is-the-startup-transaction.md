---
status: accepted
date: 2026-08-26
decisions: [D-155, D-156]
---

# Scheduler construction is the startup transaction

`Scheduler(SchedulerOptions)`在一个同步事务中验证配置、建立Runtime/Reaper handoff、创建Worker并发布Running；失败完整rollback后抛出。稳定API没有public Created/start/restart，Scheduler是copyable shared Handle，最后一个副本释放才触发既有RAII shutdown或Worker Reaper handoff。

Running publication与Finalization registration close全序：close先赢则barrier内startup回滚并抛creation rejection；Running先赢则构造成功，但Runtime随即纳入Graceful Finalization，因此调用方首次观察时可已Stopping。不存在Finalizing后的External admission窗口。

空/moved-from Scheduler只有`valid()`及invalid `runtime_id()`可无异常观察，其他runtime操作在副作用前拒绝。Creation、submission和shutdown错误域保持分离。

决策细节见 [D-155 与 D-156](../../.scratch/astra-scheduler-runtime/decision-log.md)。

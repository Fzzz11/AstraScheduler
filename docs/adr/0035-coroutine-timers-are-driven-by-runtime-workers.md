---
status: accepted
date: 2026-08-26
decisions: [D-126, D-127, D-128]
---

# Coroutine timers are driven by Runtime Workers

`astra::sleep_for`与steady-clock `sleep_until`把当前Astra Task挂起到Timer Wake Time；即时路径仍检查取消。Wake Time只是Ready eligibility下界，不是执行或完成保证，项目不声称硬实时timer。

每个Runtime State持有按Wake Time和sequence排序的indexed min-heap。Worker在scheduler loop和park handshake中驱动到期项，新最早项推进work epoch并通知parked Worker；不新增per-Runtime或process-wide Timer Thread。Timer到期与stop竞争唯一resume trigger，取消获胜时主动erase，所有恢复均在锁外发布到source Runtime Global Queue。

Timer仍属于原Task与Drain Work Closure：Graceful保留原时间语义，Immediate请求合作式取消，且timer不重复占External Pending slot或outstanding count。

决策细节见 [D-126 至 D-128](../../.scratch/astra-scheduler-runtime/decision-log.md)。

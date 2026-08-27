---
status: accepted
date: 2026-08-26
decisions: [D-160]
---

# Scheduler status is a paired snapshot

`Scheduler::status()`一次线性化返回`SchedulerState`与`ShutdownMode`成对快照，避免两个getter跨状态转换形成撕裂组合。合法pair只有Running+None、Stopping+Graceful/Immediate和Stopped+最后mode；Created/Starting不公开。

Status是非阻塞、无副作用诊断，不是admission capability。返回后可立即过时，submit仍处理真实rejection；core不增加`is_running()`等check-then-act便利接口。

决策细节见 [D-160](../../.scratch/astra-scheduler-runtime/decision-log.md)。

# AST-065 — 抽出拥有 timer 不变量的 TimerQueue

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-124)
Milestone: v1.2.0
Blocked by: AST-064
Status: done
Claimed by: agent

## Rules and decisions

- R-124 [primary] — TimerQueue 拥有 register/cancel/due/shutdown cancel；source: D-176
- R-079 [supporting] — sleep 到期与取消语义不变；source: D-126

## What to build

内部 `TimerQueue` 拥有 timer heap/map 与互斥。Impl 只在最早到期变化时唤醒 worker。模块不得只持有 `Impl*` 转发。

## Invariants

- sleep 到期/取消/关停取消语义不变。
- park handshake 仍由 Impl 拥有。

## Test-first seam

- Public seam: worker timer 行为测试。
- RED evidence: TimerQueue 以 Impl* 为唯一状态时审计失败。

## Acceptance criteria

- [x] `[R-124]` TimerQueue 拥有 heap/map，不含 Impl* 浅转发。
- [x] `[R-079]` worker timer 测试通过。

## Out of scope

- 不抽 ReadyQueues/steal/park。

## Traceability

- Spec: `.scratch/astra-scheduler-runtime/spec.md` — R-124
- Decisions: D-176
- Verification: WSL `build/wsl-gcc-debug`；`src/timer_queue.{hpp,cpp}` 拥有 `heap_`/`map_`，encapsulation 审计拒绝 `Scheduler::Impl*`；Impl 仅在 `became_earliest` 时 `work_cv.notify_one()`；`astra_worker_timers_test` 通过；ctest 52/52。

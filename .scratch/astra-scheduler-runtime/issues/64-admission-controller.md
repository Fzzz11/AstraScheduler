# AST-064 — 抽出拥有 slot 不变量的 AdmissionController

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-124)
Milestone: v1.2.0
Blocked by: AST-063
Status: done
Claimed by: agent

## Rules and decisions

- R-124 [primary] — AdmissionController 拥有外部 slot/backpressure；source: D-176
- R-061 [supporting] — 外部 Block/Reject 与 Worker 不得阻塞；source: D-084, D-085
- R-062 [supporting] — Stopping/Stopped/CapacityExhausted 语义不变；source: D-088

## What to build

内部 `AdmissionController` 拥有 pending 计数、容量、backpressure 策略与 slot 等待。`Scheduler::Impl` 通过该模块 acquire/release，而不是把 slot 字段散落在 lifecycle 里。模块不得只持有 `Impl*` 转发。

## Invariants

- 内部提交仍豁免容量。
- Block 仅非 Worker 外部提交。
- 失败路径保持 slot 平衡。

## Test-first seam

- Public seam: submit/try_submit/graph admission 现有测试。
- RED evidence: 新模块源文件含 `Scheduler::Impl*` 作为唯一状态时 encapsulation 审计失败。

## Acceptance criteria

- [x] `[R-124]` AdmissionController 拥有 pending/capacity/cv，不含 Impl* 浅转发。
- [x] `[R-061]` 现有 backpressure/Block 测试通过。
- [x] `[R-062]` Stopping/Stopped/CapacityExhausted 测试通过。

## Out of scope

- 不抽 ReadyQueues/steal/park。
- 不深化 GraphExecution。

## Traceability

- Spec: `.scratch/astra-scheduler-runtime/spec.md` — R-124
- Decisions: D-176
- Verification: WSL `build/wsl-gcc-debug`；`src/admission_controller.{hpp,cpp}` 拥有 `pending_`/`capacity_`/`slot_cv_`，encapsulation 审计拒绝 `Scheduler::Impl*`；`astra_admission_backpressure_test` 与 `astra_graph_admission_test` 通过；ctest 52/52。

# AST-044 — 实现 side-effect-free Process Metrics

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-095)
Milestone: v0.7.0
Blocked by: AST-007, AST-020, AST-021, AST-043
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-095 [primary] — Process Metrics 只观察Reaper/Finalization且查询不初始化；source: D-148

## What to build

提供固定 Reaper/Finalization counters、gauges、状态与时长；查询不初始化服务，Finalized 后保留终值，不聚合 Runtime task metrics。

## Invariants

- `[R-095]` `astra::process_metrics_snapshot()`必须始终提供固定counter runtime_registrations、runtime_handoffs、runtimes_joined、finalization_begin_calls、finalization_wait_timeouts、finalization_escalations，固定gauge registered_runtimes、pending_runtimes、join_ready_runtimes，以及ProcessServiceState、FinalizationState、capture steady time、finalization elapsed/completion duration和saturated；调用前返回NotStarted/零且不得初始化服务，Finalized后保留终值，不聚合Runtime task counters，逐字段安全并沿用fuzzy标记/区间语义。 例外边界：per-Runtime task metrics由R-084/R-085提供。

## Test-first seam

- Public seam: process-wide coordinator lifecycle诊断。
- RED evidence: 先写 Scheduler 创建前零值/无线程、handoff/join、timeout/escalation、Finalized 稳定终值和 fuzzy 字段安全。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-095]` 未创建Scheduler时查询无线程副作用，finalization超时/升级可离线诊断。

## Out of scope

- 不让 Metrics/Trace 改变调度语义，不做在线 wait-for graph、后台 Trace 文件 I/O 或每 Task 默认日志。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-095
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-148
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


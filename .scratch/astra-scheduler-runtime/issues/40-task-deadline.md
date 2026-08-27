# AST-040 — 固定 TaskDeadline 的 first-start 语义

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-082)
Milestone: v0.6.0
Blocked by: AST-038
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-082 [primary] — TaskDeadline 是显式首次开始目标；source: D-132

## What to build

在 TaskOptions 接受显式 steady-clock 绝对 Deadline；只比较首次成功进入 Running，miss 仅记录事实，不等待/取消/提升优先级。

## Invariants

- `[R-082]` 最终 `TaskOptions`值类型必须含 `Priority priority{Normal}` 与 `optional<TaskDeadline> deadline{}`；TaskDeadline包装steady_clock绝对时刻并由at/after构造，after在factory调用时checked/saturating固定；它仅表示首次成功Running的best-effort目标，不是Wake Time/完成期限/取消时刻，不继承、不动态修改，miss只记录而不改变Outcome或执行。 例外边界：无deadline Task不参与deadline disposition。

## Test-first seam

- Public seam: TaskOptions中optional deadline。
- RED evidence: 用 fake clock 覆盖 on-time/late、retry/resume、无 deadline 和 miss 不改变 Outcome/Priority。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-082]` 相同absolute deadline不因admission延迟重新计时，missed Task仍正常执行。

## Out of scope

- 不提供抢占、硬实时保证、动态 priority boost 或 deadline 自动取消。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-082
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-132
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending
 

# AST-023 — 实现 bounded non-repeating Steal Round

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-064)
Milestone: v0.2.0
Blocked by: AST-022
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-064 [primary] — Steal round 有界且 victim 不重复；source: D-093

## What to build

空闲 Worker 每轮只探测有界且不重复 victim，成功窃取后返回正常执行路径，并暴露确定性 victim selector seam。

## Invariants

- `[R-064]` 空闲Worker每个Steal Round必须默认最多探测8个不重复victim，排除自身并使用可重现seed的伪随机/轮转选择；单轮失败后进入backoff/park流程，不得无限扫描。 例外边界：Worker数不足时探测所有可用其他Worker。

## Test-first seam

- Public seam: v0.2+ 多Worker Work-Stealing。
- RED evidence: 先写固定种子 victim 序列、0/1/N Worker、轮内不重复和轮界 backoff 测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-064]` steal_attempt上界可测，固定seed可复现victim序列。

## Out of scope

- 不实现 Chase-Lev lock-free backend 及 v0.3 之后的 public 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-064
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-093
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


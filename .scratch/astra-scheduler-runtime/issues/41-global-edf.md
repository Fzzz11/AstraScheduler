# AST-041 — 实现 Priority 主导的 Global indexed EDF

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-083)
Milestone: v0.6.0
Blocked by: AST-027, AST-039, AST-040
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-083 [primary] — Deadline 使用Global indexed EDF且Priority主导；source: D-133, D-134, D-147

## What to build

首次 deadline work 进入按 Priority 分区的 Global indexed EDF，支持取消/claim 删除；Priority 先于 deadline，非 deadline work 保持公平来源。

## Invariants

- `[R-083]` never-started Deadline Task必须进入Runtime-wide按Priority分区的indexed EDF heap，支持start/cancel O(log n)删除且由同band最早deadline优先；Priority band选择仍按R-081主导，无deadline ordinary work在同band获得有界服务，deadline Task首次start后resume不再进入EDF。 例外边界：miss不抢占、不自动取消且无硬时延保证。

## Test-first seam

- Public seam: v0.6+ first-start scheduling。
- RED evidence: 覆盖同 band EDF、跨 band priority、相同 deadline tie、取消删除、steal/local 与 deadline destination precedence。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-083]` 同banddeadline顺序可测，低Priority早deadline不越过band策略抢占高Priority。

## Out of scope

- 不提供抢占、硬实时保证、动态 priority boost 或 deadline 自动取消。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-083
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-133, D-134, D-147
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


# AST-039 — 实现每 Ready source 的 8:4:2:1 band service

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-081)
Milestone: v0.6.0
Blocked by: AST-027, AST-038
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-081 [primary] — 每个Ready source按四band 8:4:2:1非抢占服务；source: D-130, D-131

## What to build

Global/Local/steal 各 source 使用固定加权日历产生 service opportunity，保持非抢占且避免低 band 永久饥饿。

## Invariants

- `[R-081]` Global与每个Local source必须分为四Priority band并以Critical:High:Normal:Low=8:4:2:1的确定加权机会选择非空band；空band机会可跳过但低优先级持续Ready时不得永久饿死，Priority只影响下一个claim且不得抢占Running segment。 例外边界：Local/Global outer service仍服从R-063。

## Test-first seam

- Public seam: v0.6+ Ready source内部选择。
- RED evidence: 用 deterministic calendar 验证长期机会比例、空 band 跳过、持续 Critical 负载和正在运行任务不被抢占。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-081]` 饱和基准长期服务比例接近8:4:2:1且每band有进展。

## Out of scope

- 不提供抢占、硬实时保证、动态 priority boost 或 deadline 自动取消。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-081
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-130, D-131
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


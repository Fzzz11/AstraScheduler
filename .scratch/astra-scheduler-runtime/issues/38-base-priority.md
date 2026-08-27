# AST-038 — 在 admission 解析并冻结 Base Priority

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-080)
Milestone: v0.6.0
Blocked by: AST-010, AST-022
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-080 [primary] — Priority 在 admission 解析并固定；source: D-129

## What to build

扩展稳定 TaskOptions，在 admission 解析 Low/Normal/High/Critical 并固定到 Task identity；默认值与继承规则可测试。

## Invariants

- `[R-080]` `Priority::{Low,Normal,High,Critical}`必须作为不可变base hint由TaskOptions显式配置；submit/try_submit/spawn/try_spawn与Graph emplace/emplace_coroutine提供options-first overload。无options External/cross-runtime为Normal，same-runtime Internal默认继承current Task，Graph Node继承GraphRun提交上下文，显式options总覆盖；不得提供动态set/boost或OS priority映射。 例外边界：Deadline不继承，见R-082。

## Test-first seam

- Public seam: Callable、Coroutine与Graph Node Task admission。
- RED evidence: 先写 external/internal/coroutine/graph admission 的解析矩阵和调用方后续修改 Options 不生效测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-080]` 同一Task所有resume segment使用相同base Priority。

## Out of scope

- 不提供抢占、硬实时保证、动态 priority boost 或 deadline 自动取消。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-080
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-129
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


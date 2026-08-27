# AST-026 — 实现 Chase-Lev growth、旧 buffer retention 与单一调度引用

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-067)
Milestone: v0.3.0
Blocked by: AST-025
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-067 [primary] — Deque growth 保留旧buffer并维持单一调度引用；source: D-099, D-100

## What to build

扩容复制物理 cell 但不复制 Scheduling Reference；旧 buffer 保留到 Worker teardown，claim/cleanup 仍恰好一次。

## Invariants

- `[R-067]` Chase-Lev buffer只能增长，旧buffer必须保留到deque quiescent teardown；Ready Task使用单一侵入式Scheduling Reference，resize cell复制不复制责任，Local growth/allocation失败必须回退Global且不得丢失、重复或错误完成Task。 例外边界：Runtime teardown达到quiescence后可释放全部历史buffer。

## Test-first seam

- Public seam: v0.3+ Local Deque resize与Task publication。
- RED evidence: 用极小初始容量强制连续 resize，验证任务不丢失、不重复执行/销毁并在 teardown 才释放旧 buffer。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-067]` resize并发steal下每Task最多执行一次，故障注入仍可从Global取得工作。

## Out of scope

- 不引入 DAG、Coroutine、Priority/Deadline 或未批准的动态 backend 切换。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-067
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-099, D-100
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


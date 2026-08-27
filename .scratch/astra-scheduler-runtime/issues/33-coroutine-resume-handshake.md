# AST-033 — 实现唯一 resume ownership 与 await handshake

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-074)
Milestone: v0.5.0
Blocked by: AST-024, AST-032
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-074 [primary] — Coroutine resume ownership 与 await handshake 唯一；source: D-116, D-117, D-118

## What to build

每次 suspension 只允许一个恢复所有者，通过 armed/triggered/claimed 握手消除完成与挂起竞态，禁止 inline foreign resume。

## Invariants

- `[R-074]` 每个Coroutine Task同一时刻只能有一个resume owner；segment在Ready/Running/Suspended间发布且final_suspend保留frame直到Runtime在Terminal publication后恰好一次destroy；所有内建awaiter必须用generation-scoped arm-trigger handshake保证并发completion/stop最多发布一个Ready ticket且不在await_suspend返回前resume。 例外边界：foreign awaitable内部协议不由Runtime控制。

## Test-first seam

- Public seam: 每次Coroutine resume、suspend、final destroy与内建awaitable。
- RED evidence: 穷举 completion-before-arm、arm-before-completion、cancel/complete race 和 exactly-once resume/destroy。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-074]` 无并发/递归double-resume、lost wake或double-destroy。

## Out of scope

- 不实现 I/O Runtime、Timer Wheel、inline foreign resume 或未批准的组合 API。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-074
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-116, D-117, D-118
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


# AST-037 — 实现 Worker-driven timer heap 与 sleep eligibility

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-079)
Milestone: v0.5.0
Blocked by: AST-024, AST-033, AST-034
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-079 [primary] — Coroutine Timer 由Worker驱动且Wake Time只限定eligibility；source: D-126, D-127, D-128

## What to build

每 Runtime 使用 Worker 驱动的 steady-clock timer 结构；Wake Time 只是 Ready eligibility 下界，到期后按 ordinary Global resume 路由。

## Invariants

- `[R-079]` `sleep_until(steady_clock::time_point)`与`sleep_for(duration)` awaiter必须使用steady-clock Wake Time、支持取消并注册到Runtime-wide indexed timer heap，由Worker在park deadline前后驱动而不得新增Timer thread；到时只使Task可Ready且经ordinary Global恢复，不保证最大jitter；timer属于原Task/Drain Closure，Graceful保留，Immediate取消恢复。 例外边界：饱和到time_point::max的timer可使Graceful无界等待。

## Test-first seam

- Public seam: `sleep_for/sleep_until`内建Coroutine等待。
- RED evidence: 使用 fake clock 测试早醒禁止、同 deadline、多 timer cancel、shutdown 和到期后调度延迟不构成语义失败。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-079]` Runtime无额外timer线程，Wake Time前不因该timer恢复，取消可撤销heap entry。

## Out of scope

- 不实现 I/O Runtime、Timer Wheel、inline foreign resume 或未批准的组合 API。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-079
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-126, D-127, D-128
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


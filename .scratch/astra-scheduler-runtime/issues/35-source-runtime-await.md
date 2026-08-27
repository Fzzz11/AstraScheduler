# AST-035 — 实现 source-Runtime await 与受限组合 API

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-076, R-078)
Milestone: v0.5.0
Blocked by: AST-012, AST-033
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-076 [primary] — Astra await 仅通过 source Runtime 异步恢复；source: D-120, D-121, D-122, D-147
- R-078 [primary] — Blocking/async组合API不扩张；source: D-125

## What to build

TaskHandle/GraphRun await completion 只向 source Runtime 发布 continuation；提供已批准的 blocking/async 组合面，不新增隐式 inline 或多套同义 API。

## Invariants

- `[R-076]` 左值TaskHandle/GraphRun的`co_await`必须注册continuation并只经awaiter所属source Runtime排队恢复，不inline resume、不让source执行target Runtime；TaskHandle传播同一Outcome，GraphRun返回同一Report，self-task/self-run拒绝；`cancellation_point`不挂起，`yield`必须总是挂起当前segment并经ordinary Global排队后恢复。 例外边界：target已完成时await_ready可不挂起；Ready destination服从R-063。
- `[R-078]` 当前稳定Task/Graph同步与Coroutine API不得增加wait_until、带stop_token的blocking wait或callback completion注册接口。 例外边界：后续accepted decision可新增非冲突能力。

## Test-first seam

- Public seam: 已spawn Astra Coroutine内的组合await。；TaskHandle、GraphRun与Finalization以外的completion surface。
- RED evidence: 先写 same/cross-runtime await、目标先完成、source shutdown eligibility 和负向 public compile tests。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-076]` await不形成跨Runtimesteal或递归resume，yield产生可见调度边界。
- [ ] `[R-078]` public API inventory只有wait/wait_for/get/co_await等已批准入口。

## Out of scope

- 不实现 I/O Runtime、Timer Wheel、inline foreign resume 或未批准的组合 API。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-076, R-078
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-120, D-121, D-122, D-147, D-125
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


# AST-032 — 实现 cold Coroutine Task 与 spawn 强保证

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-073)
Milestone: v0.5.0
Blocked by: AST-009, AST-013
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-073 [primary] — Coroutine Task cold且spawn强保证移交frame；source: D-114, D-115

## What to build

定义 cold `Task<T>`；`spawn` 成功才把 frame/Task identity 移交 Runtime，失败保持调用方可安全销毁且不部分发布。

## Invariants

- `[R-073]` `astra::Task<T>` 必须是initial-suspend的cold、move-only、single-shot frame owner；`spawn/try_spawn`成功把frame一次移交Runtime并返回统一TaskHandle，admission失败保持调用方Task/frame可销毁或重试且不执行body。 例外边界：coroutine function call时frame allocation/parameter copy异常发生在spawn前。

## Test-first seam

- Public seam: C++20 Coroutine创建与Runtime admission。
- RED evidence: 先写 cold-before-spawn、move-only frame、admission failure、exactly-once frame destruction 和 result propagation。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-073]` body只在Worker首次resume执行，frame始终恰有一个owner。

## Out of scope

- 不实现 I/O Runtime、Timer Wheel、inline foreign resume 或未批准的组合 API。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-073
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-114, D-115
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending
 

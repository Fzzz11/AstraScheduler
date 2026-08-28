# AST-036 — 将 Coroutine Graph Node 绑定同一 Node Task identity

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-077)
Milestone: v0.5.0
Blocked by: AST-028, AST-029, AST-032, AST-035
Status: done
Claimed by: Agent

## Rules and decisions

- R-077 [primary] — Graph Coroutine Node 复用同一Node Task identity；source: D-123, D-124

## What to build

Graph coroutine node 从首次运行到多次 resume 复用同一 GraphRunId+NodeId+TaskId，Terminal publication 只发生一次。

## Invariants

- `[R-077]` TaskGraph必须以显式 `emplace_coroutine(Task<void>&&)` 接受cold Coroutine并绑定同一NodeId/TaskId，不创建child Handle、第二identity、额外slot或outstanding count；普通emplace不得隐式unwrap Task，`Task<T>`本身不得直接co_await而必须先spawn。 例外边界：非voidCoroutine Node编译期拒绝。

## Test-first seam

- Public seam: DAG与Coroutine组合及Task ownership。
- RED evidence: 先写多 suspension node、异常/取消、dependent release 和逻辑 ID 稳定性测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-077]` Graph coroutine node在Metrics/Trace/Outcome中只计一个Task identity。

## Out of scope

- 不实现 I/O Runtime、Timer Wheel、inline foreign resume 或未批准的组合 API。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-077
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-123, D-124
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: `tests/test_graph_coroutine_identity.cpp` (34/34 tests passed in `build/wsl-gcc-debug` and `build/wsl-gcc-asan`, 0 leaks, 0 data races).


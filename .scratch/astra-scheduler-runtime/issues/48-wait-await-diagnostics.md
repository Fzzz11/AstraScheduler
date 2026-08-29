# AST-048 — 接入 wait/await 与 unobserved failure 诊断

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-060, R-096)
Milestone: v0.7.0
Blocked by: AST-012, AST-031, AST-035, AST-043, AST-046
Status: done
Claimed by: agent

## Rules and decisions

- R-060 [primary] — 未观察失败仅按启用观测面诊断；source: D-081, D-082, D-120, D-151
- R-096 [primary] — Wait/Await edge 可观察但不形成在线依赖图；source: D-050, D-051, D-149

## What to build

记录 task/graph waits、timeouts、Helping、await 与拒绝的 metrics/trace；未观察异常只在已启用观测面诊断，不维护在线 wait-for graph、不改执行语义。

## Invariants

- `[R-060]` Exception Outcome 在首次get/await传播前必须幂等标记observed；最终shared state释放时若仍未观察，仅在Metrics Basic/Detailed增加稳定 `unobserved_failures`，并仅在活动Trace可用时尽力发事件，不得terminate、默认日志、回调、级联取消或维持Metrics Off隐藏计数。 例外边界：wait/state/wait_for不标记observed。
- `[R-096]` Runtime Metrics必须记录task/graph waits、timeouts、same/cross-runtime Helping、Coroutine awaits与self/depth rejection，Detailed记录duration histogram；Trace在启用时发WaitBegin/End和AwaitArmed/Triggered/Resumed并携带source/target logical IDs，但Runtime不得据此维护在线wait-for graph或自动解环。 例外边界：Metrics Off/Trace disabled按各自fast path。

## Test-first seam

- Public seam: 普通Task与R-072的Graph真实Failed Node。；TaskHandle/GraphRun同步与Coroutine await observability。
- RED evidence: 先写 trace 离线重建 same/cross-runtime edge、Detailed duration bucket、Off 无诊断和 unobserved exception 不终止进程。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-060]` 关闭Metrics/Trace没有隐藏输出，启用时未观察失败可计数而不改变执行。
- [x] `[R-096]` 离线trace可重建wait edge，运行语义不受诊断启发式改变。

## Out of scope

- 不让 Metrics/Trace 改变调度语义，不做在线 wait-for graph、后台 Trace 文件 I/O 或每 Task 默认日志。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-060, R-096
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-081, D-082, D-120, D-151, D-050, D-051, D-149
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: `tests/test_wait_await_diagnostics.cpp`（8 用例：外部 wait 计数+thread_wait_duration+timeout、same/cross-runtime helping wait、coroutine await registrations+duration、direct self-wait rejection、helping depth rejection（deterministic gate 模式）、graph wait calls、trace WaitBegin/End+AwaitArmed/Triggered/Resumed 离线 edge 重建、unobserved failure Basic/Off/Trace 三态）Debug 46/46 与 ASan 46/46 通过；`tools/check_release_gates.py` 15/15 通过；traceability 校验通过。schema 演进：TraceEvent 扩展 target identity 双端字段（80→96 bytes，AST-046 golden 同步更新，pre-freeze 同 milestone 兼容演进）+ 新增 UnobservedFailure=38 事件（向后兼容新增）。
 

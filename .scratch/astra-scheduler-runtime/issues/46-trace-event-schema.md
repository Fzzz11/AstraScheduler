# AST-046 — 固定 versioned TraceEvent 与逻辑 ID 关联

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-087)
Milestone: v0.7.0
Blocked by: AST-004, AST-045
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-087 [primary] — TraceEvent 使用版本化固定记录与逻辑ID；source: D-139, D-153

## What to build

定义固定记录布局、schema version、category、steady timestamp 和 RuntimeId/TaskId/GraphRunId/NodeId 关联，不写对象地址作 identity。

## Invariants

- `[R-087]` TraceEvent必须是trivially-copyable固定schema，含schema_version、capture-relative steady timestamp、EventKind、Producer/local sequence、RuntimeId/WorkerId/TaskId及可选GraphRunId/NodeId/SegmentSequence和Priority/source/TaskState/Outcome/reason/deadline枚举；EventKind至少覆盖admission/rejection、Ready/claim/first-start/segment-end/Terminal/cancel、Local/Global/steal-success、park/wake、suspend/resume/yield、timer register/fire/cancel、Graph accepted/terminal/dependency release、deadline met/miss、wait/await与runtime handoff/join/finalization。枚举值显式版本化，不得保存raw pointer、用户字符串/payload或异常文本；每producer timestamp不降且sequence递增，跨producer只按(timestamp,ProducerId,sequence)确定merge且不宣称全局线性化。 例外边界：invalid sentinel用于缺失identity。

## Test-first seam

- Public seam: Task/queue/steal/wait/coroutine/timer/graph/deadline/runtime/finalization事件。
- RED evidence: 先写 layout/version golden test、跨 Runtime identity、Graph coroutine identity 和 event decode round-trip。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-087]` 地址复用不造成identity冲突，相同snapshot可确定重放排序。

## Out of scope

- 不让 Metrics/Trace 改变调度语义，不做在线 wait-for graph、后台 Trace 文件 I/O 或每 Task 默认日志。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-087
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-139, D-153
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


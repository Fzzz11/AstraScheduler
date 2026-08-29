# AST-042 — 实现 Runtime Metrics level 与 Basic event schema

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-084)
Milestone: v0.7.0
Blocked by: AST-004, AST-010, AST-015
Status: done
Claimed by: agent

## Rules and decisions

- R-084 [primary] — Metrics level 与 Basic事件schema固定；source: D-135, D-136, D-151

## What to build

实现 Off/Basic/Detailed 冻结配置、分片饱和 counters 和固定 Basic 状态/队列/调度/失败事件 schema，Off 热路径近零开销。

## Invariants

- `[R-084]` Scheduler必须提供Off/Basic/Detailed且默认Basic；Off不维护measurement。Basic固定counter为submission_attempts、accepted_task_identities、rejected_lifecycle、rejected_capacity、blocking_submit_waits/wakeups、first_starts、resume_segments、succeeded、failed、cancelled_before_start、cancelled_cooperative、unobserved_failures、global/local_claims、steal_attempts/successes/failures、worker_parks/wakes、explicit_yields、coroutine_suspends、timer_registrations/fires/cancellations、graph_admission_attempts/runs_accepted/runs_rejected/nodes_terminal、deadline_admitted/met/missed/cancelled_before_start；固定gauge为waiting/ready/running/suspended_tasks、external_pending_slots_used、parked_workers、active_timer_entries、active_graph_runs。字段以per-worker/external/control shard饱和到UINT64_MAX并sticky saturated，不得有TaskId/NodeId/字符串高基数label。 例外边界：process-wide生命周期指标由R-095独立提供且始终可用。

## Test-first seam

- Public seam: per-Runtime Metrics热路径与schema。
- RED evidence: 先写 Off 无更新、Basic counter 守恒、饱和标记和 quiescent point 精确断言。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-084]` Metrics启用不改变Task语义，长期counter不wrap倒退。

## Out of scope

- 不让 Metrics/Trace 改变调度语义，不做在线 wait-for graph、后台 Trace 文件 I/O 或每 Task 默认日志。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-084
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-135, D-136, D-151
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: `tests/test_runtime_metrics.cpp` (40/40 passed in debug and ASan builds), `tools/check_release_gates.py` (15/15 passed).



# AST-004 — 固定 Scheduler 公共 policy、状态、逻辑 ID 与 capability

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-098, R-099, R-100, R-101)
Milestone: v0.1.0
Blocked by: AST-002, AST-003
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-098 [primary] — SchedulerOptions 只公开稳定policy且startup冻结；source: D-078, D-157
- R-099 [primary] — Scheduler status 是state/mode成对快照；source: D-160
- R-100 [primary] — 公共逻辑ID强类型且不wrap/reuse；source: D-153, D-161
- R-101 [primary] — SchedulerCapabilities 报告实际Local Deque backend；source: D-101, D-167, D-162

## What to build

定义稳定 `SchedulerOptions`、冻结后的 resolved snapshot、成对 status snapshot、强类型不复用逻辑 ID，以及真实 Local Deque backend capability；v0.1 报告 `None`。

## Invariants

- `[R-098]` SchedulerOptions必须使用 `recommended_worker_count()`、external capacity 65536、Reject、helping64、local burst64、steal probes8、Metrics Basic及空TraceCollector默认；所有size值必须大于0且unknown enum拒绝，options在注册/ID/Worker前验证并冻结，spin/deque/timer/priority内部tuning不得公开。 例外边界：recommended_worker_count的hardware_concurrency为0时返回1。
- `[R-099]` 有效Scheduler的 `status()`必须非阻塞、无副作用且一次线性化返回仅有Running+None、Stopping+Graceful/Immediate、Stopped+最终mode的pair；不得提供独立is_running/is_stopped/mode getter，snapshot可立即过时且不授予admission能力，空Scheduler抛logic_error。 例外边界：process Finalization state由R-095观察。
- `[R-100]` RuntimeId、TaskId、GraphRunId与NodeId必须是default-zero-invalid、trivially-copyable强值类型，支持valid/equality/order/hash且无隐式整数/指针转换；有效Scheduler::runtime_id、TaskHandle::id、GraphRun::id与GraphReport::run_id返回对应稳定值。Runtime/Task/GraphRun sequence为checked nonzero monotonic不wrap，NodeId仅graph-local；完整运行Node关联GraphRunId+NodeId+TaskId，ID不授予lookup/control/lifetime。 例外边界：sequence gap允许，跨进程不承诺唯一。
- `[R-101]` 有效Scheduler的 `capabilities()`必须返回不可由用户aggregate-initialize的trivially-copyable immutable `SchedulerCapabilities`，其 `local_deque_backend()`为 `LocalDequeBackend::{None,Locked,ChaseLevLockFree}`之一且 `lock_free_local_deque()`仅最后一种为true；v0.1为None、v0.2/fallback为Locked，Stopped后保留且不得运行时切换或按版本推断，空Scheduler抛logic_error。 例外边界：该能力不声称整个Runtime lock-free。

## Test-first seam

- Public seam: Scheduler startup配置与Metrics/Benchmark resolved options。；Scheduler lifecycle观察。；Scheduler/Handle/GraphReport/Metrics/Trace/Benchmark identity。；Runtime、Trace metadata与Benchmark artifact。
- RED evidence: 先写 public compile tests、Options 修改不回写 Runtime、状态成对观察、ID 不混用及 v0.1 capability 测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-098]` invalid配置在无Runtime副作用前抛invalid_argument，调用方后改原options不影响Runtime。
- [ ] `[R-099]` 并发shutdown时不返回撕裂pair，submit仍以自身transaction决定结果。
- [ ] `[R-100]` 地址复用不改变身份，耗尽在startup/admission前抛overflow_error而不复用。
- [ ] `[R-101]` 同版本不同atomic平台可诚实报告不同backend，artifact复用同一snapshot。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-098, R-099, R-100, R-101
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-078, D-157, D-160, D-153, D-161, D-101, D-167, D-162
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending

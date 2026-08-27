# AST-005 — 实现 Scheduler startup transaction 与 Finalization gate 排序

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-023, R-024, R-097)
Milestone: v0.1.0
Blocked by: AST-004
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-023 [primary] — Reaper handoff 能力先于 Worker 启动；source: D-019
- R-024 [primary] — 运行期 handoff 不获取可失败资源；source: D-019
- R-097 [primary] — Scheduler startup transaction 与 Finalization close 唯一排序；source: D-023, D-024, D-155, D-156

## What to build

将配置验证、Reaper registration/handoff 预留、Worker 创建和 Running 发布组织成同步强事务；与永久 registration close 建立唯一线性化顺序。

## Invariants

- `[R-023]` Runtime 必须在任何 Worker 启动前建立并预留 Reaper handoff 能力；准备失败时启动必须失败、不得发布 `Running`，且不得留下活动 Worker。
- `[R-024]` 进入 `Running` 后，Worker 最后 Handle 的 handoff 必须为 `noexcept`、不分配内存、不创建线程，并且不得等待 Drain Work Closure、Worker 退出或 Shutdown Completion。 例外边界：可以使用内部同步完成线性化。
- `[R-097]` `Scheduler(options)`必须同步验证、预留/注册Reaper、创建Worker并经barrier一次发布Running后才返回，失败阻止用户工作并完整join/rollback；Running publication与Finalization close线性排序，close先则startup不开放admission并回滚后抛FinalizationStarted creation rejection，Running先则构造成功且随后可立即Graceful Stopping。 例外边界：无Runtime的空集合begin可直接Finalized。

## Test-first seam

- Public seam: Scheduler 启动事务。；R-021 的运行期所有权移交路径。；Scheduler创建、Finalization核算集合与startup race。
- RED evidence: 用可注入失败点先覆盖每个 startup 阶段回滚、无公开 Starting 状态、close/start 竞态仅有两个合法结果。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-023]` 注入注册、预留或 coordinator 建立失败时，用户任务从未获得执行窗口。
- [ ] `[R-024]` 资源耗尽故障注入不会让已经 Running 的 handoff 丢失 Runtime State 所有权。
- [ ] `[R-097]` 不存在public Created/Starting或半启动Handle，竞态Scheduler恰好成功纳入或零用户工作失败。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-023, R-024, R-097
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-019, D-023, D-024, D-155, D-156
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


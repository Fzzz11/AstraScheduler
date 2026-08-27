# AST-006 — 解耦 Runtime State 并实现最后 Worker Handle handoff

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-020, R-021, R-022)
Milestone: v0.1.0
Blocked by: AST-005
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-020 [primary] — Handle 生命周期与共享 Runtime State 解耦；source: D-017
- R-021 [primary] — 目标 Worker 最后 Handle 通过 Reaper handoff 返回；source: D-017, D-018
- R-022 [primary] — Worker orphan handoff 保留 Graceful 默认；source: D-018

## What to build

用共享 Runtime State 承载执行身份；最后 Handle 在目标 Worker 上释放时只做预留好的 orphan handoff，并保留 Graceful 默认策略后立即返回。

## Invariants

- `[R-020]` Scheduler Handle 的生命周期必须与共享 Runtime State 解耦，Runtime State 必须存活到所有 Worker 停止访问、线程完成 join 且最终回收结束。
- `[R-021]` 目标 Scheduler Worker 销毁最后一个 Handle 时，析构必须在线性化地移交 Runtime State 强所有权给非目标 Worker Reaper 后立即返回，不得 self-wait、self-join、detach、伪造 Shutdown Completion 或仅因该场景 terminate。 例外边界：已 Stopped 时仍由安全非 Worker 路径完成必要 join/reclamation。
- `[R-022]` R-021 发生时，`Running` Runtime 必须请求 Graceful Shutdown，`Stopping` Runtime 必须保留当前 Shutdown Mode，`Stopped` Runtime 只进入安全 join/final reclamation。 例外边界：并发合法 `shutdown_now()` 可按 R-014 的顺序升级模式。

## Test-first seam

- Public seam: Scheduler Handle、Worker 执行引用与 Reaper 所有权。；同 Scheduler Worker 上的最后 Handle 析构。；Worker 最后 Handle 析构时的 Runtime 状态分支。
- RED evidence: 先写 Worker 内释放最后 Handle 的确定性测试，验证无 self-join、无悬空访问、handoff 后任务仍可完成。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-020]` 最后 Handle 消失不会导致仍在访问的 Worker 发生 use-after-free。
- [ ] `[R-021]` Worker 任务可释放最后 Handle 并继续返回，Runtime State 保持有效且后续真实完成。
- [ ] `[R-022]` Worker handoff 与非 Worker 析构表达相同的默认 Graceful 意图，但前者异步返回。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-020, R-021, R-022
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-017, D-018
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


# AST-007 — 实现唯一 Reaper coordinator 的 pending/join/idle 循环

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-025, R-026, R-028, R-107)
Milestone: v0.1.0
Blocked by: AST-006
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-025 [primary] — Pending Runtime 不阻塞 Reaper；source: D-020
- R-026 [primary] — Join Ready 后唯一 join 并发布 Stopped；source: D-020
- R-028 [primary] — Reaper 空闲时保持同一服务；source: D-022
- R-107 [primary] — Supported Configuration 只有一个实现实例与Reaper coordinator；source: D-021, D-159

## What to build

建立单 implementation instance 下的唯一 coordinator；Pending Runtime 不阻塞其他回收，只在 Join Ready 后唯一 join、发布 Stopped，空闲不重启服务。

## Invariants

- `[R-025]` Reaper 可以持有 Pending Runtime State，但不得阻塞等待其任务或 Drain Work Closure；永久 Pending Runtime 不得阻塞其他 Join Ready Runtime 的回收。
- `[R-026]` 仅当 Runtime 单调进入 Join Ready 后，Reaper 才能认领 join；Reaper 与同步关停路径之间只能有一个 join owner，且 `Stopped`/Shutdown Completion 只能在全部 Worker 实际 join 后发布。
- `[R-028]` Reaper Service 首次成功建立后必须在空闲时阻塞等待并保持同一 coordinator，不得因最后一个 Runtime 消失、队列为空或空闲超时而自动停止或重建。例外边界：显式 Reaper Finalization 由 approved Spec 中对应的 active Finalization rules 覆盖。
- `[R-107]` R-111定义的Linux-only Supported Configuration中，一个进程必须只加载一个Astra Implementation Instance，并由其中一个逻辑Reaper Service和恰好一条不属于任何Scheduler的专用coordinator服务全部Runtime；不得按Scheduler/handoff增加Reaper线程，coordinator不得执行用户任务或参与steal；多个DSO各自静态嵌入实现不享有这些process-wide保证。 例外边界：从未建立服务且空集合Finalization不创建coordinator。

## Test-first seam

- Public seam: Reaper 同时核算一个或多个 orphan Runtime State。；Runtime 回收和 join ownership 竞态。；RegistrationOpen 阶段的进程级 Reaper Service。；process-wide Reaper、Finalization gate、ID allocator与Process Metrics拓扑。
- RED evidence: 用两个受控 Runtime 证明一个长期 Pending 不阻塞另一个 Join Ready，且并发 handoff 只有一次 join/coordinator。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-025]` 一个永久任务所在 Runtime 长期 Pending 时，其他 Runtime 仍能 join 并发布 Stopped。
- [ ] `[R-026]` Join Ready 本身不会提前满足完成，Worker 也不会等待 Reaper 先 join 而形成循环等待。
- [ ] `[R-028]` 多轮 Scheduler 创建/销毁复用同一休眠 coordinator，空闲时不 busy-spin 或强持有已完成 Runtime。
- [ ] `[R-107]` 支持配置中Scheduler数量不增加coordinator数，unsupported duplicate instance被部署文档/测试明确拒绝。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-025, R-026, R-028, R-107
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-020, D-022, D-021, D-159
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending

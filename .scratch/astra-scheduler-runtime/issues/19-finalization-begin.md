# AST-019 — 实现 begin_finalization、核算集合与 startup 竞态

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-031, R-037, R-038, R-104)
Milestone: v0.1.0
Blocked by: AST-005, AST-018
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-031 [primary] — begin_finalization 只发布开始请求；source: D-026
- R-037 [primary] — begin_finalization 幂等共享唯一世代；source: D-032
- R-038 [primary] — begin 与 request_immediate 可由任意应用线程请求；source: D-033
- R-104 [primary] — Finalization 对已核算与启动中 Runtime 使用 Graceful；source: D-024, D-156

## What to build

`begin_finalization()` 幂等返回唯一世代，永久关闭注册，立即返回；对已注册和赢得 startup 核算竞态的 Runtime 请求 Graceful。

## Invariants

- `[R-031]` `begin_finalization()` 必须完成永久注册关闭、初始 Graceful/sticky 请求可靠记录和 coordinator 通知后立即返回，不得等待 Runtime drain、Join Ready、Worker/coordinator join、`Stopped` 或 `Finalized`。 例外边界：空核算集合可以在同一调用内真实完成，见 R-037。
- `[R-037]` 首次 `begin_finalization()` 必须建立唯一 Finalization Completion；并发、Finalizing 或 Finalized 后的重复调用必须返回关联同一世代的控制对象且不重复副作用；从未建立 Reaper 且核算集合为空时必须永久关闭注册并直接完成，不得创建 coordinator。
- `[R-038]` `begin_finalization()` 与 `request_immediate()` 必须允许任意应用线程调用，包括任意 Scheduler Worker；两者只完成线性化、可靠记录和通知，不得等待 Runtime drain、Worker/coordinator 退出或 join。 例外边界：允许短暂内部同步，不形成 lock-free、wait-free 或固定时延承诺。
- `[R-104]` Finalization必须对close前已核算Runtime请求Graceful且不降级既有Immediate；close先于Running publication的startup必须在开放用户工作前观察sticky请求并rollback，Running先发布则构造成功并作为核算成员可立即进入Graceful Stopping。 例外边界：显式shutdown_now/request_immediate可单向升级。

## Test-first seam

- Public seam: 首次和幂等重复 begin；重复语义见 R-037。；所有进程级 begin 调用。；两个 Finalization 请求式命令。；Finalization accounted set与Scheduler startup race的shutdown mode。
- RED evidence: 先写多线程 begin、begin/startup 线性化、已核算 Runtime、调用线程身份及“begin 不等待完成”测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-031]` begin 返回只证明 Finalization 已不可逆开始；活动 Runtime 可继续在后台推进。
- [ ] `[R-037]` 多个 begin 获得的控制对象观察同一 Completion，空进程 begin 后 Completed 且无 Reaper thread。
- [ ] `[R-038]` Worker 可发起全局终结或升级后继续完成当前任务，不产生 self-wait。
- [ ] `[R-104]` Finalization不因进程收尾默认取消已接受工作，半启动Runtime不获得用户执行窗口。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-031, R-037, R-038, R-104
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-026, D-032, D-033, D-024, D-156
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


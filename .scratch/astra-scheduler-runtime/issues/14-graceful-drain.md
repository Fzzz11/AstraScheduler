# AST-014 — 实现 Graceful admission closure 与 Drain Work Closure

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-006, R-007, R-012, R-019)
Milestone: v0.1.0
Blocked by: AST-010, AST-011
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-006 [primary] — Graceful Stopping 接受授权的 Internal Submission；source: D-002
- R-007 [primary] — Graceful 转换线性化关闭 External Submission；source: D-003
- R-012 [primary] — 非 Worker graceful shutdown 排空传递闭包；source: D-010
- R-019 [primary] — Stopped 是关停吸收状态；source: D-016

## What to build

线性化关闭 External Submission，继续接受获授权 Internal Submission，排空传递闭包并把 Stopped 作为吸收状态。

## Invariants

- `[R-006]` Graceful Stopping 期间，Runtime 必须继续接受由同一 Scheduler 当前正在执行的已接受任务发起的 Internal Submission，并把这些任务纳入同一次 Drain Work Closure。 例外边界：其他线程和其他 Scheduler Worker 的提交属于 External Submission；Immediate Stopping 不适用。
- `[R-007]` External Submission 与 `Running → Stopping` 必须形成单一线性化顺序；转换前线性化的提交被接受并计入 Drain Work Closure，转换后线性化的提交被拒绝且不得入队或增加 outstanding work。 例外边界：R-006 授权的 Internal Submission。
- `[R-012]` 非目标 Scheduler Worker 调用 `shutdown()` 时，方法必须等待整个 Drain Work Closure 终结、全部 Worker 退出并 join、`Stopped` 发布后返回；闭包无法终结时可无限阻塞。 例外边界：目标 Scheduler Worker 由 R-013 覆盖。
- `[R-019]` `Stopped` 发布后，任何线程调用 `shutdown()` 或 `shutdown_now()` 必须成功且立即无副作用返回，不得创建新 Shutdown Completion、改变历史 Shutdown Mode、重复 join、重启 Runtime 或改写任务终态。

## Test-first seam

- Public seam: 同 Scheduler Worker 在 Graceful Stopping 中的任务派生。；External Submission 与 Graceful Shutdown 的竞态。；普通应用线程以及其他 Scheduler 的 Worker对目标 Scheduler 的调用。；已 Stopped 的 Scheduler Runtime。
- RED evidence: 先写关停边界前后 external/internal 提交、递归 internal fan-out、重复 shutdown 和 Stopped 后操作测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-006]` 已接受父任务在 Graceful Stopping 中提交的子任务得到正常终态，Shutdown Completion 晚于该子任务终结。
- [ ] `[R-007]` 每个竞态提交恰好落在 accepted 或 rejected 一侧，不产生孤儿任务或提前关停。
- [ ] `[R-012]` 关停不会以队列瞬时为空提前返回，返回后所有 Worker 已 join。
- [ ] `[R-019]` 两种关停 API 在 Graceful/Immediate 完成后均稳定立即返回且状态不变。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-006, R-007, R-012, R-019
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-002, D-003, D-010, D-016
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


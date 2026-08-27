# AST-015 — 实现 shutdown caller guard 与共享完成边界

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-010, R-011, R-013, R-016, R-108)
Milestone: v0.1.0
Blocked by: AST-007, AST-014
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-010 [primary] — 非 Worker shutdown_now 同步完成；source: D-008
- R-011 [primary] — 目标 Worker 的 shutdown_now 无副作用失败；source: D-009
- R-013 [primary] — 目标 Worker 的 graceful shutdown 无副作用失败；source: D-011
- R-016 [primary] — 并发非 Worker 关停共享一次完成；source: D-013
- R-108 [primary] — 同 Runtime Worker self-shutdown 抛 logic_error；source: D-009, D-011, D-166

## What to build

非 Worker shutdown 同步等待同一次完成；同 Runtime Worker 调用两种 shutdown 均无副作用抛 `logic_error`；并发调用共享 join/Stopped 发布。

## Invariants

- `[R-010]` 非目标 Scheduler Worker 调用 `shutdown_now()` 时，方法必须在全部 Worker 退出并各自完成 join、`Stopped` 发布后才返回；不合作 Running Task 可使调用无限阻塞。 例外边界：目标 Scheduler Worker 由 R-011 覆盖。
- `[R-011]` 目标 Scheduler Worker 调用该 Scheduler 的 `shutdown_now()` 必须在状态转换、任务取消、stop request、admission 或 outstanding-work 改变之前同步失败。 例外边界：`Stopped` 后调用由 R-019 覆盖；其他 Scheduler Worker 按 R-010 处理。
- `[R-013]` 目标 Scheduler Worker 调用该 Scheduler 的 `shutdown()` 必须在状态转换、admission 关闭、outstanding-work 改变或等待开始之前同步失败。 例外边界：`Stopped` 后调用由 R-019 覆盖；其他 Scheduler Worker 按 R-012 处理。
- `[R-016]` 同一次关停中的所有非 Worker `shutdown()`/`shutdown_now()` 必须幂等参与同一个 Shutdown Completion，每个调用在 `Stopped` 后返回，且每个 Worker 线程恰好被 join 一次。 例外边界：R-014 允许一次 Graceful → Immediate 升级；目标 Worker 调用由 R-011/R-013 拒绝。
- `[R-108]` `void Scheduler::shutdown()`与`void Scheduler::shutdown_now()`不得标记noexcept；当前同Runtime Worker调用任一方法必须在读取/改变lifecycle、关闭admission、发布stop、取消Task、认领join或等待前抛 `std::logic_error`，异常文本不稳定；其他Runtime Worker仍按目标Runtime的非Worker同步语义执行。 例外边界：empty/moved-from Scheduler也按R-103抛logic_error，但原因不通过enum区分。

## Test-first seam

- Public seam: 普通应用线程以及其他 Scheduler 的 Worker 对目标 Scheduler 的调用。；当前正在执行目标 Scheduler 任务的 Worker。；同一个 Runtime 的重复和并发关停调用。；Scheduler shutdown caller classification与公共异常边界。
- RED evidence: 先写 Worker/非 Worker、并发 graceful/now caller、异常前后状态和唯一 join 的确定性测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-010]` 返回后没有目标 Worker 执行用户代码或访问目标 Runtime；忽略 stop 的任务可保持调用未返回。
- [ ] `[R-011]` self-call 不进入 Shutdown Completion，不改变 Runtime 状态或任何任务结果。
- [ ] `[R-013]` self-call 不截断当前任务的派生权限，也不参与 Shutdown Completion。
- [ ] `[R-016]` 大量并发调用不会重复取消、重复发布 stop request、并发 join 或提前返回。
- [ ] `[R-108]` same-runtime Worker得到logic_error且状态不变，other-runtime Worker仍等待目标真实Stopped。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-010, R-011, R-013, R-016, R-108
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-008, D-009, D-011, D-013, D-166
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


# AST-010 — 实现 External Pending Capacity 与强 admission transaction

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-061, R-062)
Milestone: v0.1.0
Blocked by: AST-009
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-061 [primary] — External Pending Capacity 与 backpressure 固定；source: D-083, D-084, D-085, D-086
- R-062 [primary] — submit/try_submit 共享强 admission transaction；source: D-087, D-088, D-089, D-155

## What to build

为 External Submission 实现 pending 配额、`submit` backpressure 和 `try_submit` 非阻塞失败；二者共享一次性强事务并正确回滚资源与配额。

## Invariants

- `[R-061]` 每Runtime必须以正数 `external_pending_capacity` 限制已接受但未首次Running的External工作，默认65536；slot在admission占用并在首次start或start前Terminal释放，Internal不占用；容量策略仅为默认Reject或Block，Block只允许普通非Worker且必须在slot/gate竞态下无丢唤醒，CallerRuns不得提供。 例外边界：try_submit永不等待；started后Coroutine suspension不重新占slot；Block等待者不保证FIFO或公平顺序。
- `[R-062]` `submit`与`try_submit`必须在同一强异常安全事务中完成gate、slot、capture/TCB、ID、outstanding与不可丢失publication；成功返回真实TaskHandle，失败完全回滚且不执行Callable；最终 `SubmissionError` 仅为 Stopping/Stopped/CapacityExhausted，submit抛`submission_rejected`，try_submit即时返回variant alternative，其他构造/分配异常保持原类型。 例外边界：空Scheduler为logic_error，Finalization启动拒绝由R-097的creation error表达。

## Test-first seam

- Public seam: 普通Task与R-070的External Graph admission。；Callable与Coroutine spawn的Runtime admission。
- RED evidence: 注入容量耗尽、分配失败、close 竞态，先验证失败不留下 Task identity、调度引用或容量泄漏。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-061]` External未启动工作有界，Worker不会因Block自锁，关闭gate能唤醒并拒绝等待者。
- [ ] `[R-062]` 不存在orphan Handle、泄漏slot/outstanding或已拒绝却执行的Callable。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-061, R-062
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-083, D-084, D-085, D-086, D-087, D-088, D-089, D-155
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


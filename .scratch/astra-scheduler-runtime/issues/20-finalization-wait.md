# AST-020 — 实现 Finalization 无界 wait、wait_for 与唯一 coordinator join

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-032, R-033, R-039, R-040, R-041, R-042)
Milestone: v0.1.0
Blocked by: AST-007, AST-019
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-032 [primary] — wait 只观察真实 Finalization Completion；source: D-027
- R-033 [primary] — wait_for 超时不改变 Finalization；source: D-028
- R-039 [primary] — 任意 Scheduler Worker 调用 wait 抛出 logic_error；source: D-034
- R-040 [primary] — 任意 Scheduler Worker 调用 wait_for 抛出 logic_error；source: D-035
- R-041 [primary] — wait_for 使用 steady_clock 与唯一边界顺序；source: D-036
- R-042 [primary] — 合法等待者唯一 join coordinator 后发布完成；source: D-037

## What to build

`wait()` 仅在真实 Finalization Completion 后返回；`wait_for` 使用 steady clock，超时不改变策略；Worker caller 抛错；合法等待者共享唯一 join。

## Invariants

- `[R-032]` 合法调用的 `wait()` 必须在全部已核算 Runtime 达到 Shutdown Completion 并解除注册、Reaper 工作清空、coordinator 退出并 join、`Finalized` 发布后返回；它可无限阻塞，且不得升级模式、detach、伪造完成或创建新终结世代。 例外边界：Worker 调用由 R-039 拒绝。
- `[R-033]` 合法 `wait_for(timeout)` 仅在真实 Finalization Completion 达成时返回 `Completed`，否则在期限结果线性化后返回 `TimedOut`；`TimedOut` 不得发布 `Finalized`、恢复注册、重启/停止 Reaper、升级模式、detach 或创建新终结世代，后台必须继续推进同一次 Finalization。 例外边界：精确 clock 与边界竞态见 R-041；Worker 调用见 R-040。
- `[R-039]` 任意 AstraScheduler Worker 调用 `FinalizationControl::wait()` 必须在等待、join ownership 认领或 Finalization 状态改变前同步抛出 `std::logic_error`。 例外边界：普通非 Worker 按 R-032 处理。
- `[R-040]` 任意 AstraScheduler Worker 调用 `FinalizationControl::wait_for(timeout)` 必须在读取 timeout、等待或状态改变前同步抛出 `std::logic_error`，包括 timeout 为零或负值时。 例外边界：普通非 Worker 按 R-033/R-041 处理。
- `[R-041]` 合法 `wait_for(timeout)` 必须用 `std::chrono::steady_clock` 形成 deadline；timeout 小于等于零时执行一次即时无副作用观察，正 timeout 时 Completion 与 deadline 必须在同一同步域内形成唯一顺序，先观察 Completion 返回 `Completed`，先确认期限已到且 Completion 未发布返回 `TimedOut`。 例外边界：duration 到内部 deadline 的饱和转换算法属于实现选择，但不得产生未定义溢出。
- `[R-042]` coordinator 必须在工作清空后发布 `CoordinatorExited` 并退出但不得自行发布 Finalization Completion；恰好一个合法非 Worker 等待者必须在观察 Exited 后认领并执行唯一 join，再发布 `Finalized`/Completion，其他等待者只观察同一完成事件。 例外边界：没有等待者时可保持 Exited-unjoined，直到未来合法等待者完成收尾。

## Test-first seam

- Public seam: 非 Worker FinalizationControl 等待者。；非 Worker FinalizationControl 有界等待者。；全部已注册 Runtime 的 Worker，而非只检查当前 Scheduler。；全部已注册 Runtime 的 Worker。；非 Worker 的所有 `std::chrono::duration<Rep, Period>` 调用。；coordinator 退出与并发 Finalization 等待者。
- RED evidence: 先写超时后继续推进、timeout/completion 同边界顺序、多 waiter、任意 Scheduler Worker 拒绝和唯一 coordinator join。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-032]` wait 返回后进程内没有 AstraScheduler Worker 或 Reaper coordinator 存活。
- [ ] `[R-033]` 首次 TimedOut 后，同一控制对象或副本可继续等待并最终观察 Completed。
- [ ] `[R-039]` 任一 Scheduler 的 Worker 调用 wait 得到 logic_error，Completion 和唯一 join owner 不受影响。
- [ ] `[R-040]` Worker 的正、零、负 timeout 调用均抛异常而不返回 TimedOut。
- [ ] `[R-041]` wall-clock 跳变不影响等待；TimedOut 线性化后即使返回前完成，本次结果仍为 TimedOut。
- [ ] `[R-042]` 多等待者场景只有一次 coordinator join，Completed 永远晚于该 join。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-032, R-033, R-039, R-040, R-041, R-042
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-027, D-028, D-034, D-035, D-036, D-037
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


# AST-016 — 实现单向 Immediate escalation 与启动状态分类

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-009, R-014, R-015, R-106)
Milestone: v0.1.0
Blocked by: AST-013, AST-015
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-009 [primary] — Immediate 对 Running Task 仅请求协作停止；source: D-007
- R-014 [primary] — Shutdown Mode 只允许 Graceful 向 Immediate 升级；source: D-012
- R-015 [primary] — Immediate 升级按启动状态分类任务并关闭内部准入；source: D-012
- R-106 [primary] — Immediate 只直接取消从未首次 start 的任务；source: D-006, D-154

## What to build

仅允许 Graceful→Immediate；关闭 internal admission，直接取消从未首次 start 的任务，对 Running/Suspended 已开始工作只请求协作停止并等待回收。

## Invariants

- `[R-009]` Immediate Shutdown 对每个 Running Task 必须发布 cooperative stop request，不得强制终止线程、注入异步异常或把 stop request 伪装为任务已终结。 例外边界：整个进程由应用选择终止不属于单 Task Runtime 终止语义。
- `[R-014]` Graceful Stopping 中的合法 `shutdown_now()` 必须在唯一线性化点把 Shutdown Mode 升级为 Immediate；Immediate Mode 不得降级为 Graceful。 例外边界：R-011/R-013 拒绝的目标 Worker 调用不参与模式变化。
- `[R-015]` R-014 的升级点之后不得接受新的 Internal Submission；升级点前已接受且从未首次 Running 的任务按 R-106 处理，当前 Running 的任务按 R-009 处理，已经启动后 Suspended 的 Coroutine 按 R-075 处理。
- `[R-106]` Immediate Shutdown线性化后，所有已接受且从未成功首次进入Running的Waiting/Ready Task必须恰好一次发布Cancelled、唤醒等待者且永不执行用户Callable/frame；已经首次start后处于Suspended的Coroutine不得按“非Running”直接完成，而按R-075发布stop并经resume segment到达合作取消或自然完成。 例外边界：当前Running Task按R-009；已Terminal不改写。

## Test-first seam

- Public seam: 已进入 Running 的用户 Callable。；进行中的 Scheduler Shutdown Completion。；Graceful → Immediate 升级前后已接受的任务。；Immediate、Graceful→Immediate与Finalization escalation的Task分类。
- RED evidence: 用 Waiting/Ready/Running/Suspended 分类夹具验证升级幂等、Outcome 和“不强杀已开始任务”。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-009]` stop-aware Callable 可自行退出；忽略 stop request 的 Callable 继续运行且阻止真实完成。
- [ ] `[R-014]` 并发升级最多发生一次，后续 `shutdown()` 不恢复任务、stop state 或 admission。
- [ ] `[R-015]` admission、task start 与 mode upgrade 的竞态可被唯一排序，不出现取消后执行或升级后新增工作。
- [ ] `[R-106]` never-started work不执行且Handle完成；already-started frame仍有机会运行取消点与RAII unwinding。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-009, R-014, R-015, R-106
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-007, D-012, D-006, D-154
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending
 

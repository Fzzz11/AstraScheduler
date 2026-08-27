# AST-017 — 实现最后非 Worker Handle 的 noexcept 同步 RAII

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-103, R-105)
Milestone: v0.1.0
Blocked by: AST-007, AST-014, AST-015
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-103 [primary] — 只有最后一个非Worker Scheduler Handle释放触发同步RAII；source: D-014, D-017, D-018, D-155
- R-105 [primary] — 最后非Worker Handle析构是noexcept同步完成边界；source: D-014, D-155

## What to build

仅最后一个非 Worker Scheduler Handle 释放触发 Graceful fallback，并作为 `noexcept` 同步完成/回收边界；非最后副本释放不关停。

## Invariants

- `[R-103]` Scheduler必须是copyable/movable shared Handle，普通副本销毁不得关停；仅最后一个Handle释放触发RAII，非目标Worker上Running发起Graceful、Stopping保留mode，目标Worker则按R-021/R-022 handoff；空/moved-from操作除valid/destruction外抛logic_error。 例外边界：已Stopped最后释放只回收；显式shutdown可先完成。
- `[R-105]` 最后一个Scheduler Handle在非目标Worker释放时，析构必须noexcept并等待Drain Closure、全部Worker join与Stopped真实发布，允许无界阻塞且不得detach或伪造完成。 例外边界：目标Worker最后释放使用R-021/R-022的异步handoff。

## Test-first seam

- Public seam: Scheduler shared Handle lifetime与RAII策略选择。；R-103选择的非Worker RAII路径。
- RED evidence: 先覆盖多 Handle 释放顺序、最后释放阻塞到真实完成、异常任务和析构路径不传播异常。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-103]` 销毁一个非最后副本不改变status/admission，最后释放才按caller选择RAII或handoff。
- [ ] `[R-105]` 析构返回后无Worker访问Runtime，不合作任务保持析构未返回。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-103, R-105
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-014, D-017, D-018, D-155
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


# AST-013 — 实现显式 Task cancellation 的首次 start 分类

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-053, R-054)
Milestone: v0.1.0
Blocked by: AST-010, AST-011
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-053 [primary] — request_cancel 按首次 start 竞态分类；source: D-052, D-053, D-054, D-055
- R-054 [primary] — Cooperative stop 的真实退出决定 Outcome；source: D-056, D-058, D-059, D-060

## What to build

`request_cancel()` 与首次 start 线性化竞争；未开始任务发布 Cancelled，已开始任务只收到 cooperative stop，最终 Outcome 由真实退出决定。

## Invariants

- `[R-053]` `void TaskHandle::request_cancel() const noexcept` 必须幂等、可并发且在请求可靠发布后立即返回；请求在线性化上先于首次 start 时 Task 直接发布 Cancelled且不执行用户代码，start 先胜出时只发布 cooperative stop request。 例外边界：empty Handle 的 request_cancel 是 R-057 的 no-op；已Terminal不改写。
- `[R-054]` Running Callable 在 stop request 后正常返回仍必须发布 Value，抛出 `task_cancelled` 才发布 Cancelled，其他异常发布 Exception；`submit` 必须优先普通invocation，仅普通形式不可调用时在首参数注入该Task的 `std::stop_token`，并提供不挂起的 `throw_if_stop_requested(token)` cancellation point。 例外边界：Coroutine内建awaiter取消见 R-075。

## Test-first seam

- Public seam: 任意线程对有效 TaskHandle 的单 Task cancellation。；stop-aware Callable invocation 与 execution boundary。
- RED evidence: 用 barrier 穷举 cancel-before-start、start-wins、忽略 stop、`task_cancelled` 退出和普通异常路径。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-053]` cancellation/start竞态只有一个分类，重复调用不重复完成或执行。
- [ ] `[R-054]` stop request本身不覆盖用户真实结果，generic callable不会意外收到token。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-053, R-054
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-052, D-053, D-054, D-055, D-056, D-058, D-059, D-060
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


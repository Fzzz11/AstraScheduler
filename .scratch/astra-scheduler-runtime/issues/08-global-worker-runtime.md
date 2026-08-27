# AST-008 — 交付 Global-only Worker Runtime 基线

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-001, R-002)
Milestone: v0.1.0
Blocked by: AST-004, AST-005
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-001 [primary] — v0.1.0 使用全局注入队列基线；source: D-001
- R-002 [primary] — v0.1.0 排除本地队列与任务窃取；source: D-001

## What to build

实现 mutex 保护的 Global Injection Queue、固定 Worker 集合和基本执行循环；所有 Ready Task 只走 Global 路径。

## Invariants

- `[R-001]` v0.1.0 的全部 Ready Task 必须通过互斥保护的 Global Injection Queue 调度。
- `[R-002]` v0.1.0 不得包含 Per-Worker Local Queue、Work Stealing 或 Chase-Lev Deque。 例外边界：文档、接口 seam 或后续版本预留不构成 v0.1.0 功能完成。

## Test-first seam

- Public seam: v0.1.0 Basic Scheduler。；v0.1.0 release scope。
- RED evidence: 先用可观察 queue seam 证明 external/internal/worker-published Ready 都进入 Global，且构建中没有 local push/pop/steal 路径。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-001]` v0.1.0 的调度路径中不存在 Ready Task 绕过 Global Injection Queue 的本地队列路径。
- [ ] `[R-002]` v0.1.0 构建和测试不执行本地 push/pop/steal 或 Chase-Lev 算法。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-001, R-002
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-001
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending
 

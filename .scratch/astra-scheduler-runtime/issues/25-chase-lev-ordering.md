# AST-025 — 建立 Chase-Lev seq_cst oracle 与 portable memory order

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-066)
Milestone: v0.3.0
Blocked by: AST-022, AST-023
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-066 [primary] — Chase-Lev 以 oracle 验证固定 portable memory order；source: D-097, D-098

## What to build

先实现可比对的 seq_cst oracle，再实现固定 acquire/release/fence/CAS ordering 的 owner/thief 算法。

## Invariants

- `[R-066]` v0.3必须先保留行为等价seq_cst oracle；production使用uint64 atomic top/bottom、atomic active-buffer和relaxed atomic Task cells：push为relaxed bottom、acquire top、relaxed cell、release fence、relaxed bottom publication；pop为relaxed bottom decrement/store、seq_cst fence、relaxed top，多元素relaxed cell，last-item用seq_cst strong top CAS(failure relaxed)并恢复canonical bottom；steal为acquire top、seq_cst fence、acquire bottom/buffer、relaxed cell、seq_cst strong top CAS(failure relaxed)，成功后才使用cell；resize release-store active-buffer。任何弱化都需新决策，memory_order_consume不得使用。 例外边界：R-101报告的Locked fallback不声称ChaseLevLockFree。

## Test-first seam

- Public seam: owner bottom push/pop与thief top steal。
- RED evidence: 先写 oracle differential、last-item race、owner pop 与多 thief steal 的 stress/TSAN 测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-066]` oracle/production通过同一functional stress，native AArch64验证weak-memory路径。

## Out of scope

- 不引入 DAG、Coroutine、Priority/Deadline 或未批准的动态 backend 切换。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-066
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-097, D-098
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


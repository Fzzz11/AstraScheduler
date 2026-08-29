# AST-051 — 生成原始 Benchmark Artifact 与受限 regression gate

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-091)
Milestone: v0.8.0
Blocked by: AST-043, AST-047, AST-050
Status: done
Claimed by: agent

## Rules and decisions

- R-091 [primary] — Benchmark artifact 保存原始重复并限定回归gate；source: D-143

## What to build

保存环境、构建、case schema、全部 repetition、统计摘要和 invalid 诊断；只对批准的稳定场景/噪声阈值启用 gate。

## Invariants

- `[R-091]` Standard profile必须默认2秒warmup、10个至少1秒独立repetition且不删outlier；versioned artifact保存全部raw values、median/MAD/p10/p90/bootstrap95%CI、环境/构建/options/seed/checksum/schema；共享PR CI只smoke，正式regression只在专用稳定runner同时越过versioned实践阈值与置信区间，baseline更新需review。 例外边界：非Standard exploratory profile必须在artifact显式命名参数。

## Test-first seam

- Public seam: performance claim、release evidence与regression automation。
- RED evidence: 先写 artifact schema golden、原始样本保留、统计重算、环境 mismatch 和 flaky/noisy case 不误判测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-091]` 性能结论可追溯原始重复，偶发共享runner噪声不阻断发布。

## Out of scope

- 不以单次最好成绩、不可重现截图或未经批准的外部实现对比作为性能结论。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-091
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-143
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


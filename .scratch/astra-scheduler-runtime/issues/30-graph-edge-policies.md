# AST-030 — 实现 void 控制图与两类 Edge policy

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-071)
Milestone: v0.4.0
Blocked by: AST-029
Status: done
Claimed by: Antigravity Agent

## Rules and decisions

- R-071 [primary] — DAG 是 void 控制图并区分两种 Edge policy；source: D-108, D-109, D-110

## What to build

固定 DAG node 为 void 控制任务；实现 required-success 与 completion-only edge，对失败/取消仅传播到 required descendants。

## Invariants

- `[R-071]` 普通Graph Node必须是返回void的one-shot控制任务且不提供per-node TaskHandle/隐式typed dataflow；Edge仅为RequireSuccess或AfterCompletion，Failed/Cancelled predecessor只把RequireSuccess descendants传播Cancelled，independent branch与AfterCompletion continuation继续。 例外边界：R-077允许显式Task<void> Coroutine Node。

## Test-first seam

- Public seam: TaskGraph node body、edge与failure propagation。
- RED evidence: 用菱形、多父节点、混合 edge、异常与取消矩阵验证允许执行和自动取消集合。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-071]` dependency failure不伪装为descendant failure，cleanup continuation仍运行。

## Out of scope

- 不实现可复用 mutable graph template、数据流值传播或 Coroutine node 行为。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-071
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-108, D-109, D-110
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification:
  - In-tree unit tests: `tests/test_graph_edge_policies.cpp` (28/28 ctest passed in debug and ASan/UBSan/LSan)
  - Package consumer gates: `tools/check_cmake_package.py` (AST030GraphEdgePoliciesGates, 45/45 tests passed)
  - Release gates: `tools/check_release_gates.py` (15/15 tests passed)


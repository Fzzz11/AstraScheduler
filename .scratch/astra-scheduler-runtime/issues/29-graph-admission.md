# AST-029 — 实现 GraphRun 原子 admission 与依赖发布

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-070)
Milestone: v0.4.0
Blocked by: AST-010, AST-022, AST-028
Status: done
Claimed by: Antigravity Agent

## Rules and decisions

- R-070 [primary] — Graph admission 原子核算全部 Node并按完成发布依赖；source: D-106, D-107

## What to build

一次性核算全部 Node 的资源/容量，失败全回滚；成功后 roots Ready，依赖只由 predecessor Terminal publication 推进。

## Invariants

- `[R-070]` External `run(FrozenTaskGraph&&)`必须all-or-nothing为每Node占External slot并计outstanding，过大图立即CapacityExhausted，Internal图豁免slot；Node完成先发布Terminal，再对每edge exactly-once decrement，唯一1→0 owner acquire汇合后恰好一次Ready或传播Terminal。 例外边界：empty graph占0 slot并立即完成。

## Test-first seam

- Public seam: GraphRun admission、root publication与successor release。
- RED evidence: 注入第 N 个 Node admission 失败，验证无部分可见 GraphRun；覆盖多 predecessor 最后完成竞态。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-070]` 图不会部分接受，successor不会早启、重复Ready或永久漏release。

## Out of scope

- 不实现可复用 mutable graph template、数据流值传播或 Coroutine node 行为。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-070
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-106, D-107
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification:
  - In-tree unit tests: `tests/test_graph_admission.cpp` (27/27 ctest passing in debug and ASan/UBSan/LSan)
  - Package consumer gates: `tools/check_cmake_package.py` (AST029GraphAdmissionGates, 44/44 tests passed)
  - Release gates: `tools/check_release_gates.py` (15/15 tests passed)


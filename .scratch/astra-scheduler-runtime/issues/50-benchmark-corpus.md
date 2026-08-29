# AST-050 — 固定 benchmark corpus 并保存 Global baseline

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-003, R-090)
Milestone: v0.8.0
Blocked by: AST-008, AST-023, AST-027, AST-031, AST-037, AST-041, AST-049
Status: done
Claimed by: agent

## Rules and decisions

- R-003 [primary] — 保留 v0.1.0 可运行基线；source: D-001
- R-090 [primary] — Benchmark corpus 使用语义基线和受限外部背景；source: D-142, D-150

## What to build

建立 micro/CPU/imbalanced/fork-join/DAG/coroutine/timer/priority/deadline corpus，保留 v0.1 Global baseline；外部实现仅作受限背景对比。

## Invariants

- `[R-003]` 后续 Work-Stealing 版本发布后，v0.1.0 Global Queue Scheduler 必须仍可运行并作为 Benchmark 对照组。
- `[R-090]` corpus必须覆盖Global FIFO、locked Work-Stealing、Chase-Lev及micro/CPU/imbalanced/fork-join/DAG/Coroutine/timer/Priority/Deadline/shutdown/reaper组合；Global FIFO是primary correctness/regression baseline，oneTBB可选，`std::async(std::launch::async)`只用于粗粒度独立背景且不得参与递归/DAG等feature ranking。 例外边界：外部adapter缺少等价语义的case应标not comparable。

## Test-first seam

- Public seam: v0.1.0 之后的 Benchmark Framework。；fixed workload corpus与adapter比较。
- RED evidence: 先为每 case 写 correctness checksum、工作量等价和 baseline 可加载/版本不匹配测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-003]` Benchmark 可在同一工作负载下运行 Global Queue 基线与后续 Scheduler。
- [x] `[R-090]` artifact明确adapter限制，不把线程拓扑不同的std::async当主回归oracle。

## Out of scope

- 不以单次最好成绩、不可重现截图或未经批准的外部实现对比作为性能结论。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-003, R-090
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-001, D-142, D-150
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: `benchmarks/`（新增）：bench_corpus.hpp（确定性 cpu_kernel、固定 seed=0x4153545244303530、worker matrix 1/2/4/..≤hw、三类 adapter：AstraScheduler/in-tree Global FIFO baseline/std::async 粗粒度背景、9 个 corpus case：micro_empty/cpu_calibrated/imbalanced/fork_join/dag_chain/coroutine_storm/timer_storm/priority_fairness/deadline_edf、baseline 保存/加载带版本校验）、astra_bench_corpus_selftest（8 用例：跨 variant checksum 等价、fork-join 等价+std::async not comparable、语义 case 对 baseline not comparable、语义 case 确定性、worker matrix、baseline 存取、版本/schema 不匹配显式失败、固定 seed）、astra_bench_corpus runner（JSON artifact 含 seed/shape/版本/adapter 限制 + --save-baseline 生成 benchmarks/baselines/global-baseline.json）。oneTBB 可选 adapter 缺失记录为 unavailable（不使 benchmark 失败）。全量 Debug 47/47 与 ASan 47/47、gates 15/15、traceability 通过。依赖修复：corpus 揭露的既有协程帧生命周期缺陷（AST-056 hotfix）。


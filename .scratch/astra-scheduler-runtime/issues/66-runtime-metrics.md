# AST-066 — 抽出拥有分片与快照的 RuntimeMetrics

Parent: [AstraScheduler Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-124)
Milestone: v1.2.0
Blocked by: AST-065
Status: done
Claimed by: agent

## Rules and decisions

- R-124 [primary] — RuntimeMetrics 拥有分片、record 与 snapshot 累加；source: D-176
- R-084 [supporting] — Off/Basic/Detailed 快照语义不变；source: D-135, D-136

## What to build

内部 `RuntimeMetrics` 拥有 worker/control shard、饱和计数与直方图累加。`metrics_snapshot()` 从该模块读取计数，生命周期 gauge 仍可由 Impl 提供。模块不得只持有 `Impl*` 转发。

## Invariants

- Off 为零开销投影；schema 与 saturation 语义不变。

## Test-first seam

- Public seam: runtime metrics 行为测试。
- RED evidence: RuntimeMetrics 以 Impl* 为唯一状态时审计失败。

## Acceptance criteria

- [x] `[R-124]` RuntimeMetrics 拥有 shard 状态，不含 Impl* 浅转发。
- [x] `[R-084]` runtime metrics 测试通过。

## Out of scope

- 不抽 ReadyQueues/steal/park。

## Traceability

- Spec: `.scratch/astra-scheduler-runtime/spec.md` — R-124
- Decisions: D-176
- Verification: WSL `build/wsl-gcc-debug`；`src/runtime/runtime_metrics.{hpp,cpp}` 拥有 worker/control shard 与 `fill_counters_and_histograms`；encapsulation 审计拒绝 `Scheduler::Impl*`；`astra_runtime_metrics_test` 与 `astra_metrics_snapshot_test` 通过；ctest 52/52。

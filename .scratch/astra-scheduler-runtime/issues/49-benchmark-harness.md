# AST-049 — 建立 micro harness 与独立 scenario runner

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-089)
Milestone: v0.8.0
Blocked by: AST-001, AST-002
Status: done
Claimed by: agent

## Rules and decisions

- R-089 [primary] — Benchmark 分为 micro harness 与独立场景runner；source: D-141

## What to build

分离 deque/queue/admission microbench 与端到端 Runtime scenario runner；每 case 固定 timed region、参数、checksum 和 primary metric。

## Invariants

- `[R-089]` Benchmark Framework必须用pinned Google Benchmark承载纯机制micro case，并用独立 `astra_bench_scenarios` 承载多阶段/生命周期case；setup、warmup、timed region、drain verification与teardown必须分离，checksum/rejection/drop/子进程异常使sample invalid而非产生性能值。 例外边界：consumer build默认不构建/下载benchmark依赖。

## Test-first seam

- Public seam: v0.8 benchmark targets与CI smoke。
- RED evidence: 先写 harness self-test，验证错误 checksum、空 repetition、异常 case 和 timed-region 污染会标记 invalid。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-089]` 计时区不混入构建/销毁，错误工作量不能被报告为更快。

## Out of scope

- 不以单次最好成绩、不可重现截图或未经批准的外部实现对比作为性能结论。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-089
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-141
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: `benchmarks/`（新增）：bench_harness.hpp 阶段协议（setup/warmup/timed/verify/teardown 分离、checksum/rejection/drop/异常→sample invalid）、`astra_bench_harness_selftest`（7 用例 RED evidence：checksum mismatch/empty repetition/timed_work_mismatch/三类异常/setup+teardown 不入计时区/warmup 不计数/rejection+drop 语义）、`astra_bench_scenarios`（submit_drain 场景 JSON artifact 3 samples valid + finalization 子进程隔离 case）、`astra_bench_micro`（pinned Google Benchmark v1.8.3，幂等下载+重试，4 个 micro case smoke 通过）。ASTRA_BUILD_BENCHMARKS 默认 OFF，consumer 不构建/下载依赖；Debug 46/46 与 gates 15/15、traceability 通过。注：本环境网络不稳定时 micro 目标自动跳过（scenario runner 不受影响）；Debug 构建的 micro 数字不可用于性能结论（D-141）。


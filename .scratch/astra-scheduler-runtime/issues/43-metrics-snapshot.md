# AST-043 — 实现 fuzzy Metrics Snapshot 与 Detailed log2 histogram

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-085)
Milestone: v0.7.0
Blocked by: AST-042
Status: ready-for-agent
Claimed by: None

## Rules and decisions

- R-085 [primary] — Metrics Snapshot fuzzy且Detailed使用固定log2 histogram；source: D-137

## What to build

聚合逐字段安全的不可变 snapshot，标注 capture 区间/fuzzy/saturated；Detailed 使用固定 log2 histogram 记录指定延迟。

## Invariants

- `[R-085]` `metrics_snapshot()`必须返回immutable、逐字段race-free的fuzzy snapshot并记录capture_started_at/capture_finished_at、level与saturated，运行中不声称全局单点一致；quiescent point满足R-084守恒。Detailed使用64个base-2纳秒bucket（0含0–1ns，后续[2^(n-1),2^n)，末桶吸收溢出）及饱和count/sum_ns/max_ns，固定记录ready_queue_wait、execution_segment、task_wall_time、blocking_admission_wait、timer_wake_lateness、deadline_start_lateness(仅miss)、worker_park_duration、runtime_join_latency；不存raw sample、不在Runtime算percentile、不stop-the-world或重置counter。 例外边界：Off返回明确disabled/empty schema而不读未初始化shard。

## Test-first seam

- Public seam: Runtime metrics读取与offline analysis。
- RED evidence: 先写并发 snapshot 合法区间、quiescent 精确值、bucket 边界、overflow saturation 和无 reset 行为。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [ ] `[R-085]` 并发snapshot字段各自有效，静止后accepted/outcome/steal等关系收敛。

## Out of scope

- 不让 Metrics/Trace 改变调度语义，不做在线 wait-for graph、后台 Trace 文件 I/O 或每 Task 默认日志。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-085
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-137
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: Pending


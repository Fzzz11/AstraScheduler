# AST-045 — 实现 bounded reusable TraceCollector

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-086)
Milestone: v0.7.0
Blocked by: AST-004, AST-042
Status: done
Claimed by: agent

## Rules and decisions

- R-086 [primary] — Trace capture 有界、可重复且显式提交；source: D-138, D-158, D-163

## What to build

显式 attach/capture/stop/submit 的共享 collector，固定容量、重复代际、drop 计数，Runtime 热路径不做文件 I/O。

## Invariants

- `[R-086]` 线程安全shared TraceCollector必须显式附加并一次只允许一代Recording；TraceOptions默认events_per_worker=16,384、external_control_events=65,536、events_per_reaper_producer=4,096，Default启用Task/queue/claim/steal-success/Wait/Await/Coroutine/Graph/Timer/Deadline/Runtime/Reaper而逐steal-attempt/Verbose关闭。三个容量须非零，未知bit/零值在状态改变前抛invalid_argument，总buffer算术溢出抛length_error，分配失败保留Stopped和上一snapshot并重抛bad_alloc；start_capture在Recording前完成全部producer预分配，Recording中新Scheduler附加失败则startup rollback。emit不得分配/I/O/callback/block且满时drop-newest计loss；只有move-only TraceCapture显式stop产生可复制immutable Snapshot，重复/并发stop共享结果，活动析构noexcept disable/quiesce并丢弃该代。 例外边界：未附加/Stopped/category disabled为fast no-op且不算drop。

## Test-first seam

- Public seam: 多Runtime/Reaper共享capture。
- RED evidence: 先写 disabled fast path、容量溢出、重复 capture、并发 producer、collector 生命周期不拥有 Runtime。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-086]` buffer overflow或异常展开不阻塞Scheduler，Collector可安全启动下一代。

## Out of scope

- 不让 Metrics/Trace 改变调度语义，不做在线 wait-for graph、后台 Trace 文件 I/O 或每 Task 默认日志。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-086
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-138, D-158, D-163
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: `tests/test_trace_collector.cpp`（8 用例：disabled fast path、选项校验/强异常安全、重复 capture、并发 producer 零丢失、drop-newest 计 loss、Capture move/幂等 stop/活动析构 abort、Recording 中附加 Scheduler 与溢出活性、Snapshot 独立存活）Debug 43/43 与 ASan 43/43 通过；`tools/check_release_gates.py` 15/15 通过；traceability 校验通过。注：bad_alloc 注入未直接验证（无注入 seam），由 length_error/校验先于状态改变覆盖强异常安全路径；`astra_coroutine_resume_handshake_test` 存在与本 Ticket 无关的既有间歇性终止（已在不含本 Ticket 改动的 HEAD 上复现）。


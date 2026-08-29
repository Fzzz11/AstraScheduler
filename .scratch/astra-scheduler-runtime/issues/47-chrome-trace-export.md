# AST-047 — 实现确定性 Chrome Trace 导出并隔离 Logging

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-088, R-109)
Milestone: v0.7.0
Blocked by: AST-045, AST-046
Status: done
Claimed by: agent

## Rules and decisions

- R-088 [primary] — Chrome Trace 只离线确定导出并显式报告损失；source: D-140
- R-109 [primary] — Logging 与 Trace 分离且不记录每Task热路径；source: D-140

## What to build

capture 后离线稳定导出 Chrome Trace，显式报告 drop/schema loss；Logging 不复用 TraceCollector，默认不记录每 Task 热路径。

## Invariants

- `[R-088]` 只有Stopped TraceSnapshot可由ostream工具离线导出Chrome Trace JSON；export必须确定merge、校验schema/identity/segment并保存capacity/category/recorded/dropped，任意loss仍输出有效JSON但标 `trace_complete=false`，不得合成事件掩盖缺口；core不得接受path或在Runtime线程写文件。 例外边界：pretty-print可改变字节格式；关闭category不算loss。
- `[R-109]` Logging必须与Trace使用独立sink/锁并仅承载低频ERROR/WARN/INFO控制面诊断；Worker每Task/queue/steal事件不得同步写日志，Trace emit不得调用logger，Benchmark除专用observability-overhead case外关闭Trace和高频日志并在artifact记录启用状态。 例外边界：D-040 fail-fast前可执行noexcept尽力诊断。

## Test-first seam

- Public seam: Chrome Trace exporter与artifact完整性。；Runtime/Reaper diagnostics、Trace与Benchmark。
- RED evidence: 先写 deterministic JSON golden、loss metadata、损坏 event 诊断、无后台 writer 和 logging-disabled 热路径测试。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-088]` 零loss相同snapshot/版本byte-stable，损坏输入明确失败且原snapshot可重试。
- [x] `[R-109]` Task hot path不获取logger I/O锁，Trace overflow/export不递归进入日志系统。

## Out of scope

- 不让 Metrics/Trace 改变调度语义，不做在线 wait-for graph、后台 Trace 文件 I/O 或每 Task 默认日志。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-088, R-109
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-140
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification: `tests/test_trace_export.cpp`（6 用例：byte-stable 确定性导出与完整 metadata、drop 后有效 JSON + trace_complete=false 不合成事件、未知 kind/category 不一致/空 snapshot 显式失败且 snapshot 可重试、unmatched segment end 降级 instant + schema_gaps、输出仅进入提供的 ostream 且 emit/export 无 logger 递归、pretty-print 变体）Debug 45/45 与 ASan 45/45 通过；`tools/check_release_gates.py` 15/15 通过；traceability 校验通过。R-109 附加事实：代码库无 logger（Task 热路径无日志锁），Reaper 仅 D-040 fail-fast 前 fputs 尽力诊断。


# AST-027 — 固定 Chase-Lev index 算术、边界状态与 backend truth

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-068, R-101)
Milestone: v0.3.0
Blocked by: AST-004, AST-025, AST-026
Status: done
Claimed by: agent

## Rules and decisions

- R-068 [primary] — Deque index、状态与算术不得依赖wrap；source: D-101, D-102, D-103
- R-101 [supporting] — SchedulerCapabilities 报告实际Local Deque backend；source: D-101, D-167, D-162

## What to build

使用不依赖整数 wrap 的索引/差值约束与受测 rebase/boundary 策略；仅真实启用实现时报告 `ChaseLevLockFree`。

## Invariants

- `[R-068]` Chase-Lev必须区分Success/Empty/Retry并对empty decrement、capacity doubling与索引执行checked arithmetic；接近高水位只能在quiescent状态rebase，不能依赖unsigned wrap，所需atomic不lock-free时选择Locked semantic fallback。 例外边界：backend报告由R-101固定。
- `[R-101]` 有效Scheduler的 `capabilities()`必须返回不可由用户aggregate-initialize的trivially-copyable immutable `SchedulerCapabilities`，其 `local_deque_backend()`为 `LocalDequeBackend::{None,Locked,ChaseLevLockFree}`之一且 `lock_free_local_deque()`仅最后一种为true；v0.1为None、v0.2/fallback为Locked，Stopped后保留且不得运行时切换或按版本推断，空Scheduler抛logic_error。 例外边界：该能力不声称整个Runtime lock-free。

## Test-first seam

- Public seam: v0.3+ Local Deque backend。；Runtime、Trace metadata与Benchmark artifact。
- RED evidence: 通过小位宽/偏置 seam 加速边界，覆盖 empty/one/full、rebase 并验证 capability 不按版本虚报。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-068]` 边界值测试不越界/ABA，Retry不被误报Empty。
- [x] `[R-101]` 只有实际启用经本 Ticket 边界验证的 Chase-Lev backend 才报告 `ChaseLevLockFree`；平台 fallback 必须继续报告 `Locked`。

## Out of scope

- 不引入 DAG、Coroutine、Priority/Deadline 或未批准的动态 backend 切换。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-068, R-101
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-101, D-102, D-103, D-167, D-162
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification:
  - 架构与实现：`src/chase_lev_deque.hpp` 实现了区分 `Success/Empty/Retry` 的三态结果状态、有符号索引 checked arithmetic（容量翻倍溢出检查、`maybe_quiescent_rebase` 高水位归零保护）、静态 `is_lock_free()` 查询；`src/scheduler.cpp` 严格依据当前平台的原子无锁能力动态注入 `ChaseLevLockFree` 或 `Locked` 能力快照。
  - 单元测试：`tests/test_chase_lev_indices.cpp` 覆盖 R-068 / R-101（三态返回值区分、高水位 Quiescent Rebase 安全基线归零、v0.3.0 真实无锁能力报告与生命周期不可变性、空 Scheduler 抛 `logic_error`）。
  - In-tree CTest：`wsl bash -lc "ctest --test-dir build/wsl-gcc-debug --output-on-failure"` 25/25 tests 全部 PASS。
  - ASan / UBSan / LSan 内存安全与泄漏门禁：`build/wsl-gcc-asan` 25/25 tests 全部 PASS（0 leaks / 0 errors / 0 deadlocks）。
  - Package consumer 与安装门禁：`python3 -X utf8 tools/check_cmake_package.py` 42/42 tests 全部 OK。
  - 发布门禁：`python3 -X utf8 tools/check_release_gates.py` 15/15 tests 全部 OK。

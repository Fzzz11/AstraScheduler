# AST-034 — 实现 Suspended cancellation 与 Immediate cooperative resume

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-075)
Milestone: v0.5.0
Blocked by: AST-016, AST-033
Status: done
Claimed by: Agent

## Rules and decisions

- R-075 [primary] — Suspended取消与 Immediate 只恢复已开始frame；source: D-119, D-154

## What to build

已开始 Suspended frame 收到 cancel/Immediate 时只通过 source Runtime 安排恢复以观察 stop；未开始 coroutine 仍可直接取消且不执行用户代码。

## Invariants

- `[R-075]` Suspended Coroutine收到取消时不得直接destroy或伪造Cancelled；内建cancellation-aware awaiter由stop winner撤销正常registration、发布source-Runtime Ready并在await_resume抛task_cancelled，foreign awaitable仅保留stop request且可永久挂起；Immediate禁止never-started frame首次start，但允许already-started resume segment运行到合作取消或自然完成。 例外边界：用户可捕获task_cancelled并继续，Runtime不保证有界终结。

## Test-first seam

- Public seam: Task/Graph/Shutdown/Finalization cancellation of Coroutine。
- RED evidence: 覆盖 suspended timer/await、cancel/trigger race、Immediate、忽略 stop 和 `task_cancelled` 退出。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-075]` frame不在suspend点被异步销毁，Immediate仍可执行必要unwind segment。

## Out of scope

- 不实现 I/O Runtime、Timer Wheel、inline foreign resume 或未批准的组合 API。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-075
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-119, D-154
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification:
  - In-tree Debug Tests: `tests/test_suspended_cancellation.cpp` (5/5 assertions, ctest 32/32 passed)
  - Address/Undefined/Leak Sanitizer: `build/wsl-gcc-asan` (ctest 32/32 passed, 0 leaks, 0 data races)
  - Independent Consumer Package Gates: `tools/check_cmake_package.py` (`AST034SuspendedCancellationGates`, 49/49 passed)
  - Release Milestone Gates: `tools/check_release_gates.py` (15/15 passed)


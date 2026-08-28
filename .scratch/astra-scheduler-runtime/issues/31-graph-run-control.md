# AST-031 — 实现 GraphRun cancel、完整报告与 caller-relative wait

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-072)
Milestone: v0.4.0
Blocked by: AST-012, AST-013, AST-030
Status: done
Claimed by: Antigravity Agent

## Rules and decisions

- R-072 [primary] — GraphRun 提供显式取消、完整报告与 caller-relative 等待；source: D-111, D-112, D-113, D-152

## What to build

GraphRun 提供显式取消、所有 Node Terminal 才完成的稳定 report，以及非 Worker/Helping wait 与 timeout。

## Invariants

- `[R-072]` copyable/movable GraphRun必须支持default/moved-from empty与valid()；invalid的id/state/wait/wait_for/get_report抛logic_error而request_cancel为no-op。有效GraphRun提供id/state/wait/wait_for/get_report/request_cancel，wait_for返回GraphWaitResult::{Completed,TimedOut}；全部Node Terminal后一次发布按NodeId排序的immutable GraphReport，含run_id、Node总数、Succeeded/Failed/Cancelled counts、每个真实Failed Node的NodeId/TaskId/exception_ptr与内部取消原因counts，状态优先Failed>Cancelled>Succeeded且空图Succeeded；get_report或co_await标记全部真实Node异常observed，wait/state不标记；同步等待复用R-052/R-056且own GraphRun wait拒绝。 例外边界：request_cancel对Running/Suspended Node沿各自cooperative规则且不等待完成。

## Test-first seam

- Public seam: 单次Graph execution观察与全图取消。
- RED evidence: 先写部分运行时 cancel、已终态 node 保持、完整 report 顺序、single-worker wait 和 timeout 不伪造完成。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-072]` 并发失败无任意first-error丢失，Graph aggregate不制造synthetic exception。

## Out of scope

- 不实现可复用 mutable graph template、数据流值传播或 Coroutine node 行为。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-072
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-111, D-112, D-113, D-152
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification:
  - In-tree unit tests: `tests/test_graph_run_control.cpp` (29/29 ctest passed in debug and ASan/UBSan/LSan)
  - Package consumer gates: `tools/check_cmake_package.py` (AST031GraphRunControlGates, 46/46 tests passed)
  - Release gates: `tools/check_release_gates.py` (15/15 tests passed)


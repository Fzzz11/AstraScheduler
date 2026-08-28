# AST-021 — 实现 Finalization escalation 与控制面 fail-fast

Parent: [AstraScheduler v0.1 → v1.0 Ticket Plan](../ticket-plan.md)
Spec: [AstraScheduler Runtime Spec](../spec.md) (approved; R-034, R-047)
Milestone: v0.1.0
Blocked by: AST-016, AST-019, AST-020
Status: done
Claimed by: agent

## Rules and decisions

- R-034 [primary] — 显式 Finalization Escalation 覆盖全部未完成 Runtime；source: D-029, D-039
- R-047 [primary] — Reaper 控制面不可恢复故障 fail-fast；source: D-040

## What to build

`request_immediate()` 单向覆盖全部已核算未完成 Runtime，且不伪造完成；Reaper 控制面不可恢复故障进入定义好的 fail-fast 路径。

## Invariants

- `[R-034]` `request_immediate()` 必须幂等地把同一核算集合内全部尚未达到 Shutdown Completion 的 Runtime 单向请求为 Immediate，包括 orphan 和 Starting Runtime；请求可靠记录并通知后返回，不得等待 Completion，已完成 Runtime 不得被改写。 例外边界：Running Task 仍只适用 R-009，因此升级不保证有界完成。
- `[R-047]` Reaper coordinator 顶层必须拦截全部逃逸异常；无法证明安全恢复的控制面故障必须先执行 `noexcept` 的尽力诊断再调用 `std::terminate()`，不得伪造 `Stopped`/`Finalized`/`TimedOut`、detach、泄漏后继续或重启 Reaper；用户 Callable 异常、合法 timeout 与永久 Pending 不得进入该 fatal path。 例外边界：Worker 启动前可正常报告的资源准备失败按 R-023 处理。

## Test-first seam

- Public seam: FinalizationControl 的显式进程级升级。；coordinator 逃逸异常、join ownership、handoff 所有权连续性及其他不可恢复不变量破坏。
- RED evidence: 先用可控 Runtime 集合验证升级覆盖、幂等、升级后继续 wait，以及 fault-injection 下的终止 handler 证据。
- 验证必须覆盖本 Ticket 的 primary 规则；supporting 规则只验证协作边界，不转移主实现责任。

## Acceptance criteria

- [x] `[R-034]` 没有 Scheduler Handle 的 Pending Runtime 也收到 Immediate 请求，Completed Runtime 历史不变。
- [x] `[R-047]` 子进程故障注入确定性进入 terminate；任务异常、TimedOut 和 Pending 场景保持正常域语义。

## Out of scope

- 不实现 Local Deque、Work Stealing、Chase-Lev、DAG、Coroutine、Priority/Deadline 或完整观测与 benchmark 功能。
- 未声明为 primary/supporting 的行为不在本 Ticket 内；可以建立最小私有 seam，但不得提前扩张 public API 或 observable semantics。
- 不实现被本 Ticket 阻塞的后续 Ticket；本 Ticket 只提供其明确依赖的可验证事实。

## Traceability

- Spec: [`.scratch/astra-scheduler-runtime/spec.md`](../spec.md) — R-034, R-047
- Decisions: [`.scratch/astra-scheduler-runtime/decision-log.md`](../decision-log.md) — D-029, D-039, D-040
- ADRs: [`docs/adr/`](../../../docs/adr/)；以以上规则和决策引用选择相关 accepted ADR。
- Verification:
  - 单元测试：`tests/test_finalization_escalation.cpp` 覆盖全部 R-034（活动与 orphan Runtime 的 Immediate 广播升级）和 R-047（子进程隔离下 coordinator 控制面异常触发 std::terminate）。
  - In-tree CTest：`wsl bash -lc "ctest --test-dir build/wsl-gcc-debug --output-on-failure"` 19/19 tests 全部 PASS。
  - Package consumer 与安装门禁：`python3 -X utf8 tools/check_cmake_package.py` 36/36 tests 全部 OK。
  - 发布门禁：`python3 -X utf8 tools/check_release_gates.py` 15/15 tests 全部 OK。

